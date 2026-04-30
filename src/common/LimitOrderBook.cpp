#include "LimitOrderBook.hpp"
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <vector>

// ── L2 helpers ────────────────────────────────────────────────────────────────

void LimitOrderBook::AddToLevel(Side side, int64_t price, uint32_t qty)
{
    if (qty == 0)
        return;
    if (side == Side::Bid)
        _bids[price] += qty;
    else if (side == Side::Ask)
        _asks[price] += qty;
}

void LimitOrderBook::SubtractFromLevel(Side side, int64_t price, uint32_t qty)
{
    if (qty == 0)
        return;
    if (side == Side::Bid)
    {
        auto it = _bids.find(price);
        if (it == _bids.end())
            return;
        if (it->second <= qty)
            _bids.erase(it);
        else
            it->second -= qty;
    }
    else if (side == Side::Ask)
    {
        auto it = _asks.find(price);
        if (it == _asks.end())
            return;
        if (it->second <= qty)
            _asks.erase(it);
        else
            it->second -= qty;
    }
}

// ── Event dispatch ─────────────────────────────────────────────────────────────

void LimitOrderBook::ApplyEvent(const MarketDataEvent& event)
{
    switch (event.action)
    {
    case Action::Add:
    {
        if (event.side == Side::None || event.size == 0)
            break;
        _orders[event.order_id] = {event.side, event.price, event.size};
        AddToLevel(event.side, event.price, event.size);
        break;
    }

    case Action::Cancel:
    {
        auto it = _orders.find(event.order_id);
        if (it == _orders.end())
            break;
        auto& e = it->second;
        uint32_t rem = event.size;
        // Partial cancel if rem < e.size, otherwise remove the order
        if (rem < e.size)
            e.size = rem;
        else
            _orders.erase(it);
        SubtractFromLevel(e.side, e.price, e.size);
        break;
    }

    case Action::Modify:
    {
        auto it = _orders.find(event.order_id);
        if (it == _orders.end())
            break;
        auto& e = it->second;
        SubtractFromLevel(e.side, e.price, e.size);
        e.price = event.price;
        e.size = event.size;
        AddToLevel(e.side, e.price, e.size);
        break;
    }

    case Action::Fill:
    {
        auto it = _orders.find(event.order_id);
        if (it == _orders.end())
            break;
        auto& e = it->second;
        uint32_t rem = event.size;
        if (rem == 0)
        {
            SubtractFromLevel(e.side, e.price, e.size);
            _orders.erase(it);
        }
        else if (rem < e.size)
        {
            SubtractFromLevel(e.side, e.price, e.size - rem);
            e.size = rem;
        }
        break;
    }

    case Action::Trade:
        break;

    case Action::Clear:
        _orders.clear();
        _bids.clear();
        _asks.clear();
        break;

    case Action::None:
        break;
    }
}

// ── Queries ────────────────────────────────────────────────────────────────────

auto LimitOrderBook::BestBid() const -> std::optional<std::pair<int64_t, uint32_t>>
{
    if (_bids.empty())
        return std::nullopt;
    auto it = _bids.begin();
    return std::make_pair(it->first, it->second);
}

auto LimitOrderBook::BestAsk() const -> std::optional<std::pair<int64_t, uint32_t>>
{
    if (_asks.empty())
        return std::nullopt;
    auto it = _asks.begin();
    return std::make_pair(it->first, it->second);
}

auto LimitOrderBook::VolumeAtPrice(int64_t price) const -> uint32_t
{
    if (auto it = _bids.find(price); it != _bids.end())
        return it->second;
    if (auto it = _asks.find(price); it != _asks.end())
        return it->second;
    return 0;
}

// ── Snapshot ──────────────────────────────────────────────────────────────────

void LimitOrderBook::PrintSnapshot(uint32_t instrument_id, int depth) const
{
    std::cout << std::fixed << std::setprecision(9);
    std::cout << "\n=== LOB [instrument_id=" << instrument_id << "] ===\n";

    std::vector<std::pair<int64_t, uint32_t>> ask_levels;
    for (auto it = _asks.begin(); it != _asks.end() && (int)ask_levels.size() < depth; ++it)
        ask_levels.emplace_back(it->first, it->second);

    for (auto it = ask_levels.rbegin(); it != ask_levels.rend(); ++it)
    {
        std::cout << "  ASK  " << std::setw(16) << MarketDataEvent::priceToDouble(it->first)
                  << "  x  " << it->second << "\n";
    }

    auto bb = BestBid();
    auto ba = BestAsk();
    if (bb && ba)
    {
        double spread = MarketDataEvent::priceToDouble(ba->first - bb->first);
        std::cout << "  --- spread: " << spread << " ---\n";
    }
    else
    {
        std::cout << "  --- spread: N/A ---\n";
    }

    int count = 0;
    for (auto it = _bids.begin(); it != _bids.end() && count < depth; ++it, ++count)
    {
        std::cout << "  BID  " << std::setw(16) << MarketDataEvent::priceToDouble(it->first)
                  << "  x  " << it->second << "\n";
    }

    std::cout << "  [orders=" << _orders.size()
              << "  bid_levels=" << _bids.size()
              << "  ask_levels=" << _asks.size() << "]\n";
}
