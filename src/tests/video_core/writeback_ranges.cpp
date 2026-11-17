// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/writeback_ranges.h"

using VideoCommon::WritebackRanges::AppendLanded;
using VideoCommon::WritebackRanges::AppendNewerLanded;
using VideoCommon::WritebackRanges::CapPageRecords;
using VideoCommon::WritebackRanges::ForEachLandedPart;
using VideoCommon::WritebackRanges::ForEachUnkept;
using VideoCommon::WritebackRanges::KeepNewerBytes;
using VideoCommon::WritebackRanges::MaxTickedRanges;
using VideoCommon::WritebackRanges::MaxPageRecords;
using VideoCommon::WritebackRanges::MergeRanges;
using VideoCommon::WritebackRanges::NoteTick;
using VideoCommon::WritebackRanges::PageRecords;
using VideoCommon::WritebackRanges::Segment;
using VideoCommon::WritebackRanges::TickedRange;
using VideoCommon::WritebackRanges::TickedRanges;
using VideoCommon::WritebackRanges::TrimOverwritten;

namespace {
using Range = std::pair<DAddr, DAddr>;

/// The bytes a guest read's download of an image at modification tick `tick` leaves as they are,
/// as DownloadMemory takes them from the landed ranges.
std::vector<Range> Kept(const std::vector<TickedRange>& landed, u64 tick) {
    std::vector<Range> kept;
    for (const TickedRange& range : landed) {
        if (range.tick > tick) {
            kept.emplace_back(range.begin, range.end);
        }
    }
    return kept;
}
} // Anonymous namespace

TEST_CASE("A landed write-back's kept ranges keep the tick of the data they hold",
          "[video_core][texture_cache][writeback]") {
    // A write-back at tick 5 of [0x1000, 0x2000) left [0x1400, 0x1600) to an image flushed at
    // tick 9.
    const std::vector<Range> keep{{0x1400, 0x1600}};
    const TickedRanges kept{{0x1400, 0x1600, 9}};
    std::vector<TickedRange> landed;
    AppendLanded(landed, 0x1000, 0x1000, 5, keep, kept);

    // An image modified at tick 7 is newer than the write-back but older than the flushed bytes.
    CHECK(Kept(landed, 7) == std::vector<Range>{{0x1400, 0x1600}});
    // Older than both, it leaves all of it; newer than both, none.
    CHECK(Kept(landed, 3) ==
          std::vector<Range>{{0x1000, 0x1400}, {0x1600, 0x2000}, {0x1400, 0x1600}});
    CHECK(Kept(landed, 10).empty());
}

TEST_CASE("A landed write-back's own bytes are the ones it did not keep",
          "[video_core][texture_cache][writeback]") {
    // Kept ranges in any order, overlapping, and one at each end.
    const std::vector<Range> keep{
        {0x1800, 0x1900}, {0x1100, 0x1300}, {0x1200, 0x1400}, {0x1000, 0x1080}, {0x1f00, 0x2000}};
    std::vector<TickedRange> landed;
    AppendLanded(landed, 0x1000, 0x1000, 5, keep, TickedRanges{});
    CHECK(Kept(landed, 4) == std::vector<Range>{{0x1080, 0x1100}, {0x1400, 0x1800},
                                                {0x1900, 0x1f00}});
}

namespace {
/// A write-back waiting for the GPU, as the texture cache keeps them.
struct WaitingDownload {
    DAddr cpu_addr;
    u64 size;
    u64 tick;
    std::vector<Range> keep;
    TickedRanges keep_ticks;
};
} // Anonymous namespace

