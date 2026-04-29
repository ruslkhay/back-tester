#include "ingestion/DataIngestion.hpp"
#include "common/MarketDataParser.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <unistd.h>
#include <vector>

// Helper to generate a minimal valid NDJSON line
static auto MakeNDJSON(uint64_t ts_event_ns, uint64_t ts_recv_ns, uint64_t order_id) -> std::string
{
    auto ts_event_iso = nanosToIso(ts_event_ns);
    auto ts_recv_iso = nanosToIso(ts_recv_ns);

    std::ostringstream oss;
    oss << R"({"ts_recv":")" << ts_recv_iso << R"(","hd":{"ts_event":")" << ts_event_iso
        << R"(","rtype":160,"publisher_id":1,"instrument_id":100},"action":"A","side":"B","price":"10000000000","size":10,"channel_id":0,"order_id":")"
        << order_id << R"(","flags":0,"ts_in_delta":100})";
    return oss.str();
}

// Helper to write NDJSON lines to a temp file
static auto WriteTempNDJSON(const std::vector<std::string>& lines) -> std::string
{
    // Create template for mkstemp in system temp directory (XXXXXX must be at end)
    std::string temp_template = std::filesystem::temp_directory_path().string() + "/ndjson_XXXXXX";
    std::vector<char> temp_path(temp_template.begin(), temp_template.end());
    temp_path.push_back('\0');

    // mkstemp creates the file securely and returns a file descriptor
    int fd = mkstemp(temp_path.data());
    if (fd == -1)
    {
        throw std::runtime_error("Failed to create temporary file");
    }

    std::string temp_filename(temp_path.data());

    // Convert file descriptor to FILE* for writing
    FILE* cfile = fdopen(fd, "w");
    if (!cfile)
    {
        close(fd);
        throw std::runtime_error("Failed to open file descriptor");
    }

    for (const auto& line : lines)
    {
        fprintf(cfile, "%s\n", line.c_str());
    }

    fclose(cfile);

    // Rename to add .json extension
    std::string final_filename = temp_filename + ".json";
    std::filesystem::rename(temp_filename, final_filename);

    return final_filename;
}

// Helper to clean up temp file
static void RemoveTempFile(const std::string& filename)
{
    std::filesystem::remove(filename);
}

TEST_CASE("DataIngestion - FlatMerger single file", "[DataIngestion][FlatMerger]")
{
    std::vector<std::string> ndjson;
    size_t numEvents = 50;
    uint64_t ts = 1000;
    for (size_t i = 0; i < numEvents; ++i)
    {
        ts += i * 100; // 100ms apart
        auto file = MakeNDJSON(ts, ts + 5, i);
        ndjson.push_back(file);
    }

    std::string filename = WriteTempNDJSON(ndjson);

    std::vector<MarketDataEvent> events;
    events.reserve(numEvents);
    FlatMergerEngine engine;
    engine.Ingest({filename}, [&](const MarketDataEvent& e)
                  { events.push_back(e); });

    REQUIRE(events.size() == numEvents);
    // Check that events are in chronological order
    for (std::size_t i = 0; i < events.size() - 1; ++i)
    {
        REQUIRE(events[i].ts_event <= events[i + 1].ts_event);
    }
    RemoveTempFile(filename);
}

TEST_CASE("DataIngestion - FlatMerger two interleaved files", "[DataIngestion][FlatMerger]")
{
    // File 1: timestamps 1000, 3000, 5000, 7000, 9000
    std::vector<std::string> file1_ndjson;
    size_t numEvents = 50;
    uint64_t ts = 1000;
    for (size_t i = 0; i < numEvents; ++i)
    {
        file1_ndjson.push_back(MakeNDJSON(ts, ts + 5, i));
        ts += 2000; // 2000ms apart
    }

    // File 2: timestamps 2000, 4000, 6000, 8000, 10000
    std::vector<std::string> file2_ndjson;
    ts = 2000;
    for (size_t i = 0; i < numEvents; ++i)
    {
        file2_ndjson.push_back(MakeNDJSON(ts, ts + 5, 20000 + i));
        ts += 2000; // 2000ms apart
    }

    std::string file1 = WriteTempNDJSON(file1_ndjson);
    std::string file2 = WriteTempNDJSON(file2_ndjson);

    std::vector<MarketDataEvent> events;
    events.reserve(2 * numEvents);
    FlatMergerEngine engine;
    engine.Ingest({file1, file2}, [&](const MarketDataEvent& e)
                  { events.push_back(e); });

    REQUIRE(events.size() == 2 * numEvents);
    for (std::size_t i = 0; i < events.size() - 1; ++i)
    {
        REQUIRE(events[i].ts_event <= events[i + 1].ts_event);
    }

    RemoveTempFile(file1);
    RemoveTempFile(file2);
}

