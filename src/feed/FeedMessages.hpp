#pragma once

#include "common/BasicTypes.hpp"

#include <array>
#include <cstdint>
#include <type_traits>

// Market-data feed protocol messages (HW3 Group 2, inbound half).
//
// These sit one level above MarketDataEvent: they describe *book-state changes*
// (deltas, snapshots, trades) that the publisher fans out to trading engines.
//
// Hard rule: every message must be a flat POD so it lives directly in a
// LockFreeQueue ring slot with NO heap allocation on the publish path.  Fixed
// std::array (never std::vector/std::string) and value semantics everywhere.
namespace cmf::feed
{

// Number of price levels carried in a BookSnapshot per side.
inline constexpr std::size_t kSnapshotDepth = 10;

// One aggregated price level: (price, total resting quantity at that price).
struct Level
{
    ScaledPrice price = 0;
    uint64_t qty = 0;
};
static_assert(std::is_trivially_copyable_v<Level>);

// Incremental update: the aggregated size at one (instrument, side, price)
// level changed.  new_qty == 0 means the level was removed.
struct BookUpdate
{
    uint64_t seq = 0; // per-instrument monotonic sequence
    NanoTime ts_event = 0;
    uint32_t instrument_id = 0;
    Side side = Side::None;
    ScaledPrice price = 0;
    uint64_t new_qty = 0;
};
static_assert(std::is_trivially_copyable_v<BookUpdate>);

// Self-contained top-N snapshot for one instrument.  Sent on subscribe and on
// Clear so a subscriber can (re)bootstrap its local view without prior state.
struct BookSnapshot
{
    uint64_t seq = 0; // seq this snapshot is consistent with; subscriber
                      // resets its expected-next to seq + 1
    NanoTime ts_event = 0;
    uint32_t instrument_id = 0;
    uint16_t bid_depth = 0;                   // # valid entries in bids[]
    uint16_t ask_depth = 0;                   // # valid entries in asks[]
    std::array<Level, kSnapshotDepth> bids{}; // index 0 = best bid (descending)
    std::array<Level, kSnapshotDepth> asks{}; // index 0 = best ask (ascending)
};
static_assert(std::is_trivially_copyable_v<BookSnapshot>);

// A trade executed.
struct Trade
{
    uint64_t seq = 0;
    NanoTime ts_event = 0;
    uint32_t instrument_id = 0;
    Side aggressor = Side::None; // side that took liquidity
    ScaledPrice price = 0;
    uint64_t size = 0;
};
static_assert(std::is_trivially_copyable_v<Trade>);

enum class FeedMsgType : uint8_t
{
    Update = 0,
    Snapshot = 1,
    Trade = 2,
};

// Tagged union (NOT std::variant): a plain trivially-copyable POD so the queue
// slot is fixed-size with zero heap.  Ordering across types is preserved
// because all three share one queue.  Consumers dispatch with feed::visit,
// which is a cheap switch (no std::variant indirection).
struct FeedMessage
{
    FeedMsgType type;
    union
    {
        BookUpdate update;
        BookSnapshot snapshot;
        Trade trade;
    };

    // A union with members that have default member initializers deletes the
    // implicit default ctor, so we provide one (defaults to an empty Update).
    // This does NOT affect trivial-copyability (asserted below).
    FeedMessage() noexcept : type(FeedMsgType::Update), update{} {}

    static FeedMessage make(const BookUpdate& u) noexcept
    {
        FeedMessage m;
        m.type = FeedMsgType::Update;
        m.update = u;
        return m;
    }
    static FeedMessage make(const BookSnapshot& s) noexcept
    {
        FeedMessage m;
        m.type = FeedMsgType::Snapshot;
        m.snapshot = s;
        return m;
    }
    static FeedMessage make(const Trade& t) noexcept
    {
        FeedMessage m;
        m.type = FeedMsgType::Trade;
        m.trade = t;
        return m;
    }

    // Instrument id regardless of variant — used for fan-out filtering.
    [[nodiscard]] uint32_t instrument_id() const noexcept
    {
        switch (type)
        {
        case FeedMsgType::Update:
            return update.instrument_id;
        case FeedMsgType::Snapshot:
            return snapshot.instrument_id;
        case FeedMsgType::Trade:
            return trade.instrument_id;
        }
        return 0;
    }

    [[nodiscard]] uint64_t seq() const noexcept
    {
        switch (type)
        {
        case FeedMsgType::Update:
            return update.seq;
        case FeedMsgType::Snapshot:
            return snapshot.seq;
        case FeedMsgType::Trade:
            return trade.seq;
        }
        return 0;
    }
};

static_assert(std::is_trivially_copyable_v<FeedMessage>,
              "FeedMessage must be trivially copyable to live in a "
              "LockFreeQueue ring slot with no heap allocation");

// Dispatch a FeedMessage to a visitor with one overload per message type.
// Reads like std::visit at call sites but compiles to a plain switch.
template <typename Visitor>
decltype(auto) visit(const FeedMessage& m, Visitor&& v)
{
    switch (m.type)
    {
    case FeedMsgType::Snapshot:
        return std::forward<Visitor>(v)(m.snapshot);
    case FeedMsgType::Trade:
        return std::forward<Visitor>(v)(m.trade);
    case FeedMsgType::Update:
    default:
        return std::forward<Visitor>(v)(m.update);
    }
}

} // namespace cmf::feed
