#include "catch2/catch_all.hpp"

#include "common/LockFreeQueue.hpp"
#include "feed/FeedMessages.hpp"
#include "feed/MarketDataPublisher.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

using namespace cmf;

namespace
{
// Spin (bounded) until `pred` holds, so async-delivery tests don't hang on
// failure.  Returns the final value of pred().
template <typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds timeout =
                             std::chrono::seconds(5))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    return pred();
}
} // namespace

TEST_CASE("FeedMessage - trivially copyable and survives queue round-trip",
          "[Feed]")
{
    STATIC_REQUIRE(std::is_trivially_copyable_v<feed::FeedMessage>);

    LockFreeQueue<feed::FeedMessage, 16> q;

    // Update
    q.push(feed::FeedMessage::make(
        feed::BookUpdate{7, 1000, 42, Side::Sell, 12345, 99}));
    // Trade
    q.push(feed::FeedMessage::make(
        feed::Trade{8, 1100, 42, Side::Buy, 12350, 5}));
    // Snapshot
    feed::BookSnapshot snap{};
    snap.seq = 9;
    snap.ts_event = 1200;
    snap.instrument_id = 42;
    snap.bid_depth = 2;
    snap.ask_depth = 1;
    snap.bids[0] = feed::Level{500, 10};
    snap.bids[1] = feed::Level{499, 20};
    snap.asks[0] = feed::Level{501, 15};
    q.push(feed::FeedMessage::make(snap));

    feed::FeedMessage out;

    REQUIRE(q.pop([&](feed::FeedMessage&& m)
                  { out = m; }));
    REQUIRE(out.type == feed::FeedMsgType::Update);
    REQUIRE(out.update.seq == 7);
    REQUIRE(out.update.instrument_id == 42);
    REQUIRE(out.update.side == Side::Sell);
    REQUIRE(out.update.price == 12345);
    REQUIRE(out.update.new_qty == 99);
    REQUIRE(out.instrument_id() == 42);
    REQUIRE(out.seq() == 7);

    REQUIRE(q.pop([&](feed::FeedMessage&& m)
                  { out = m; }));
    REQUIRE(out.type == feed::FeedMsgType::Trade);
    REQUIRE(out.trade.aggressor == Side::Buy);
    REQUIRE(out.trade.price == 12350);
    REQUIRE(out.trade.size == 5);

    REQUIRE(q.pop([&](feed::FeedMessage&& m)
                  { out = m; }));
    REQUIRE(out.type == feed::FeedMsgType::Snapshot);
    REQUIRE(out.snapshot.bid_depth == 2);
    REQUIRE(out.snapshot.ask_depth == 1);
    REQUIRE(out.snapshot.bids[0].price == 500);
    REQUIRE(out.snapshot.bids[1].qty == 20);
    REQUIRE(out.snapshot.asks[0].price == 501);
}

TEST_CASE("FeedMessage - visit dispatches to the matching overload", "[Feed]")
{
    auto type_of = [](const feed::FeedMessage& m)
    {
        return feed::visit(
            m, [](const auto& x) -> feed::FeedMsgType
            {
                   using T = std::decay_t<decltype(x)>;
                   if constexpr (std::is_same_v<T, feed::BookUpdate>)
                       return feed::FeedMsgType::Update;
                   else if constexpr (std::is_same_v<T, feed::Trade>)
                       return feed::FeedMsgType::Trade;
                   else
                       return feed::FeedMsgType::Snapshot; });
    };

    REQUIRE(type_of(feed::FeedMessage::make(feed::BookUpdate{})) ==
            feed::FeedMsgType::Update);
    REQUIRE(type_of(feed::FeedMessage::make(feed::Trade{})) ==
            feed::FeedMsgType::Trade);
    REQUIRE(type_of(feed::FeedMessage::make(feed::BookSnapshot{})) ==
            feed::FeedMsgType::Snapshot);
}

TEST_CASE("MarketDataPublisher - per-instrument sequence numbers are "
          "independent",
          "[Feed]")
{
    feed::MarketDataPublisher pub;

    REQUIRE(pub.current_seq(1) == 0);
    REQUIRE(pub.next_seq(1) == 1);
    REQUIRE(pub.next_seq(1) == 2);
    REQUIRE(pub.next_seq(2) == 1); // independent counter for instrument 2
    REQUIRE(pub.current_seq(1) == 2);
    REQUIRE(pub.current_seq(2) == 1);
}

TEST_CASE("MarketDataPublisher - single subscriber sees contiguous, in-order "
          "sequence numbers with no drops",
          "[Feed]")
{
    feed::MarketDataPublisher pub;

    std::mutex m;
    std::vector<uint64_t> seqs;
    std::atomic<int> received{0};

    const feed::SubscriberHandle h = pub.subscribe(
        [&](const feed::FeedMessage& msg)
        {
            {
                std::lock_guard lock(m);
                seqs.push_back(msg.seq());
            }
            received.fetch_add(1, std::memory_order_release);
        });

    // N <= queue capacity - 1 so nothing can be dropped regardless of how the
    // worker is scheduled (the deterministic "no gap" guarantee).
    constexpr int N = 4000;
    for (int i = 0; i < N; ++i)
        pub.publishUpdate(feed::BookUpdate{0, i, 42, Side::Buy, 100,
                                           static_cast<uint64_t>(i)});

    REQUIRE(wait_for([&]
                     { return received.load() >= N; }));
    REQUIRE(pub.dropped(h) == 0);

    std::lock_guard lock(m);
    REQUIRE(seqs.size() == static_cast<std::size_t>(N));
    for (int i = 0; i < N; ++i)
        REQUIRE(seqs[static_cast<std::size_t>(i)] ==
                static_cast<uint64_t>(i + 1)); // seq starts at 1, contiguous
}

TEST_CASE("MarketDataPublisher - clean shutdown joins workers and is idempotent",
          "[Feed]")
{
    feed::MarketDataPublisher pub;
    std::atomic<int> count{0};

    (void)pub.subscribe([&](const feed::FeedMessage&)
                        { count.fetch_add(1, std::memory_order_relaxed); });

    constexpr int N = 100;
    for (int i = 0; i < N; ++i)
        pub.publishUpdate(feed::BookUpdate{0, i, 1, Side::Buy, 10,
                                           static_cast<uint64_t>(i)});

    // shutdown() closes queues and joins workers (drains remaining first); must
    // return without hanging.
    pub.shutdown();
    REQUIRE(count.load() == N); // all queued messages delivered before exit

    pub.shutdown(); // idempotent: no hang, no throw
    SUCCEED("second shutdown returned cleanly");
}

TEST_CASE("MarketDataPublisher - unsubscribe stops delivery", "[Feed]")
{
    feed::MarketDataPublisher pub;
    std::atomic<int> count{0};

    const feed::SubscriberHandle h = pub.subscribe(
        [&](const feed::FeedMessage&)
        { count.fetch_add(1, std::memory_order_relaxed); });

    pub.publishUpdate(feed::BookUpdate{0, 0, 1, Side::Buy, 10, 1});
    REQUIRE(wait_for([&]
                     { return count.load() >= 1; }));

    pub.unsubscribe(h);

    // After unsubscribe the subscriber is gone; publishing must not deliver or
    // crash.
    const int before = count.load();
    for (int i = 0; i < 100; ++i)
        pub.publishUpdate(feed::BookUpdate{0, i, 1, Side::Buy, 10, 1});

    REQUIRE(count.load() == before);
}
