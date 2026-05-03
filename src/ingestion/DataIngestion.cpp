#include "DataIngestion.hpp"
#include "ThreadSafeQueue.hpp"
#include "common/MarketDataParser.hpp"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <queue>
#include <thread>
#include <vector>

auto ListNDJSONFiles(const std::string& folder_path) -> std::vector<std::string>
{
    std::vector<std::string> files;
    try
    {
        for (const auto& entry : std::filesystem::directory_iterator(folder_path))
        {
            if (entry.is_regular_file())
            {
                const auto& path = entry.path();
                if (path.extension() == ".json" || path.extension() == ".ndjson")
                {
                    files.push_back(path.string());
                }
            }
        }
        std::sort(files.begin(), files.end());
    }
    catch (const std::filesystem::filesystem_error&)
    {
    }
    return files;
}

namespace
{

using batch_t = std::vector<MarketDataEvent>;
using queue_t = BatchQueue<MarketDataEvent>;

constexpr std::size_t BATCH_SIZE = 1024;
constexpr std::size_t QUEUE_CAPACITY = 64;

void ProducerThread(const std::string& file_path, queue_t& queue)
{
    std::ifstream file{file_path};
    if (!file.is_open())
    {
        queue.MarkDone();
        return;
    }

    batch_t batch;
    batch.reserve(BATCH_SIZE);

    std::string line;
    while (std::getline(file, line))
    {
        auto event = parseNDJSON(line);
        if (!event)
            continue;
        batch.push_back(std::move(*event));
        if (batch.size() >= BATCH_SIZE)
        {
            queue.Push(std::move(batch));
            batch = batch_t{};
            batch.reserve(BATCH_SIZE);
        }
    }

    if (!batch.empty())
        queue.Push(std::move(batch));

    queue.MarkDone();
}

// Holds a producer's current batch and a cursor into it. Refills from the
// upstream queue when exhausted. Returns false when the source is fully drained.
struct BatchCursor
{
    queue_t* source{nullptr};
    batch_t current;
    std::size_t pos{0};

    auto Empty() const -> bool { return pos >= current.size(); }

    auto Refill() -> bool
    {
        auto next = source->Pop();
        if (!next)
        {
            current.clear();
            pos = 0;
            return false;
        }
        current = std::move(*next);
        pos = 0;
        return !current.empty();
    }

    auto Front() -> MarketDataEvent& { return current[pos]; }
    void Advance() { ++pos; }
};

struct HeapItem
{
    std::uint64_t ts_event;
    std::size_t producer_idx;
    bool operator>(const HeapItem& other) const { return ts_event > other.ts_event; }
};

// Pushes the merged_batch into out_queue when it reaches BATCH_SIZE.
inline void EmitMerged(batch_t& merged_batch, queue_t& out_queue)
{
    if (merged_batch.size() >= BATCH_SIZE)
    {
        out_queue.Push(std::move(merged_batch));
        merged_batch = batch_t{};
        merged_batch.reserve(BATCH_SIZE);
    }
}

void DispatcherLoop(queue_t& queue, EventCallback& on_event)
{
    while (auto batch = queue.Pop())
    {
        for (auto& event : *batch)
            on_event(event);
    }
}

} // namespace

void FlatMergerEngine::Ingest(
    const std::vector<std::string>& file_paths,
    EventCallback on_event)
{
    if (file_paths.empty())
        return;

    const std::size_t n = file_paths.size();
    std::vector<std::unique_ptr<queue_t>> producer_queues;
    producer_queues.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        producer_queues.push_back(std::make_unique<queue_t>(QUEUE_CAPACITY));

    auto merged_queue = std::make_unique<queue_t>(QUEUE_CAPACITY);

    std::vector<std::jthread> threads;
    threads.reserve(n + 2);

    for (std::size_t i = 0; i < n; ++i)
        threads.emplace_back(ProducerThread, file_paths[i], std::ref(*producer_queues[i]));

    threads.emplace_back([&producer_queues, &merged_queue, n]()
                         {
        std::vector<BatchCursor> cursors(n);
        std::priority_queue<HeapItem, std::vector<HeapItem>, std::greater<HeapItem>> heap;

        for (std::size_t i = 0; i < n; ++i)
        {
            cursors[i].source = producer_queues[i].get();
            if (cursors[i].Refill())
                heap.push({cursors[i].Front().ts_event, i});
        }

        batch_t merged;
        merged.reserve(BATCH_SIZE);

        while (!heap.empty())
        {
            auto top = heap.top();
            heap.pop();
            auto& cur = cursors[top.producer_idx];
            merged.push_back(std::move(cur.Front()));
            cur.Advance();
            EmitMerged(merged, *merged_queue);

            if (cur.Empty() && !cur.Refill())
                continue;
            heap.push({cur.Front().ts_event, top.producer_idx});
        }

        if (!merged.empty())
            merged_queue->Push(std::move(merged));
        merged_queue->MarkDone(); });

    threads.emplace_back([&merged_queue, &on_event]()
                         { DispatcherLoop(*merged_queue, on_event); });
}

void HierarchyMergerEngine::Ingest(
    const std::vector<std::string>& file_paths,
    EventCallback on_event)
{
    if (file_paths.empty())
        return;

    const std::size_t n = file_paths.size();
    std::vector<std::unique_ptr<queue_t>> queues;
    queues.reserve(2 * n);
    for (std::size_t i = 0; i < n; ++i)
        queues.push_back(std::make_unique<queue_t>(QUEUE_CAPACITY));

    std::vector<std::jthread> threads;
    threads.reserve(2 * n + 1);

    for (std::size_t i = 0; i < n; ++i)
        threads.emplace_back(ProducerThread, file_paths[i], std::ref(*queues[i]));

    std::vector<std::size_t> current_level;
    current_level.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        current_level.push_back(i);

    while (current_level.size() > 1)
    {
        std::vector<std::size_t> next_level;
        next_level.reserve((current_level.size() + 1) / 2);

        for (std::size_t i = 0; i + 1 < current_level.size(); i += 2)
        {
            const std::size_t left_idx = current_level[i];
            const std::size_t right_idx = current_level[i + 1];
            const std::size_t out_idx = queues.size();
            queues.push_back(std::make_unique<queue_t>(QUEUE_CAPACITY));
            next_level.push_back(out_idx);

            threads.emplace_back([&queues, left_idx, right_idx, out_idx]()
                                 {
                BatchCursor a{queues[left_idx].get(), {}, 0};
                BatchCursor b{queues[right_idx].get(), {}, 0};
                a.Refill();
                b.Refill();

                batch_t merged;
                merged.reserve(BATCH_SIZE);

                while (!a.Empty() || !b.Empty())
                {
                    bool pick_left;
                    if (a.Empty())
                        pick_left = false;
                    else if (b.Empty())
                        pick_left = true;
                    else
                        pick_left = a.Front().ts_event <= b.Front().ts_event;

                    auto& src = pick_left ? a : b;
                    merged.push_back(std::move(src.Front()));
                    src.Advance();
                    EmitMerged(merged, *queues[out_idx]);

                    if (src.Empty())
                        src.Refill();
                }

                if (!merged.empty())
                    queues[out_idx]->Push(std::move(merged));
                queues[out_idx]->MarkDone(); });
        }

        if (current_level.size() % 2 == 1)
            next_level.push_back(current_level.back());

        current_level = std::move(next_level);
    }

    const std::size_t root_idx = current_level[0];
    threads.emplace_back([&queues, root_idx, &on_event]()
                         { DispatcherLoop(*queues[root_idx], on_event); });
}