TEST_CASE("A waiting write-back's kept bytes take the tick of a flush no newer than it",
          "[video_core][texture_cache][writeback]") {
    // A write-back at tick 20 of [0x1000, 0x2000) keeps [0x1400, 0x1500), which a smaller image
    // wrote back at tick 9 after the written-back image loaded.
    WaitingDownload download{0x1000, 0x1000, 20, {{0x1400, 0x1500}}, {{0x1400, 0x1500, 9}}};
    // An image at tick 15 is flushed over part of it.
    KeepNewerBytes(download, 0x1300, 0x1480, 15);
    CHECK(download.keep == std::vector<Range>{{0x1400, 0x1500}});
    std::vector<TickedRange> landed;
    AppendLanded(landed, download.cpu_addr, download.size, download.tick, download.keep,
                 download.keep_ticks);
    // A guest read's download of an image at tick 12 leaves the flushed bytes there as they are,
    // and writes the rest of the kept range.
    CHECK(Kept(landed, 12) ==
          std::vector<Range>{{0x1000, 0x1400}, {0x1500, 0x2000}, {0x1400, 0x1480}});
}

TEST_CASE("A waiting write-back keeps only the bytes of a flush newer than it",
          "[video_core][texture_cache][writeback]") {
    WaitingDownload download{0x1000, 0x1000, 20, {}, {}};
    // No newer than it, outside its kept ranges: the write-back lands over them.
    KeepNewerBytes(download, 0x1800, 0x1900, 15);
    CHECK(download.keep.empty());
    CHECK(download.keep_ticks.empty());
    // Newer, over its end: kept within it, at the flushed image's tick.
    KeepNewerBytes(download, 0x1f00, 0x2100, 25);
    CHECK(download.keep == std::vector<Range>{{0x1f00, 0x2000}});
    REQUIRE(download.keep_ticks.size() == 1);
    CHECK(download.keep_ticks.front().tick == 25);
}

TEST_CASE("Ticked ranges keep the newest data's tick", "[video_core][texture_cache][writeback]") {
    TickedRanges ranges;
    NoteTick(ranges, 0x1000, 0x2000, 9);
    // Inside it at an older tick: already held at a newer one.
    NoteTick(ranges, 0x1400, 0x1600, 7);
    REQUIRE(ranges.size() == 1);
    // The same bytes written again (a guest write after the cache's tick moved): only the newer.
    NoteTick(ranges, 0x1000, 0x2000, 12);
    REQUIRE(ranges.size() == 1);
    CHECK(ranges.front().tick == 12);
    // An empty range notes nothing.
    NoteTick(ranges, 0x3000, 0x3000, 20);
    CHECK(ranges.size() == 1);
}

TEST_CASE("Ticked ranges past the limit merge without losing any byte or a newer tick",
          "[video_core][texture_cache][writeback]") {
    TickedRanges ranges;
    for (u64 index = 0; index <= MaxTickedRanges; ++index) {
        // Disjoint ranges, the last two closest together.
        const DAddr begin = index == MaxTickedRanges ? 0x100'0000 + (index - 1) * 0x1000 + 0x900
                                                     : 0x100'0000 + index * 0x1000;
        NoteTick(ranges, begin, begin + 0x800, 10 + index);
    }
    REQUIRE(ranges.size() == MaxTickedRanges);
    for (u64 index = 0; index <= MaxTickedRanges; ++index) {
        const DAddr begin = index == MaxTickedRanges ? 0x100'0000 + (index - 1) * 0x1000 + 0x900
                                                     : 0x100'0000 + index * 0x1000;
        const bool held = std::ranges::any_of(ranges, [&](const TickedRange& range) {
            return range.begin <= begin && begin + 0x800 <= range.end && range.tick >= 10 + index;
        });
        CHECK(held);
    }
}

namespace {
/// A write-back record as the texture cache keeps them per page.
struct Record {
    DAddr begin;
    DAddr end;
    u64 tick;
};

bool HasRecord(const std::vector<Record>& records, DAddr begin, DAddr end, u64 tick) {
    return std::ranges::any_of(records, [&](const Record& record) {
        return record.begin == begin && record.end == end && record.tick == tick;
    });
}
} // Anonymous namespace

