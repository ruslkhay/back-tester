#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

template <typename T>
class BatchQueue
{
  public:
    using batch_t = std::vector<T>;

    explicit BatchQueue(std::size_t capacity = 64) : _capacity{capacity} {}

    BatchQueue(const BatchQueue&) = delete;
    BatchQueue(BatchQueue&&) = delete;
    BatchQueue& operator=(const BatchQueue&) = delete;
    BatchQueue& operator=(BatchQueue&&) = delete;

    void Push(batch_t&& batch)
    {
        if (batch.empty())
            return;
        std::unique_lock<std::mutex> lock{_mtx};
        _cvNotFull.wait(lock, [this]
                        { return _queue.size() < _capacity || _done; });
        if (_done)
            return;
        _queue.push_back(std::move(batch));
        lock.unlock();
        _cvNotEmpty.notify_one();
    }

    auto Pop() -> std::optional<batch_t>
    {
        std::unique_lock<std::mutex> lock{_mtx};
        _cvNotEmpty.wait(lock, [this]
                         { return !_queue.empty() || _done; });
        if (_queue.empty())
            return std::nullopt;
        auto batch = std::move(_queue.front());
        _queue.pop_front();
        lock.unlock();
        _cvNotFull.notify_one();
        return batch;
    }

    void MarkDone()
    {
        std::lock_guard<std::mutex> lock{_mtx};
        _done = true;
        _cvNotEmpty.notify_all();
        _cvNotFull.notify_all();
    }

  private:
    std::size_t _capacity;
    std::deque<batch_t> _queue;
    bool _done{false};
    std::mutex _mtx;
    std::condition_variable _cvNotEmpty;
    std::condition_variable _cvNotFull;
};

template <typename T>
using ThreadSafeQueue = BatchQueue<T>;
