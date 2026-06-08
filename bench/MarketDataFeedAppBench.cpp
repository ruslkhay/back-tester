#include "common/BlockingQueue.hpp"
#include "common/MarketDataEvent.hpp"
#include "feed/FeedMessages.hpp"
#include "feed/MarketDataPublisher.hpp"
#include "ingestion/FlatMerger.hpp"
#include "ingestion/IngestionPipeline.hpp"
#include "ingestion/JsonNativeDataParser.hpp"
#include "order_book/AbseilOrderBook.hpp"
#include "order_book/SimpleOrderBookRouter.hpp"
#include <benchmark/benchmark.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <thread>
#include <vector>

// Sub-real end-to-end scenario: replay ~20 raw .mbo.json files through the LOB
// router (previously developed) and feed every resulting book change to the
// market-data publisher with S subscriber worker threads. Reports pipeline
// throughput (events/s) and feed fan-out (delivered/s, drops), the same metrics
// as the micro-benchmarks.
//
//   BENCH_DIR    directory holding *.mbo.json files (required)
//   BENCH_FILES  cap on number of files ingested (optional; default 20)
namespace
{
using Router = cmf::SimpleOrderBookRouter<cmf::AbseilOrderBook>;
using parser_impl = cmf::JsonNativeDataParser;

cmf::feed::BookSnapshot build_snapshot(const Router& router, uint32_t instr)
{
    cmf::feed::BookSnapshot snap{};
    snap.instrument_id = instr;
    const auto* book = router.find_book(instr);
    if (book == nullptr)
        return snap;

    const auto bids = book->side_levels(cmf::Side::Buy);
    const std::size_t nb = std::min(bids.size(), cmf::feed::kSnapshotDepth);
    for (std::size_t i = 0; i < nb; ++i)
        snap.bids[i] = {bids[i].first, static_cast<uint64_t>(bids[i].second)};
    snap.bid_depth = static_cast<uint16_t>(nb);

    const auto asks = book->side_levels(cmf::Side::Sell);
    const std::size_t na = std::min(asks.size(), cmf::feed::kSnapshotDepth);
    for (std::size_t i = 0; i < na; ++i)
        snap.asks[i] = {asks[i].first, static_cast<uint64_t>(asks[i].second)};
    snap.ask_depth = static_cast<uint16_t>(na);
    return snap;
}

void publish_feed(Router& router, cmf::feed::MarketDataPublisher& pub,
                  const cmf::MarketDataEvent& e)
{
    const uint32_t instr = router.resolved_instrument(e);
    if (instr == 0)
        return;
    switch (e.action)
    {
    case cmf::Action::Add:
    case cmf::Action::Modify:
    case cmf::Action::Cancel:
    {
        if (e.side == cmf::Side::None || !e.is_price_defined())
            break;
        const auto* book = router.find_book(instr);
        const uint64_t new_qty = book ? book->volume_at(e.side, e.price) : 0;
        pub.publishUpdate(
            {0, e.ts_event, instr, e.side, e.price, new_qty});
        break;
    }
    case cmf::Action::Trade:
    case cmf::Action::Fill:
        pub.publishTrade({0, e.ts_event, instr, e.side, e.price, e.size});
        break;
    case cmf::Action::Clear:
    {
        cmf::feed::BookSnapshot snap = build_snapshot(router, instr);
        snap.ts_event = e.ts_event;
        pub.publishSnapshot(snap);
        break;
    }
    default:
        break;
    }
}

std::vector<std::filesystem::path> gather_files()
{
    std::vector<std::filesystem::path> files;
    const char* dir = std::getenv("BENCH_DIR");
    if (!dir || !std::filesystem::is_directory(dir))
        return files;
    for (const auto& e : std::filesystem::recursive_directory_iterator(dir))
        if (e.is_regular_file() &&
            e.path().string().ends_with(parser_impl::filename_ext))
            files.push_back(e.path());
    std::sort(files.begin(), files.end());

    std::size_t cap = 20;
    if (const char* n = std::getenv("BENCH_FILES"))
        cap = static_cast<std::size_t>(std::atoll(n));
    if (files.size() > cap)
        files.resize(cap);
    return files;
}
} // namespace

template <int Subscribers>
static void BM_FeedApp(benchmark::State& state)
{
    const auto files = gather_files();
    if (files.empty())
    {
        state.SkipWithError("BENCH_DIR unset/empty or no .mbo.json files.");
        return;
    }
    const std::size_t N = files.size();

    int64_t events = 0;
    std::atomic<int64_t> delivered{0};
    uint64_t dropped = 0;

    for (auto _ : state)
    {
        std::deque<cmf::BlockingQueue<cmf::MarketDataEvent>> file_queues(N);
        cmf::BlockingQueue<cmf::MarketDataEvent> merged_queue;
        cmf::FlatMerger<cmf::BlockingQueue, cmf::BlockingQueue> merger(
            file_queues, merged_queue);
        std::thread merger_thread([&]
                                  { merger.run_impl(); });

        std::vector<std::thread> producers;
        producers.reserve(N);
        for (std::size_t i = 0; i < N; ++i)
            producers.emplace_back(
                [&file_queues, &files, i]
                {
                    auto push = [&file_queues, i](const cmf::MarketDataEvent& e)
                    { file_queues[i].push(e); };
                    cmf::IngestionPipeline<parser_impl, decltype(push)> pipeline(
                        files[i], push);
                    pipeline.ingest();
                    file_queues[i].close();
                });

        Router router;
        cmf::feed::MarketDataPublisher pub;
        std::vector<cmf::feed::SubscriberHandle> subs;
        for (int s = 0; s < Subscribers; ++s)
            subs.push_back(pub.subscribe(
                [&delivered](const cmf::feed::FeedMessage&)
                { delivered.fetch_add(1, std::memory_order_relaxed); }));

        int64_t count = 0;
        while (merged_queue.pop(
            [&](cmf::MarketDataEvent&& e)
            {
                ++count;
                router.apply(e);
                if constexpr (Subscribers > 0)
                    publish_feed(router, pub, e);
            }))
            ;

        for (auto& t : producers)
            t.join();
        merger_thread.join();
        pub.shutdown(); // drain + join feed workers before counting

        for (const auto& h : subs)
            dropped += pub.dropped(h);
        events = count;
        benchmark::DoNotOptimize(count);
    }

    state.SetItemsProcessed(state.iterations() * events);
    state.counters["events"] = static_cast<double>(events);
    state.counters["feed_delivered_per_s"] = benchmark::Counter(
        static_cast<double>(delivered.load()), benchmark::Counter::kIsRate);
    state.counters["feed_dropped"] = static_cast<double>(dropped);
}

#define REGISTER_FEED_APP(Subs, Label)   \
    BENCHMARK_TEMPLATE(BM_FeedApp, Subs) \
        ->Name(Label)                    \
        ->Unit(benchmark::kMillisecond)  \
        ->Iterations(1)                  \
        ->Repetitions(1)                 \
        ->UseRealTime()

REGISTER_FEED_APP(0, "FeedApp/baseline_no_feed");
REGISTER_FEED_APP(1, "FeedApp/1_subscriber");
REGISTER_FEED_APP(4, "FeedApp/4_subscribers");
REGISTER_FEED_APP(8, "FeedApp/8_subscribers");
