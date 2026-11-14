// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/writeback_ranges.h"

using VideoCommon::WritebackRanges::AppendLanded;
using VideoCommon::WritebackRanges::MaxTickedRanges;
using VideoCommon::WritebackRanges::NoteTick;
using VideoCommon::WritebackRanges::TickedRange;
using VideoCommon::WritebackRanges::TickedRanges;

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
