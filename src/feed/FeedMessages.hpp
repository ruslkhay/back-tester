#pragma once

#include "common/BasicTypes.hpp"

#include <array>
#include <cstdint>
#include <type_traits>

namespace cmf::feed
{

inline constexpr std::size_t kSnapshotDepth = 10;

struct Level
{
    ScaledPrice price = 0;
    uint64_t qty = 0;
};

struct BookUpdate
{
    uint64_t seq = 0;
    NanoTime ts_event = 0;
    uint32_t instrument_id = 0;
    Side side = Side::None;
    ScaledPrice price = 0;
    uint64_t new_qty = 0; // 0 = level removed
};

// Top-N snapshot; seq is the sequence it is consistent with.
struct BookSnapshot
{
    uint64_t seq = 0;
    NanoTime ts_event = 0;
    uint32_t instrument_id = 0;
    uint16_t bid_depth = 0;
    uint16_t ask_depth = 0;
    std::array<Level, kSnapshotDepth> bids{}; // index 0 = best, descending
    std::array<Level, kSnapshotDepth> asks{}; // index 0 = best, ascending
};

struct Trade
{
    uint64_t seq = 0;
    NanoTime ts_event = 0;
    uint32_t instrument_id = 0;
    Side aggressor = Side::None;
    ScaledPrice price = 0;
    uint64_t size = 0;
};

enum class FeedMsgType : uint8_t
{
    Update = 0,
    Snapshot = 1,
    Trade = 2,
};

// Trivially-copyable tagged union: a queue slot is fixed-size with no heap.
struct FeedMessage
{
    FeedMsgType type;
    union
    {
        BookUpdate update;
        BookSnapshot snapshot;
        Trade trade;
    };

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

static_assert(std::is_trivially_copyable_v<FeedMessage>);

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