TEST_CASE("A page's write-back records keep the bytes only the oldest holds",
          "[video_core][texture_cache][writeback]") {
    constexpr DAddr page = 0x1000;
    // The oldest record alone holds [0x1000, 0x1100); eight newer ones hold the rest of the page.
    std::vector<Record> records{{0x1000, 0x1100, 1}};
    for (u64 index = 0; index < PageRecords; ++index) {
        const DAddr begin = 0x1100 + index * 0x100;
        records.push_back(Record{begin, begin + 0x100, index + 2});
    }
    CapPageRecords(records, page, page + 0x1000);
    CHECK(records.size() == PageRecords + 1);
    CHECK(HasRecord(records, 0x1000, 0x1100, 1));
}

TEST_CASE("A page's write-back records drop the oldest that newer ones hold all of",
          "[video_core][texture_cache][writeback]") {
    constexpr DAddr page = 0x1000;
    // Tick 1 is held by two newer records together, neither on its own; its bytes on the page
    // before are not this page's. Tick 3 is held only by the older tick 2: it stays.
    std::vector<Record> records{{0x0800, 0x1200, 1}, {0x1000, 0x1100, 5}, {0x1100, 0x1200, 6},
                                {0x1800, 0x1a00, 2}, {0x1800, 0x1900, 3}};
    for (u64 index = 0; records.size() <= PageRecords; ++index) {
        const DAddr begin = 0x1c00 + index * 0x80;
        records.push_back(Record{begin, begin + 0x80, index + 10});
    }
    CapPageRecords(records, page, page + 0x1000);
    CHECK(records.size() == PageRecords);
    CHECK_FALSE(HasRecord(records, 0x0800, 0x1200, 1));
    CHECK(HasRecord(records, 0x1800, 0x1900, 3));
    CHECK(HasRecord(records, 0x1800, 0x1a00, 2));
}

namespace {
/// A write-back record with whether its data was lost, as the texture cache keeps them per page.
struct LandedRecord {
    DAddr begin;
    DAddr end;
    u64 tick;
    bool lost;
};
} // Anonymous namespace

TEST_CASE("A flush's snapshot leaves the bytes newer landed write-backs hold",
          "[video_core][texture_cache][writeback]") {
    // A snapshot at tick 20 of [0x1000, 0x2000).
    const std::vector<LandedRecord> records{
        {0x1000, 0x1400, 30, false}, // newer, landed
        {0x1300, 0x1500, 25, false}, // newer, overlapping the one before
        {0x1400, 0x1600, 10, false}, // older
        {0x1600, 0x1700, 30, true},  // newer, but its data is gone
        {0x1700, 0x1800, 30, false}, // newer, still waiting
        {0x1800, 0x2400, 30, false}, // newer, past the snapshot's end
        {0x3000, 0x3100, 40, false}, // newer, elsewhere
    };
    const auto waiting = [](const LandedRecord& record) { return record.begin == 0x1700; };
    std::vector<Range> keep;
    // A record over two pages is in both pages' records.
    AppendNewerLanded(keep, records, 0x1000, 0x2000, 20, waiting);
    AppendNewerLanded(keep, records, 0x1000, 0x2000, 20, waiting);
    MergeRanges(keep);
    CHECK(keep == std::vector<Range>{{0x1000, 0x1500}, {0x1800, 0x2000}});
}

TEST_CASE("A flush's snapshot is written whole without newer data over it",
          "[video_core][texture_cache][writeback]") {
    const std::vector<LandedRecord> records{{0x1000, 0x2000, 20, false},
                                            {0x1400, 0x1800, 5, false}};
    std::vector<Range> keep;
    AppendNewerLanded(keep, records, 0x1000, 0x2000, 20,
                      [](const LandedRecord&) { return false; });
    MergeRanges(keep);
    CHECK(keep.empty());
}

