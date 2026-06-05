#include "common/BasicTypes.hpp"
#include "feed/FeedMessages.hpp"
#include "feed/MarketDataPublisher.hpp"
#include <benchmark/benchmark.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_set>

namespace
{

using cmf::Side;
using cmf::feed::BookUpdate;
using cmf::feed::FeedMessage;
using cmf::feed::MarketDataPublisher;

// One counter per subscriber, each on its own cache line, so the worker threads
// don't false-share while counting deliveries (keeps the bench measuring the
// feed, not counter contention).
struct alignas(64) PaddedCounter
{
    std::atomic<int64_t> v{0};
};

BookUpdate make_update(int64_t i, uint32_t instrument)
{
    return BookUpdate{0, i, instrument, Side::Buy, 100, 1};
}

} // namespace

// Fan-out throughput: one producer (this thread) publishes N updates per
// iteration; `Subscribers` worker threads (owned by the publisher) drain in
// parallel.  The timed region covers publish + full delivery to every
// subscriber.  N is kept below the queue capacity so nothing is dropped and the
// drain always completes.
template <int Subscribers>
static void BM_Feed_Throughput(benchmark::State& state)
{
    const int64_t N = state.range(0);
    constexpr uint32_t kInstr = 1;

    std::array<PaddedCounter, Subscribers> counts;
    auto pub = std::make_unique<MarketDataPublisher>();
    for (int s = 0; s < Subscribers; ++s)
        (void)pub->subscribe(
            [&counts, s](const FeedMessage&)
            { counts[s].v.fetch_add(1, std::memory_order_relaxed); },
            {kInstr});

    for (auto _ : state)
    {
        std::array<int64_t, Subscribers> target;
        for (int s = 0; s < Subscribers; ++s)
            target[s] = counts[s].v.load(std::memory_order_relaxed) + N;

        for (int64_t i = 0; i < N; ++i)
            pub->publishUpdate(make_update(i, kInstr));

        // Wait until every subscriber has drained this iteration's batch.
        for (int s = 0; s < Subscribers; ++s)
            while (counts[s].v.load(std::memory_order_acquire) < target[s])
                benchmark::DoNotOptimize(counts[s].v.load());
    }

    state.SetItemsProcessed(state.iterations() * N * Subscribers);
    pub->shutdown();
}

// Cost of the per-message instrument filter: one subscriber whose filter holds
// `FilterSize` instruments; every published update matches, so all are
// delivered and the wants() set lookup is on the hot path.
template <int FilterSize>
static void BM_Feed_FilterCost(benchmark::State& state)
{
    const int64_t N = state.range(0);
    constexpr uint32_t kInstr = 1; // always present in the filter set

    std::unordered_set<uint32_t> filter;
    for (int i = 0; i < FilterSize; ++i)
        filter.insert(static_cast<uint32_t>(i + 1));

    PaddedCounter delivered;
    auto pub = std::make_unique<MarketDataPublisher>();
    (void)pub->subscribe(
        [&delivered](const FeedMessage&)
        { delivered.v.fetch_add(1, std::memory_order_relaxed); },
        filter);

    for (auto _ : state)
    {
        const int64_t target = delivered.v.load(std::memory_order_relaxed) + N;
        for (int64_t i = 0; i < N; ++i)
            pub->publishUpdate(make_update(i, kInstr));
        while (delivered.v.load(std::memory_order_acquire) < target)
            benchmark::DoNotOptimize(delivered.v.load());
    }

    state.SetItemsProcessed(state.iterations() * N);
    pub->shutdown();
}

// 1 << 12 = 4096 < kSubscriberQueueCap (8192): the whole batch fits, so no drops
// and the per-iteration drain always completes.
#define REGISTER_FEED_BENCH(BenchFn, Param, Label) \
    BENCHMARK_TEMPLATE(BenchFn, Param)             \
        ->Name(Label)                              \
        ->Arg(1 << 12)                             \
        ->Unit(benchmark::kMicrosecond)            \
        ->MinWarmUpTime(0.5)                       \
        ->MinTime(1.0)                             \
        ->Repetitions(5)                           \
        ->DisplayAggregatesOnly()                  \
        ->UseRealTime()

REGISTER_FEED_BENCH(BM_Feed_Throughput, 1, "Feed/Throughput/1_subscriber");
REGISTER_FEED_BENCH(BM_Feed_Throughput, 2, "Feed/Throughput/2_subscribers");
REGISTER_FEED_BENCH(BM_Feed_Throughput, 4, "Feed/Throughput/4_subscribers");
REGISTER_FEED_BENCH(BM_Feed_Throughput, 8, "Feed/Throughput/8_subscribers");
REGISTER_FEED_BENCH(BM_Feed_FilterCost, 1, "Feed/FilterCost/1_instrument");
REGISTER_FEED_BENCH(BM_Feed_FilterCost, 10, "Feed/FilterCost/10_instruments");
