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

// Capacity of each subscriber's SPSC queue (power of two, per LockFreeQueue).
inline constexpr std::size_t kSubscriberQueueCap = 8192;

// MarketDataPublisher: one producer (the dispatcher thread) fans feed messages
// out to N subscribers, each draining its own SPSC LockFreeQueue on its own
// worker thread.  A slow subscriber cannot block a fast one.
//
// Threading contract:
//   - publishUpdate / publishTrade / publishSnapshot are called from a SINGLE
//     producer thread (the dispatcher).  This is what makes the per-subscriber
//     SPSC queues and the lock-free seq counters correct.
//   - subscribe / unsubscribe are rare control-plane operations; they take a
//     write lock and copy-on-write swap the immutable subscriber vector so the
//     publish hot path never holds a mutex.
//   - For Phase B, subscribe/unsubscribe are expected to happen before/after
//     the publish stream, not concurrently with it.
class MarketDataPublisher
{
  public:
    using Callback = std::function<void(const FeedMessage&)>;

    MarketDataPublisher();
    ~MarketDataPublisher();

    MarketDataPublisher(const MarketDataPublisher&) = delete;
    MarketDataPublisher& operator=(const MarketDataPublisher&) = delete;

    // Register a subscriber.  Empty `instruments` => receive ALL instruments.
    // The callback runs on the subscriber's own worker thread.
    //
    // `bootstrap` messages (typically BookSnapshots) are enqueued into the new
    // subscriber's queue BEFORE it joins the broadcast set, so they arrive
    // ahead of any live delta -- the snapshot-then-deltas bootstrap pattern.
    // Because each queue is SPSC, a non-empty `bootstrap` requires calling
    // subscribe() on the producer thread (or before live publishing begins), so
    // every push to this queue originates from a single thread.
    [[nodiscard]] SubscriberHandle
    subscribe(Callback cb, std::unordered_set<uint32_t> instruments = {},
              std::vector<FeedMessage> bootstrap = {});

    // Stop a subscriber: closes its queue (its worker drains and exits).
    void unsubscribe(const SubscriberHandle& h);

    // Publish (hot path).  Each stamps the message's seq before fan-out.
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
    // A snapshot is a fresh start: it carries the seq it is consistent with and
    // does NOT advance the counter; subscribers reset expected-next to seq + 1.
    void publishSnapshot(BookSnapshot s)
    {
        s.seq = current_seq(s.instrument_id);
        broadcast(FeedMessage::make(s));
    }

    // Sequence counters (producer-thread only; see threading contract).
    // Inline: they sit on the publish hot path.  Lazily create the counter on
    // first sight of an instrument.
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

    // Messages dropped for a subscriber because its queue was full.
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

        // Drain loop: exits when the queue is closed AND empty.
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
        std::jthread worker; // declared LAST -> destroyed (joined) FIRST
    };

    using SubVec = std::vector<std::shared_ptr<Subscriber>>;

    // Per-instrument sequence counter, padded to its own cache line so adjacent
    // instruments' counters don't false-share.
    struct alignas(64) SeqCounter
    {
        std::atomic<uint64_t> v{0};
    };

    // Hot path: enqueue with non-blocking drop-on-overflow.  Safe because the
    // queue is SPSC and only this (producer) thread calls size() and push():
    // the consumer can only FREE slots, so a passed size-check guarantees the
    // subsequent push() finds room and never spins.
    void enqueue(Subscriber& s, const FeedMessage& m) const
    {
        if (s.queue.is_closed()) [[unlikely]]
            return;
        if (s.queue.size() < kSubscriberQueueCap - 1)
            s.queue.push(m);
        else
            s.dropped.fetch_add(1, std::memory_order_relaxed);
    }

    void broadcast(const FeedMessage& m) const
    {
        const std::shared_ptr<const SubVec> snap = active_.load();
        if (!snap) [[unlikely]]
            return;
        const uint32_t instr = m.instrument_id();
        for (const auto& s : *snap)
            if (s->wants(instr))
                enqueue(*s, m);
    }

    // Immutable, atomically-swapped subscriber vector (copy-on-write).
    std::atomic<std::shared_ptr<const SubVec>> active_;
    std::shared_mutex sub_mutex_; // guards subscribe/unsubscribe writers
    std::atomic<uint64_t> next_sub_id_{1};

    // Producer-thread-owned (no lock; see threading contract).  unique_ptr keeps
    // each SeqCounter's address stable across map rehash.
    std::unordered_map<uint32_t, std::unique_ptr<SeqCounter>> seq_;
};

} // namespace cmf::feed