TEST_CASE("A page's write-back records drop the oldest past the hard limit",
          "[video_core][texture_cache][writeback]") {
    constexpr DAddr page = 0x1000;
    std::vector<Record> records;
    for (u64 index = 0; index <= MaxPageRecords; ++index) {
        const DAddr begin = page + index * 0x40;
        records.push_back(Record{begin, begin + 0x40, index + 1});
    }
    CapPageRecords(records, page, page + 0x1000);
    CHECK(records.size() == MaxPageRecords);
    CHECK_FALSE(HasRecord(records, page, page + 0x40, 1));
}

namespace {
/// A page's write-back record with the write-back it is part of, as the texture cache keeps them.
struct SourcedRecord {
    DAddr begin;
    DAddr end;
    u64 tick;
    bool lost;
    DAddr source_begin;
    DAddr source_end;
};

/// As NoteOverlapWriteback on a page [0x1000, 0x2000): records of the bytes the write-back
/// wrote, and those of older ones taken out of them.
void NoteWriteback(std::vector<SourcedRecord>& records, DAddr begin, DAddr end, u64 tick,
                   const std::vector<Range>& keep) {
    TrimOverwritten(records, begin, end, tick, keep, 0x1000, 0x2000);
    ForEachUnkept(begin, end, keep, [&](DAddr written_begin, DAddr written_end) {
        records.push_back(SourcedRecord{written_begin, written_end, tick, false, begin, end});
    });
}
} // Anonymous namespace

TEST_CASE("A write-back's records leave the bytes it kept to their data's tick",
          "[video_core][texture_cache][writeback]") {
    // A large image loaded at tick 5 and modified at tick 20 keeps a smaller image's bytes
    // written back at tick 9 (after it loaded) and a newer image's at tick 30.
    std::vector<SourcedRecord> records{
        {0x1400, 0x1500, 9, false, 0x1400, 0x1500},  // kept, older
        {0x1800, 0x1900, 4, false, 0x1800, 0x1900},  // written over
        {0x1c00, 0x1d00, 30, false, 0x1c00, 0x1d00}, // kept, newer
        {0x0800, 0x1100, 3, false, 0x0800, 0x1100},  // past its start: not all written over
    };
    NoteWriteback(records, 0x1000, 0x2000, 20, {{0x1400, 0x1500}, {0x1c00, 0x1d00}});
    const auto has = [&](DAddr begin, DAddr end, u64 tick) {
        return std::ranges::any_of(records, [&](const SourcedRecord& record) {
            return record.begin == begin && record.end == end && record.tick == tick;
        });
    };
    CHECK(has(0x1400, 0x1500, 9));
    CHECK_FALSE(has(0x1800, 0x1900, 4));
    CHECK(has(0x1c00, 0x1d00, 30));
    CHECK(has(0x0800, 0x1100, 3));
    CHECK(has(0x1000, 0x1400, 20));
    CHECK(has(0x1500, 0x1c00, 20));
    CHECK(has(0x1d00, 0x2000, 20));
    CHECK_FALSE(has(0x1000, 0x2000, 20));

    // A flush's snapshot at tick 15 that pops after it landed writes its bytes over the tick-9
    // data: the write-back did not put tick-20 data there.
    std::vector<Range> keep;
    const auto landed = [](const SourcedRecord&) { return false; };
    AppendNewerLanded(keep, records, 0x1000, 0x2000, 15, landed);
    MergeRanges(keep);
    CHECK(keep == std::vector<Range>{{0x1000, 0x1400}, {0x1500, 0x2000}});

    // Still waiting, every record of it is found by the write-back's range: only the newer
    // image's bytes stay.
    keep.clear();
    AppendNewerLanded(keep, records, 0x1000, 0x2000, 15, [](const SourcedRecord& record) {
        return record.source_begin == 0x1000 && record.source_end == 0x2000 &&
               record.tick == 20;
    });
    MergeRanges(keep);
    CHECK(keep == std::vector<Range>{{0x1c00, 0x1d00}});
}

