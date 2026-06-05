#include "args.hpp"
#include "common/BlockingQueue.hpp"
#include "common/LockFreeQueue.hpp"
#include "common/NaiveTimer.hpp"
#include "ingestion/FeatherDataParser.hpp"
#include "ingestion/FlatMerger.hpp"
#include "ingestion/IngestionPipeline.hpp"

#include <cinttypes>
#include <cstdio>
#include <deque>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "common/MarketDataEvent.hpp"
#include "feed/MarketDataPublisher.hpp"
#include "order_book/AbseilOrderBook.hpp"
#include "order_book/BookAnomalyLog.hpp"
#include "order_book/SimpleOrderBookRouter.hpp"

#include <atomic>

#define PROCESS_MARKET_DATA_EVENT_MODE 1 // 1 = COUNT mode, 2 = PRINT mode

// Market-data feed (HW3 Group 2, inbound half): 1 = derive feed messages after
// each event and publish them to subscribers; 0 = disabled (original behavior,
// no feed threads, zero overhead).
#define ENABLE_MARKET_DATA_FEED 1

using namespace cmf;

constexpr uint64_t REPORT_AFTER_EACH_N_EVENTS = 5'000'000;

template <typename T, template <typename> typename QImpl,
          std::size_t BatchSize = 256>
struct BatchPusher
{
    QImpl<T>& queue;
    std::array<T, BatchSize> batch = {};
    std::size_t count = 0;

    explicit BatchPusher(QImpl<T>& q) : queue(q) {}

    void push(T item)
    {
        batch[count++] = std::move(item);
        if (count >= BatchSize)
            flush();
    }

    void flush()
    {
        if (count > 0 && !queue.is_closed())
        {
            queue.push_batch(batch.data(), count);
            count = 0;
        }
    }

    ~BatchPusher() { flush(); }
};

struct ProcessMarketDataEvent
{
    ProcessMarketDataEvent() : counter_(0)
    {
#if ENABLE_MARKET_DATA_FEED
        // Phase B: a single demo subscriber (all instruments) that just counts
        // delivered messages, to prove end-to-end fan-out and measure overhead.
        demo_sub_ = publisher_.subscribe(
            [this](const feed::FeedMessage&)
            { feed_count_.fetch_add(1, std::memory_order_relaxed); });
#endif
    }

    BlockingQueue<std::string> print_queue;

    void operator()(const MarketDataEvent& e)
    {
        order_book_router_.apply(e);

#if ENABLE_MARKET_DATA_FEED
        publish_feed(e);
#endif

#if PROCESS_MARKET_DATA_EVENT_MODE == 1
        if (e.ts_recv > 0)
        {
            const std::uint64_t count =
                counter_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count % REPORT_AFTER_EACH_N_EVENTS == 0)
            {
                std::ostringstream oss;
                oss << "\n=== Snapshot at " << count << " events ===\n"
                    << order_book_router_.snapshot_as_string(5)
                    << "===============================\n\n";
                print_queue.push(std::move(oss).str());
            }
        }
#endif

#if PROCESS_MARKET_DATA_EVENT_MODE == 2
        char buf[256];
        std::snprintf(
            buf, sizeof(buf),
            "ts_recv=%lld ts_event=%lld order_id=%llu side=%d price=%" PRId64
            " size=%u action=%d\n",
            e.ts_recv, e.ts_event, e.order_id, static_cast<int>(e.side), e.price,
            e.size, static_cast<int>(e.action));
        print_queue.push(std::string(buf));
#endif
    }

    void summary(double elapsed_seconds) const
    {
        const std::uint64_t count = counter_.load(std::memory_order_acquire);
        const double throughput = elapsed_seconds > 0.0
                                      ? static_cast<double>(count) / elapsed_seconds
                                      : 0.0;
        std::printf("Total messages processed : %" PRIu64 "\n", count);
        std::printf("Wall-clock time          : %.3f s\n", elapsed_seconds);
        std::printf("Throughput               : %.0f msg/s\n", throughput);
    }

    void print_best_bid_ask(std::ostream& cout) const
    {
        order_book_router_.print_best_bid_ask(cout);
    }

#if ENABLE_MARKET_DATA_FEED
    void print_feed_summary() const
    {
        std::printf("Feed messages delivered  : %" PRIu64 "\n",
                    feed_count_.load(std::memory_order_acquire));
        std::printf("Feed messages dropped    : %" PRIu64 "\n",
                    publisher_.dropped(demo_sub_));
    }

    // Close subscriber queues and join worker threads.  Call after the event
    // stream ends so no publish races a close.
    void shutdown_feed() { publisher_.shutdown(); }