TEST_CASE("DataIngestion - FlatMerger empty file", "[DataIngestion][FlatMerger]")
{
    std::vector<std::string> file1_ndjson;
    std::vector<std::string> file2_ndjson{
        MakeNDJSON(1000, 1001, 1000),
        MakeNDJSON(2000, 2001, 1001),
        MakeNDJSON(3000, 3001, 1002)};

    std::string file1 = WriteTempNDJSON(file1_ndjson);
    std::string file2 = WriteTempNDJSON(file2_ndjson);

    std::vector<MarketDataEvent> events;
    FlatMergerEngine engine;
    engine.Ingest({file1, file2}, [&](const MarketDataEvent& e)
                  { events.push_back(e); });

    REQUIRE(events.size() == 3);

    RemoveTempFile(file1);
    RemoveTempFile(file2);
}

TEST_CASE("DataIngestion - HierarchyMerger multiple files", "[DataIngestion][HierarchyMerger]")
{
    // Create 4 files with interleaved timestamps
    std::vector<std::string> files;
    size_t numFiles = 40, numEventsPerFile = 500;
    for (size_t f = 0; f < numFiles; ++f)
    {
        std::vector<std::string> ndjson;
        for (size_t i = 0; i < numEventsPerFile; ++i)
        {
            uint64_t ts = 1000 + (f * numEventsPerFile + i) * 500;
            ndjson.push_back(MakeNDJSON(ts, ts, 10000 + f * 1000 + i));
        }
        files.push_back(WriteTempNDJSON(ndjson));
    }

    std::vector<MarketDataEvent> events;
    HierarchyMergerEngine engine;
    engine.Ingest(files, [&](const MarketDataEvent& e)
                  { events.push_back(e); });

    REQUIRE(events.size() == numFiles * numEventsPerFile);
    for (std::size_t i = 0; i < events.size() - 1; ++i)
    {
        REQUIRE(events[i].ts_event <= events[i + 1].ts_event);
    }

    for (const auto& file : files)
    {
        RemoveTempFile(file);
    }
}

TEST_CASE("DataIngestion - HierarchyMerger three files (odd)", "[DataIngestion][HierarchyMerger]")
{
    // Create 3 files (odd number) with interleaved timestamps
    std::vector<std::string> files;
    for (int f = 0; f < 3; ++f)
    {
        std::vector<std::string> ndjson;
        for (int i = 0; i < 4; ++i)
        {
            uint64_t ts = 1000 + (f * 4 + i) * 333;
            ndjson.push_back(MakeNDJSON(ts, ts, 20000 + f * 1000 + i));
        }
        files.push_back(WriteTempNDJSON(ndjson));
    }

    std::vector<MarketDataEvent> events;
    HierarchyMergerEngine engine;
    engine.Ingest(files, [&](const MarketDataEvent& e)
                  { events.push_back(e); });

    REQUIRE(events.size() == 12);
    for (std::size_t i = 0; i < events.size() - 1; ++i)
    {
        REQUIRE(events[i].ts_event <= events[i + 1].ts_event);
    }

    for (const auto& file : files)
    {
        RemoveTempFile(file);
    }
}

TEST_CASE("DataIngestion - Both strategies event count", "[DataIngestion]")
{
    // Create 2 files with known event counts
    std::vector<std::string> file1_ndjson{
        MakeNDJSON(1000, 1000, 1000),
        MakeNDJSON(2000, 2000, 1001),
        MakeNDJSON(3000, 3000, 1002)};
    std::vector<std::string> file2_ndjson{
        MakeNDJSON(1500, 1500, 2000),
        MakeNDJSON(2500, 2500, 2001),
        MakeNDJSON(3500, 3500, 2002),
        MakeNDJSON(4000, 4000, 2003)};

    std::string file1 = WriteTempNDJSON(file1_ndjson);
    std::string file2 = WriteTempNDJSON(file2_ndjson);

    // Test FlatMerger
    std::vector<MarketDataEvent> flat_events;
    FlatMergerEngine flat_engine;
    flat_engine.Ingest({file1, file2}, [&](const MarketDataEvent& e)
                       { flat_events.push_back(e); });
    REQUIRE(flat_events.size() == 7);

    // Test HierarchyMerger
    std::vector<MarketDataEvent> hier_events;
    HierarchyMergerEngine hier_engine;
    hier_engine.Ingest({file1, file2}, [&](const MarketDataEvent& e)
                       { hier_events.push_back(e); });
    REQUIRE(hier_events.size() == 7);

    // Check that the events are the same
    for (size_t i = 0; i < flat_events.size(); ++i)
    {
        REQUIRE(flat_events[i].ts_event == hier_events[i].ts_event);
        REQUIRE(flat_events[i].ts_recv == hier_events[i].ts_recv);
        REQUIRE(flat_events[i].order_id == hier_events[i].order_id);
    }

    RemoveTempFile(file1);
    RemoveTempFile(file2);
}

