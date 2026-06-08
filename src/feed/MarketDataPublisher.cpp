#include "feed/MarketDataPublisher.hpp"

#include <mutex>
#include <utility>

namespace cmf::feed
{

MarketDataPublisher::MarketDataPublisher()
{
    active_.store(std::make_shared<SubVec>());
}

MarketDataPublisher::~MarketDataPublisher() { shutdown(); }

SubscriberHandle
MarketDataPublisher::subscribe(Callback cb,
                               std::unordered_set<uint32_t> instruments,
                               std::vector<FeedMessage> bootstrap)
{
    const uint64_t id = next_sub_id_.fetch_add(1, std::memory_order_relaxed);
    auto sub =
        std::make_shared<Subscriber>(id, std::move(cb), std::move(instruments));

    // Enqueue bootstrap before the subscriber joins active_, so it precedes any
    // live delta in the FIFO queue.
    for (const FeedMessage& m : bootstrap)
        enqueue(*sub, m);

    std::unique_lock lock(sub_mutex_);
    auto next = std::make_shared<SubVec>(*active_.load());
    next->push_back(std::move(sub));
    active_.store(std::move(next));
    return SubscriberHandle{id};
}

void MarketDataPublisher::unsubscribe(const SubscriberHandle& h)
{
    std::shared_ptr<Subscriber> removed;
    {
        std::unique_lock lock(sub_mutex_);
        const std::shared_ptr<const SubVec> cur = active_.load();
        auto next = std::make_shared<SubVec>();
        next->reserve(cur->size());
        for (const auto& s : *cur)
        {
            if (s->id == h.id)
                removed = s;
            else
                next->push_back(s);
        }
        active_.store(std::move(next));
    }
    // Closing makes the worker's pop() return false; the Subscriber is joined
    // when the last reference (incl. any in-flight broadcast snapshot) drops.
    if (removed)
        removed->queue.close();
}

uint64_t MarketDataPublisher::dropped(const SubscriberHandle& h) const
{
    for (const auto& s : *active_.load())
        if (s->id == h.id)
            return s->dropped.load(std::memory_order_relaxed);
    return 0;
}

void MarketDataPublisher::shutdown()
{
    std::shared_ptr<const SubVec> cur;
    {
        std::unique_lock lock(sub_mutex_);
        cur = active_.exchange(std::make_shared<SubVec>());
    }
    for (const auto& s : *cur)
        s->queue.close();
}

} // namespace cmf::feed
