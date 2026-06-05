#include "feed/MarketDataPublisher.hpp"

#include <mutex>
#include <utility>

namespace cmf::feed
{

MarketDataPublisher::MarketDataPublisher()
{
    // Start with an empty (non-null) subscriber vector so the hot path never
    // sees a null snapshot.
    active_.store(std::make_shared<SubVec>());
}

MarketDataPublisher::~MarketDataPublisher() { shutdown(); }

SubscriberHandle
MarketDataPublisher::subscribe(Callback cb,
                               std::unordered_set<uint32_t> instruments)
{
    const uint64_t id = next_sub_id_.fetch_add(1, std::memory_order_relaxed);
    auto sub =
        std::make_shared<Subscriber>(id, std::move(cb), std::move(instruments));

    std::unique_lock lock(sub_mutex_);
    const std::shared_ptr<const SubVec> cur = active_.load();
    auto next =
        cur ? std::make_shared<SubVec>(*cur) : std::make_shared<SubVec>();
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
        if (!cur)
            return;
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
    // Close outside the lock.  Once removed from `active_` no new messages are
    // enqueued; close() makes the worker's pop() return false so its jthread
    // exits.  The Subscriber is destroyed (joined) when the last ref drops.
    if (removed)
        removed->queue.close();
}

uint64_t MarketDataPublisher::dropped(const SubscriberHandle& h) const
{
    const std::shared_ptr<const SubVec> cur = active_.load();
    if (!cur)
        return 0;
    for (const auto& s : *cur)
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
    if (!cur)
        return;
    // Close every queue so each worker's pop() returns false and its jthread
    // exits.  Subscribers are destroyed (and their workers joined) when `cur`
    // (and any in-flight broadcast snapshot) drops the last reference.
    for (const auto& s : *cur)
        s->queue.close();
}

} // namespace cmf::feed
