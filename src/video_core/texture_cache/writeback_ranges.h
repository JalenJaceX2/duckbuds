// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <utility>

#include <boost/container/small_vector.hpp>

#include "common/common_types.h"

/// The ranges of guest memory a write-back of a GPU-written overlap leaves to newer data, and the
/// modification ticks of the data each range holds.
namespace VideoCommon::WritebackRanges {

/// A range [begin, end) of guest memory holding data of modification tick `tick`.
struct TickedRange {
    DAddr begin;
    DAddr end;
    u64 tick;
};
using TickedRanges = boost::container::small_vector<TickedRange, 4>;

/// Ranges past which the ticked ranges of one write-back are merged.
inline constexpr size_t MaxTickedRanges = 32;

/// Notes that [begin, end) holds data of modification tick `tick`: nothing when a range noted
/// before holds all of it at a tick at least as new, and the ranges it holds all of at a tick no
/// newer go. Past MaxTickedRanges, overlapping and touching ranges are merged, then the closest
/// ones, each merged range at the newest of their ticks (it may then protect bytes between them).
inline void NoteTick(TickedRanges& ranges, DAddr begin, DAddr end, u64 tick) {
    if (begin >= end || std::ranges::any_of(ranges, [&](const TickedRange& range) {
            return range.begin <= begin && end <= range.end && range.tick >= tick;
        })) {
        return;
    }
    ranges.erase(std::remove_if(ranges.begin(), ranges.end(),
                                [&](const TickedRange& range) {
                                    return begin <= range.begin && range.end <= end &&
                                           range.tick <= tick;
                                }),
                 ranges.end());
    ranges.push_back(TickedRange{begin, end, tick});
    if (ranges.size() <= MaxTickedRanges) {
        return;
    }
    std::ranges::sort(ranges, {}, &TickedRange::begin);
    TickedRanges merged;
    for (const TickedRange& range : ranges) {
        if (!merged.empty() && range.begin <= merged.back().end) {
            merged.back().end = (std::max)(merged.back().end, range.end);
            merged.back().tick = (std::max)(merged.back().tick, range.tick);
        } else {
            merged.push_back(range);
        }
    }
    while (merged.size() > MaxTickedRanges) {
        size_t closest = 0;
        for (size_t index = 1; index + 1 < merged.size(); ++index) {
            if (merged[index + 1].begin - merged[index].end <
                merged[closest + 1].begin - merged[closest].end) {
                closest = index;
            }
        }
        merged[closest].end = merged[closest + 1].end;
        merged[closest].tick = (std::max)(merged[closest].tick, merged[closest + 1].tick);
        merged.erase(merged.begin() + static_cast<std::ptrdiff_t>(closest) + 1);
    }
    ranges = std::move(merged);
}

/// Appends to `landed` what a write-back of [cpu_addr, cpu_addr + size) at modification tick
/// `tick` left in guest memory: its own bytes (all but the `keep` ranges it left as they were) at
/// its tick, and each range of `kept` at the tick of the data that range holds.
template <typename Landed, typename Keep, typename Kept>
void AppendLanded(Landed& landed, DAddr cpu_addr, u64 size, u64 tick, const Keep& keep,
                  const Kept& kept) {
    boost::container::small_vector<std::pair<DAddr, DAddr>, 4> sorted(keep.begin(), keep.end());
    std::ranges::sort(sorted);
    const DAddr end = cpu_addr + size;
    DAddr cursor = cpu_addr;
    for (const auto& [keep_begin, keep_end] : sorted) {
        if (keep_begin > cursor && cursor < end) {
            landed.push_back(TickedRange{cursor, (std::min)(keep_begin, end), tick});
        }
        cursor = (std::max)(cursor, keep_end);
    }
    if (cursor < end) {
        landed.push_back(TickedRange{cursor, end, tick});
    }
    for (const TickedRange& range : kept) {
        landed.push_back(range);
    }
}

} // namespace VideoCommon::WritebackRanges
