#pragma once

#include "common/BasicTypes.hpp"
#include "common/LockFreeQueue.hpp"
#include "feed/FeedMessages.hpp"
#include "feed/Subscriber.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cmf::feed
{

inline constexpr std::size_t kSubscriberQueueCap = 8192;

// Fans feed messages out to N subscribers, each draining its own SPSC queue on
// its own worker thread, so a slow subscriber cannot block a fast one.
//
// Threading: publish*/next_seq run on a single producer thread. subscribe/
// unsubscribe run on a single control thread and copy-on-write swap an immutable
// subscriber vector, so they may run concurrently with publishing -- except a
// subscribe() with a non-empty `bootstrap`, which must run on the producer
// thread to keep each queue single-producer.
class MarketDataPublisher
{
  public:
    using Callback = std::function<void(const FeedMessage&)>;

    MarketDataPublisher();
    ~MarketDataPublisher();

    MarketDataPublisher(const MarketDataPublisher&) = delete;
    MarketDataPublisher& operator=(const MarketDataPublisher&) = delete;

    // Empty `instruments` => receive all. `bootstrap` messages are enqueued
    // before the subscriber joins the broadcast set (snapshot-then-deltas).
    [[nodiscard]] SubscriberHandle
    subscribe(Callback cb, std::unordered_set<uint32_t> instruments = {},
              std::vector<FeedMessage> bootstrap = {});

    void unsubscribe(const SubscriberHandle& h);

    void publishUpdate(BookUpdate u)
    {
        u.seq = next_seq(u.instrument_id);
        broadcast(FeedMessage::make(u));
    }
    void publishTrade(Trade t)
    {
        t.seq = next_seq(t.instrument_id);
        broadcast(FeedMessage::make(t));
    }
    // A snapshot carries the seq it is consistent with and does NOT advance it.
    void publishSnapshot(BookSnapshot s)
    {
        s.seq = current_seq(s.instrument_id);
        broadcast(FeedMessage::make(s));
    }

    [[nodiscard]] uint64_t next_seq(uint32_t instrument)
    {
        auto it = seq_.find(instrument);
        if (it == seq_.end())
            it = seq_.emplace(instrument, std::make_unique<SeqCounter>()).first;
        return it->second->v.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    [[nodiscard]] uint64_t current_seq(uint32_t instrument) const
    {
        const auto it = seq_.find(instrument);
        return it == seq_.end()
                   ? 0
                   : it->second->v.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t dropped(const SubscriberHandle& h) const;

    // Idempotent: close every subscriber queue and join all worker threads.
    void shutdown();

  private:
    struct Subscriber
    {
        Subscriber(uint64_t id_, Callback cb,
                   std::unordered_set<uint32_t> instruments)
            : id(id_), filter(std::move(instruments)),
              filter_all(filter.empty()), callback(std::move(cb)),
              worker([this]
                     { run(); })
        {
        }

        [[nodiscard]] bool wants(uint32_t instr) const noexcept
        {
            return filter_all || filter.contains(instr);
        }

        void run()
        {
            while (queue.pop([this](FeedMessage&& m)
                             { callback(m); }))
                ;
        }

        uint64_t id;
        std::unordered_set<uint32_t> filter;
        bool filter_all;
        Callback callback;
        std::atomic<uint64_t> dropped{0};
        LockFreeQueue<FeedMessage, kSubscriberQueueCap> queue;
        std::jthread worker; // last member: joined before queue/callback die
    };

    using SubVec = std::vector<std::shared_ptr<Subscriber>>;

    struct alignas(64) SeqCounter
    {
        std::atomic<uint64_t> v{0};
    };

    // Non-blocking drop-on-overflow. The size check guarantees room, so push()
    // won't spin; it may still throw if unsubscribe() closed the queue
    // concurrently -- benign, message dropped.
    void enqueue(Subscriber& s, const FeedMessage& m) const
    {
        if (s.queue.is_closed()) [[unlikely]]
            return;
        if (s.queue.size() < kSubscriberQueueCap - 1)
        {
            try
            {
                s.queue.push(m);
            }
            catch (const std::runtime_error&)
            {
            }
        }
        else
            s.dropped.fetch_add(1, std::memory_order_relaxed);
    }

    void broadcast(const FeedMessage& m) const
    {
        const std::shared_ptr<const SubVec> snap = active_.load();
        const uint32_t instr = m.instrument_id();
        for (const auto& s : *snap)
            if (s->wants(instr))
                enqueue(*s, m);
    }

    std::atomic<std::shared_ptr<const SubVec>> active_;
    std::shared_mutex sub_mutex_;
    std::atomic<uint64_t> next_sub_id_{1};
    std::unordered_map<uint32_t, std::unique_ptr<SeqCounter>> seq_;
};

} // namespace cmf::feed