#endif

  private:
#if ENABLE_MARKET_DATA_FEED
    // Derive feed messages from a just-applied event and publish them.  Runs on
    // the dispatcher thread (single producer), so reading book state is safe.
    void publish_feed(const MarketDataEvent& e)
    {
        const uint32_t instr = order_book_router_.resolved_instrument(e);
        if (instr == 0)
            return;

        switch (e.action)
        {
        case Action::Add:
        case Action::Modify:
        case Action::Cancel:
        {
            // A cancel may carry no side / undefined price; skip what we can't
            // attribute to a single level.
            if (e.side == Side::None || !e.is_price_defined())
                break;
            const auto* book = order_book_router_.find_book(instr);
            const uint64_t new_qty = book ? book->volume_at(e.side, e.price) : 0;
            publisher_.publishUpdate(feed::BookUpdate{0, e.ts_event, instr,
                                                      e.side, e.price, new_qty});
            break;
        }
        case Action::Trade:
        case Action::Fill:
            publisher_.publishTrade(
                feed::Trade{0, e.ts_event, instr, e.side, e.price, e.size});
            break;
        case Action::Clear:
            // Phase C: publish a fresh BookSnapshot here to resync subscribers.
            break;
        default:
            break;
        }
    }
#endif

    mutable std::atomic_ullong counter_;
#if ENABLE_MARKET_DATA_FEED
    std::atomic_uint64_t feed_count_{0};
    feed::SubscriberHandle demo_sub_;
#endif
    cmf::SimpleOrderBookRouter<cmf::AbseilOrderBook> order_book_router_;
#if ENABLE_MARKET_DATA_FEED
    // Declared last -> destroyed first, so worker threads are joined while the
    // counters/router they reference are still alive.
    feed::MarketDataPublisher publisher_;
#endif
};

int main([[maybe_unused]] int argc, [[maybe_unused]] const char* argv[])
{
    try
    {
        using parser_impl = cmf::FeatherDataParser;
        const Config cfg =
            parse_args(std::span(argv, argc), parser_impl::filename_ext);
        const std::size_t data_files_count = cfg.data_files.size();

        // Open predefined anomaly log files (logs/order-book-anomalies.log, etc.).
        [[maybe_unused]] auto& anomaly_log = BookAnomalyLog::instance();

        NaiveTimer timer;
        std::deque<BlockingQueue<MarketDataEvent>> file_queues;
        for (std::size_t i = 0; i < data_files_count; ++i)
            file_queues.emplace_back();

        BlockingQueue<MarketDataEvent> merged_queue;
        const FlatMerger<BlockingQueue, BlockingQueue> merger(file_queues,
                                                              merged_queue);

        ProcessMarketDataEvent sink;
        std::thread io_thread([&sink]()
                              {
      while (sink.print_queue.pop(
          [](std::string &&msg) { std::fputs(msg.c_str(), stdout); })) {
      } });
        std::thread merger_thread([&]()
                                  { merger.run_impl(); });
        std::thread dispatcher_thread([&]()
                                      {
      while (merged_queue.pop([&](MarketDataEvent &&e) { sink(e); }))
        ;

#if PROCESS_MARKET_DATA_EVENT_MODE == 1
      sink.summary(timer.elapsed_seconds());
#endif

#if ENABLE_MARKET_DATA_FEED
      sink.print_feed_summary();
      sink.shutdown_feed();
#endif

      sink.print_best_bid_ask(std::cout);
      BookAnomalyLog::instance().write_summary(std::cerr);
      BookAnomalyLog::instance().flush();
      sink.print_queue.close(); });

        std::vector<std::thread> producers;
        producers.reserve(data_files_count);
        for (std::size_t i = 0; i < data_files_count; ++i)
        {
            producers.emplace_back([&file_queues, &cfg, i]()
                                   {
        BatchPusher batcher{file_queues[i]};
        auto push_fn = [&batcher](const MarketDataEvent &e) {
          batcher.push(e);
        };
        IngestionPipeline<parser_impl, decltype(push_fn)> pipeline(
            cfg.data_files[i], push_fn);
        pipeline.ingest();
        // Flush the final partial batch BEFORE closing: BatchPusher::flush()
        // skips closed queues, so closing first would silently drop the last
        // (< BatchSize) events of every file.
        batcher.flush();
        file_queues[i].close(); });
        }

        for (auto& t : producers)
            t.join();
        merger_thread.join();
        dispatcher_thread.join();
        io_thread.join();
    }
    catch (std::exception& ex)
    {
        std::cerr << "Back-tester threw an exception: " << ex.what() << std::endl;
        return 1;
    }

    return 0;
}