TEST_CASE("DataIngestion - FlatMerger validation against merged.mbo.ndjson", "[DataIngestion][Validation]")
{
    // Load test data directory
    std::string test_dir = "../../test/data/T6R_407/1000_counts";
    std::string merged_file = "../../test/data/T6R_407/merged.mbo.ndjson";

    // Check if test data exists
    if (!std::filesystem::exists(test_dir) || !std::filesystem::exists(merged_file))
    {
        SKIP("Test data directory or merged file not found");
    }

    // Collect files from 1000_counts directory
    std::vector<std::string> test_files;
    try
    {
        for (const auto& entry : std::filesystem::directory_iterator(test_dir))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".ndjson")
            {
                test_files.push_back(entry.path().string());
            }
        }
        std::sort(test_files.begin(), test_files.end());
    }
    catch (const std::filesystem::filesystem_error&)
    {
        SKIP("Could not read test data directory");
    }

    REQUIRE(test_files.size() == 10);

    // Merge using FlatMerger
    std::vector<MarketDataEvent> merged_events;
    FlatMergerEngine engine;
    engine.Ingest(test_files, [&](const MarketDataEvent& e)
                  { merged_events.push_back(e); });

    // Read reference merged file
    std::vector<MarketDataEvent> expected_events;
    std::ifstream ref_file{merged_file};
    std::string line;
    while (std::getline(ref_file, line))
    {
        auto event = parseNDJSON(line);
        if (event)
        {
            expected_events.push_back(*event);
        }
    }

    // Compare event counts
    REQUIRE(merged_events.size() == expected_events.size());
    INFO("FlatMerger: Merged " << merged_events.size() << " events");

    // Verify chronological ordering and critical fields
    for (std::size_t i = 0; i < merged_events.size(); ++i)
    {
        const auto& event = merged_events[i];

        // Verify strict chronological order
        if (i > 0)
        {
            REQUIRE(event.ts_event >= merged_events[i - 1].ts_event);
        }
    }

    // Verify all events from both sources contain the same set of order_ids
    // (event order may differ for events with same timestamp, but all events must be present)
    std::set<std::uint64_t> merged_ids;
    std::set<std::uint64_t> expected_ids;
    for (const auto& e : merged_events)
        merged_ids.insert(e.order_id);
    for (const auto& e : expected_events)
        expected_ids.insert(e.order_id);

    REQUIRE(merged_ids == expected_ids);
}

TEST_CASE("DataIngestion - HierarchyMerger validation against merged.mbo.ndjson", "[DataIngestion][Validation]")
{
    // Load test data directory
    std::string test_dir = "../../test/data/T6R_407/1000_counts";
    std::string merged_file = "../../test/data/T6R_407/merged.mbo.ndjson";

    // Check if test data exists
    if (!std::filesystem::exists(test_dir))
    {
        SKIP("Test data directory not found");
    }
    if (!std::filesystem::exists(merged_file))
    {
        SKIP("Merged file not found");
    }

    // Collect files from 1000_counts directory
    std::vector<std::string> test_files;
    try
    {
        for (const auto& entry : std::filesystem::directory_iterator(test_dir))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".ndjson")
            {
                test_files.push_back(entry.path().string());
            }
        }
        std::sort(test_files.begin(), test_files.end());
    }
    catch (const std::filesystem::filesystem_error&)
    {
        SKIP("Could not read test data directory");
    }

    REQUIRE(test_files.size() == 10);

    // Merge using HierarchyMerger
    std::vector<MarketDataEvent> merged_events;
    HierarchyMergerEngine engine;
    engine.Ingest(test_files, [&](const MarketDataEvent& e)
                  { merged_events.push_back(e); });

    // Read reference merged file
    std::vector<MarketDataEvent> expected_events;
    std::ifstream ref_file{merged_file};
    std::string line;
    while (std::getline(ref_file, line))
    {
        auto event = parseNDJSON(line);
        if (event)
        {
            expected_events.push_back(*event);
        }
    }

    // Compare event counts
    REQUIRE(merged_events.size() == expected_events.size());
    INFO("HierarchyMerger: Merged " << merged_events.size() << " events");

    // Verify chronological ordering and critical fields
    for (std::size_t i = 0; i < merged_events.size(); ++i)
    {
        const auto& event = merged_events[i];

        // Verify strict chronological order
        if (i > 0)
        {
            REQUIRE(event.ts_event >= merged_events[i - 1].ts_event);
        }
    }

    // Verify all events from both sources contain the same set of order_ids
    // (event order may differ for events with same timestamp, but all events must be present)
    std::set<std::uint64_t> merged_ids;
    std::set<std::uint64_t> expected_ids;
    for (const auto& e : merged_events)
        merged_ids.insert(e.order_id);
    for (const auto& e : expected_events)
        expected_ids.insert(e.order_id);

    REQUIRE(merged_ids == expected_ids);
}
