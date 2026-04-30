#pragma once

#include "MarketDataEvent.hpp"
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>

class LimitOrderBook
{
  public:
    void ApplyEvent(const MarketDataEvent& event);

    auto BestBid() const -> std::optional<std::pair<int64_t, uint32_t>>;
    auto BestAsk() const -> std::optional<std::pair<int64_t, uint32_t>>;

    auto VolumeAtPrice(int64_t price) const -> uint32_t;

    void PrintSnapshot(uint32_t instrument_id, int depth = 5) const;

    auto OrderCount() const -> std::size_t { return _orders.size(); }
    auto BidLevels() const -> std::size_t { return _bids.size(); }
    auto AskLevels() const -> std::size_t { return _asks.size(); }

  private:
    struct OrderEntry
    {
        Side side;
        int64_t price;
        uint32_t size;
    };

    std::unordered_map<uint64_t, OrderEntry> _orders;
    std::map<int64_t, uint32_t, std::greater<int64_t>> _bids;
    std::map<int64_t, uint32_t> _asks;

    void AddToLevel(Side side, int64_t price, uint32_t qty);
    void SubtractFromLevel(Side side, int64_t price, uint32_t qty);
};
