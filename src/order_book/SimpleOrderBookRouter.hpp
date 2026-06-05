#pragma once

#include "BookAnomalyLog.hpp"
#include "LimitOrderBook.hpp"
#include "OrderBookRouter.hpp"
#include "PmrCompat.hpp"
#include <ostream>
#include <sstream>
#include <string>

namespace cmf
{
template <typename BookType = LimitOrderBook>
class SimpleOrderBookRouter
    : public OrderBookRouter<SimpleOrderBookRouter<BookType>>
{
    friend class OrderBookRouter<SimpleOrderBookRouter<BookType>>;

  private:
#if CMF_HAS_STD_PMR
    MemoryResource* mr_;
#endif
    PmrUnorderedMap<uint32_t, BookType> order_books_;
    PmrUnorderedMap<uint64_t, uint32_t> order_to_instrument_;

    // Records any new order_id -> instrument mapping and returns the resolved
    // id (0 if unknown).  Single source of truth for apply_impl's resolution.
    uint32_t resolve_for_apply(const MarketDataEvent& e)
    {
        if (e.instrument_id != 0 && e.order_id != 0)
            order_to_instrument_[e.order_id] = e.instrument_id;
        return order_to_instrument_[e.order_id];
    }

  public:
#if CMF_HAS_STD_PMR
    explicit SimpleOrderBookRouter(MemoryResource* mr = default_memory_resource())
        : mr_(mr), order_books_{mr_}, order_to_instrument_{mr_}
    {
    }
#else
    SimpleOrderBookRouter() = default;
#endif

    void apply_impl(const MarketDataEvent& e)
    {
        const uint32_t instr_id = resolve_for_apply(e);

        if (instr_id == 0 && e.order_id != 0) [[unlikely]]
        {
            BookAnomalyLog::instance().log_router(RouterAnomaly::UnresolvedInstrument,
                                                  e);
            return;
        }

        order_books_[instr_id].apply(e);
    }

    // Look up the book for a (resolved) instrument id; nullptr if none exists.
    // Lets downstream consumers (e.g. the market-data feed) read book state to
    // build snapshots and delta sizes.
    [[nodiscard]] const BookType* find_book(uint32_t instrument_id) const
    {
        const auto it = order_books_.find(instrument_id);
        return it == order_books_.end() ? nullptr : &it->second;
    }

    // Resolve the instrument id an event maps to.  Call AFTER apply(e) so any
    // new order_id -> instrument mapping is already recorded.  Returns 0 if
    // unresolved.  Const, non-mutating twin of resolve_for_apply(): cancels and
    // modifies may carry instrument_id == 0 and rely on the order_id mapping.
    [[nodiscard]] uint32_t resolved_instrument(const MarketDataEvent& e) const
    {
        if (e.instrument_id != 0)
            return e.instrument_id;
        const auto it = order_to_instrument_.find(e.order_id);
        return it == order_to_instrument_.end() ? 0u : it->second;
    }

    // Invoke fn(uint32_t instrument_id, const BookType& book) for each book.
    template <typename Fn>
    void for_each_instrument(Fn&& fn) const
    {
        for (const auto& [instrument_id, order_book] : order_books_)
            fn(instrument_id, order_book);
    }

    void print_snapshot_impl(std::ostream& out,
                             const size_t group_by_levels) const
    {
        for (const auto& [instrument_id, order_book] : order_books_)
        {
            const auto best_bid = order_book.best_price(Side::Buy);
            const auto best_ask = order_book.best_price(Side::Sell);

            if (!best_bid && !best_ask)
                continue;

            out << "// " << instrument_id << "\n";
            order_book.print_snapshot(out, group_by_levels);
        }
    }

    void print_best_bid_ask_impl(std::ostream& out) const
    {
        out << "\n// ====== Final Best Bid/Ask ======\n";
        for (const auto& [instrument_id, order_book] : order_books_)
        {
            auto best_bid = order_book.best_price(Side::Buy);
            auto best_ask = order_book.best_price(Side::Sell);

            if (!best_bid && !best_ask)
                continue;

            out << "Instrument " << instrument_id << ":\n";

            if (best_bid)
            {
                auto bid_volume = order_book.volume_at(Side::Buy, *best_bid);
                out << "  Best Bid: " << *best_bid << " x " << bid_volume << "\n";
            }
            else
            {
                out << "  Best Bid: (empty)\n";
            }

            if (best_ask)
            {
                auto ask_volume = order_book.volume_at(Side::Sell, *best_ask);
                out << "  Best Ask: " << *best_ask << " x " << ask_volume << "\n";
            }
            else
            {
                out << "  Best Ask: (empty)\n";
            }
        }
        out << "// ====== End Best Bid/Ask ======\n";
    }

    [[nodiscard]] std::string
    snapshot_as_string_impl(const std::size_t group_by_levels) const
    {
        std::ostringstream oss;
        for (const auto& [instrument_id, order_book] : order_books_)
        {
            const auto best_bid = order_book.best_price(Side::Buy);
            const auto best_ask = order_book.best_price(Side::Sell);

            if (!best_bid && !best_ask)
                continue;

            oss << "// " << instrument_id << "\n";
            order_book.print_snapshot(oss, group_by_levels);
        }
        return std::move(oss).str();
    }
};

} // namespace cmf
