// Unit tests for LimitOrderBook

#include "common/LimitOrderBook.hpp"
#include "common/MarketDataEvent.hpp"

#include <catch2/catch_test_macros.hpp>
#include <unordered_map>

TEST_CASE("LimitOrderBook - Add bid order", "[LimitOrderBook]")
{
    LimitOrderBook lob;

    MarketDataEvent event;
    event.order_id = 101;
    event.instrument_id = 100;
    event.action = Action::Add;
    event.side = Side::Bid;
    event.price = 10'000'000'000;
    event.size = 100;

    lob.ApplyEvent(event);

    REQUIRE(lob.OrderCount() == 1);
    REQUIRE(lob.BidLevels() == 1);
    auto bb = lob.BestBid();
    REQUIRE(bb);
    REQUIRE(bb->first == 10'000'000'000);
    REQUIRE(bb->second == 100);
}

TEST_CASE("LimitOrderBook - Add ask order", "[LimitOrderBook]")
{
    LimitOrderBook lob;

    MarketDataEvent event;
    event.order_id = 101;
    event.instrument_id = 100;
    event.action = Action::Add;
    event.side = Side::Ask;
    event.price = 10'000'000'000;
    event.size = 50;

    lob.ApplyEvent(event);

    REQUIRE(lob.OrderCount() == 1);
    REQUIRE(lob.AskLevels() == 1);
    auto ba = lob.BestAsk();
    REQUIRE(ba);
    REQUIRE(ba->first == 10000000000);
    REQUIRE(ba->second == 50);
}

TEST_CASE("LimitOrderBook - Cancel partial", "[LimitOrderBook]")
{
    LimitOrderBook lob;

    MarketDataEvent add_event;
    add_event.order_id = 101;
    add_event.action = Action::Add;
    add_event.side = Side::Bid;
    add_event.price = 10'000'000'000;
    add_event.size = 100;
    lob.ApplyEvent(add_event);

    REQUIRE(lob.OrderCount() == 1);
    REQUIRE(lob.BidLevels() == 1);

    MarketDataEvent cancel_event;
    cancel_event.order_id = 101;
    cancel_event.action = Action::Cancel;
    cancel_event.side = Side::Bid;
    cancel_event.price = 10'000'000'000;
    cancel_event.size = 30;
    lob.ApplyEvent(cancel_event);

    REQUIRE(lob.OrderCount() == 1);
    REQUIRE(lob.VolumeAtPrice(10'000'000'000) == 70);
}

TEST_CASE("LimitOrderBook - Cancel full", "[LimitOrderBook]")
{
    LimitOrderBook lob;

    MarketDataEvent add_event;
    add_event.order_id = 101;
    add_event.action = Action::Add;
    add_event.side = Side::Bid;
    add_event.price = 10'000'000'000;
    add_event.size = 100;
    lob.ApplyEvent(add_event);

    REQUIRE(lob.OrderCount() == 1);

    MarketDataEvent cancel_event;
    cancel_event.order_id = 101;
    cancel_event.action = Action::Cancel;
    cancel_event.side = Side::Bid;
    cancel_event.price = 10'000'000'000;
    cancel_event.size = 100;
    lob.ApplyEvent(cancel_event);

    REQUIRE(lob.OrderCount() == 0);
    REQUIRE(lob.BidLevels() == 0);
}

TEST_CASE("LimitOrderBook - Modify price and size", "[LimitOrderBook]")
{
    LimitOrderBook lob;

    MarketDataEvent add_event;
    add_event.order_id = 101;
    add_event.action = Action::Add;
    add_event.side = Side::Bid;
    add_event.price = 10'000'000'000;
    add_event.size = 100;
    lob.ApplyEvent(add_event);

    REQUIRE(lob.VolumeAtPrice(10'000'000'000) == 100);

    MarketDataEvent mod_event;
    mod_event.order_id = 101;
    mod_event.action = Action::Modify;
    mod_event.side = Side::Bid;
    mod_event.price = 11'000'000'000;
    mod_event.size = 75;
    lob.ApplyEvent(mod_event);

    REQUIRE(lob.OrderCount() == 1);
    REQUIRE(lob.VolumeAtPrice(10000000000) == 0);
    REQUIRE(lob.VolumeAtPrice(11000000000) == 75);
}

TEST_CASE("LimitOrderBook - Fill partial", "[LimitOrderBook]")
{
    LimitOrderBook lob;

    MarketDataEvent add_event;
    add_event.order_id = 101;
    add_event.action = Action::Add;
    add_event.side = Side::Bid;
    add_event.price = 10'000'000'000;
    add_event.size = 100;
    lob.ApplyEvent(add_event);

    REQUIRE(lob.OrderCount() == 1);

    MarketDataEvent fill_event;
    fill_event.order_id = 101;
    fill_event.action = Action::Fill;
    fill_event.side = Side::Bid;
    fill_event.price = 10'000'000'000;
    fill_event.size = 40;
    lob.ApplyEvent(fill_event);

    REQUIRE(lob.OrderCount() == 1);
    REQUIRE(lob.VolumeAtPrice(10000000000) == 40);
}

TEST_CASE("LimitOrderBook - Fill complete", "[LimitOrderBook]")
{
    LimitOrderBook lob;

    MarketDataEvent add_event;
    add_event.order_id = 101;
    add_event.action = Action::Add;
    add_event.side = Side::Bid;
    add_event.price = 10'000'000'000;
    add_event.size = 100;
    lob.ApplyEvent(add_event);

    MarketDataEvent fill_event;
    fill_event.order_id = 101;
    fill_event.action = Action::Fill;
    fill_event.side = Side::Bid;
    fill_event.price = 10'000'000'000;
    fill_event.size = 0;
    lob.ApplyEvent(fill_event);

    REQUIRE(lob.OrderCount() == 0);
    REQUIRE(lob.BidLevels() == 0);
}

TEST_CASE("LimitOrderBook - Trade is informational", "[LimitOrderBook]")
{
    LimitOrderBook lob;

    MarketDataEvent trade_event;
    trade_event.order_id = 999;
    trade_event.action = Action::Trade;
    trade_event.side = Side::Bid;
    trade_event.price = 10'000'000'000;
    trade_event.size = 50;
    lob.ApplyEvent(trade_event);

    REQUIRE(lob.OrderCount() == 0);
    REQUIRE(lob.BidLevels() == 0);
}

TEST_CASE("LimitOrderBook - BestBid is highest buy price", "[LimitOrderBook]")
{
    LimitOrderBook lob;

    std::vector<int64_t> prices = {9'000'000'000, 10'000'000'000, 11'000'000'000};
    for (int i = 0; i < 3; ++i)
    {
        MarketDataEvent event;
        event.order_id = static_cast<uint64_t>(100 + i);
        event.action = Action::Add;
        event.side = Side::Bid;
        event.price = prices[i];
        event.size = 100;
        lob.ApplyEvent(event);
    }

    auto bb = lob.BestBid();
    REQUIRE(bb);
    REQUIRE(bb->first == 11'000'000'000);
}

TEST_CASE("LimitOrderBook - BestAsk is lowest sell price", "[LimitOrderBook]")
{
    LimitOrderBook lob;

    std::vector<int64_t> prices = {9'000'000'000, 10'000'000'000, 11'000'000'000};
    for (int i = 0; i < 3; ++i)
    {
        MarketDataEvent event;
        event.order_id = static_cast<uint64_t>(200 + i);
        event.action = Action::Add;
        event.side = Side::Ask;
        event.price = prices[i];
        event.size = 50;
        lob.ApplyEvent(event);
    }

    auto ba = lob.BestAsk();
    REQUIRE(ba);
    REQUIRE(ba->first == 9'000'000'000);
}

TEST_CASE("LimitOrderBook - Spread is positive", "[LimitOrderBook]")
{
    LimitOrderBook lob;

    MarketDataEvent bid_event;
    bid_event.order_id = 101;
    bid_event.action = Action::Add;
    bid_event.side = Side::Bid;
    bid_event.price = 10'000'000'000;
    bid_event.size = 100;
    lob.ApplyEvent(bid_event);

    MarketDataEvent ask_event;
    ask_event.order_id = 201;
    ask_event.action = Action::Add;
    ask_event.side = Side::Ask;
    ask_event.price = 10'000'000'100;
    ask_event.size = 50;
    lob.ApplyEvent(ask_event);

    auto bb = lob.BestBid();
    auto ba = lob.BestAsk();
    REQUIRE(bb);
    REQUIRE(ba);
    REQUIRE(ba->first > bb->first);
}

TEST_CASE("LimitOrderBook - VolumeAtPrice aggregates", "[LimitOrderBook]")
{
    LimitOrderBook lob;

    MarketDataEvent event1;
    event1.order_id = 101;
    event1.action = Action::Add;
    event1.side = Side::Bid;
    event1.price = 10'000'000'000;
    event1.size = 100;
    lob.ApplyEvent(event1);

    MarketDataEvent event2;
    event2.order_id = 102;
    event2.action = Action::Add;
    event2.side = Side::Bid;
    event2.price = 10'000'000'000;
    event2.size = 75;
    lob.ApplyEvent(event2);

    REQUIRE(lob.VolumeAtPrice(10000000000) == 175);
}

TEST_CASE("LimitOrderBook - Multiple instruments routing", "[LimitOrderBook]")
{
    std::unordered_map<uint32_t, LimitOrderBook> lobs;

    MarketDataEvent event1;
    event1.order_id = 101;
    event1.instrument_id = 100;
    event1.action = Action::Add;
    event1.side = Side::Bid;
    event1.price = 10'000'000'000;
    event1.size = 100;
    lobs[event1.instrument_id].ApplyEvent(event1);

    MarketDataEvent event2;
    event2.order_id = 201;
    event2.instrument_id = 200;
    event2.action = Action::Add;
    event2.side = Side::Ask;
    event2.price = 9'000'000'000;
    event2.size = 50;
    lobs[event2.instrument_id].ApplyEvent(event2);

    REQUIRE(lobs.size() == 2);
    REQUIRE(lobs[100].OrderCount() == 1);
    REQUIRE(lobs[200].OrderCount() == 1);
    REQUIRE(lobs[100].BidLevels() == 1);
    REQUIRE(lobs[200].AskLevels() == 1);
}

TEST_CASE("LimitOrderBook - Clear resets book", "[LimitOrderBook]")
{
    LimitOrderBook lob;

    for (int i = 0; i < 3; ++i)
    {
        MarketDataEvent event;
        event.order_id = static_cast<uint64_t>(100 + i);
        event.action = Action::Add;
        event.side = (i % 2 == 0 ? Side::Bid : Side::Ask);
        event.price = 10'000'000'000 + i * 1000;
        event.size = 100;
        lob.ApplyEvent(event);
    }

    REQUIRE(lob.OrderCount() == 3);
    REQUIRE(lob.BidLevels() > 0);
    REQUIRE(lob.AskLevels() > 0);

    MarketDataEvent clear_event;
    clear_event.action = Action::Clear;
    lob.ApplyEvent(clear_event);

    REQUIRE(lob.OrderCount() == 0);
    REQUIRE(lob.BidLevels() == 0);
    REQUIRE(lob.AskLevels() == 0);
}