TEST_CASE("A write-back without kept bytes supersedes every older record within it",
          "[video_core][texture_cache][writeback]") {
    std::vector<SourcedRecord> records{{0x1400, 0x1500, 9, false, 0x1400, 0x1500},
                                       {0x1000, 0x2000, 12, true, 0x1000, 0x2000}};
    NoteWriteback(records, 0x1000, 0x2000, 20, {});
    REQUIRE(records.size() == 1);
    CHECK(records.front().begin == 0x1000);
    CHECK(records.front().end == 0x2000);
    CHECK(records.front().tick == 20);
}

TEST_CASE("An older record the write-back kept part of keeps that part",
          "[video_core][texture_cache][writeback]") {
    // [0x1300, 0x1480) at tick 8, lost, of an image over [0x1300, 0x1480); the write-back at tick
    // 20 keeps [0x1400, 0x1500) for a newer image's bytes.
    std::vector<SourcedRecord> records{{0x1300, 0x1480, 8, true, 0x1300, 0x1480},
                                       {0x1400, 0x1500, 30, false, 0x1400, 0x1500}};
    NoteWriteback(records, 0x1000, 0x2000, 20, {{0x1400, 0x1500}});
    const auto part = std::ranges::find_if(records, [](const SourcedRecord& record) {
        return record.tick == 8;
    });
    REQUIRE(part != records.end());
    CHECK(part->begin == 0x1400);
    CHECK(part->end == 0x1480);
    CHECK(part->lost);
    CHECK(part->source_begin == 0x1300);
    CHECK(part->source_end == 0x1480);
    CHECK(std::ranges::count_if(records, [](const SourcedRecord& r) { return r.tick == 8; }) == 1);
}

TEST_CASE("An older record over two pages keeps on each only its kept part there",
          "[video_core][texture_cache][writeback]") {
    // A record over [0x1f00, 0x2200), a write-back of [0x1000, 0x3000) keeping [0x1f80, 0x2100).
    const SourcedRecord record{0x1f00, 0x2200, 4, false, 0x1f00, 0x2200};
    const std::vector<Range> keep{{0x1f80, 0x2100}};
    std::vector<SourcedRecord> first{record};
    std::vector<SourcedRecord> second{record};
    TrimOverwritten(first, 0x1000, 0x3000, 20, keep, 0x1000, 0x2000);
    TrimOverwritten(second, 0x1000, 0x3000, 20, keep, 0x2000, 0x3000);
    REQUIRE(first.size() == 1);
    CHECK(first.front().begin == 0x1f80);
    CHECK(first.front().end == 0x2000);
    REQUIRE(second.size() == 1);
    CHECK(second.front().begin == 0x2000);
    CHECK(second.front().end == 0x2100);
}

TEST_CASE("A landing writes what the swizzle wrote but newer data, through each segment",
          "[video_core][texture_cache][writeback]") {
    // The image's first 4 KiB at device 0x10000, the next at 0x30000.
    const std::vector<Segment> segments{{0x0, 0x10000, 0x1000}, {0x1000, 0x30000, 0x1000}};
    const std::vector<std::pair<u64, u64>> written{{0x0, 0x800}, {0xc00, 0x1400}};
    // A guest write in the first segment and a kept range at the second's start.
    const std::vector<Range> skip{{0x10200, 0x10300}, {0x30000, 0x30100}};
    struct Part {
        DAddr device;
        u64 offset;
        u64 size;
        bool operator==(const Part&) const = default;
    };
    std::vector<Part> parts;
    ForEachLandedPart(segments, written, skip, [&](DAddr device, u64 offset, u64 size) {
        parts.push_back(Part{device, offset, size});
    });
    CHECK(parts == std::vector<Part>{{0x10000, 0x0, 0x200},
                                     {0x10300, 0x300, 0x500},
                                     {0x10c00, 0xc00, 0x400},
                                     {0x30100, 0x1100, 0x300}});
}
