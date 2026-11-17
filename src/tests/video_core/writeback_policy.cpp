// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/writeback_policy.h"

TEST_CASE("Overlap write-backs apply to the affected titles", "[video_core][texture_cache][writeback]") {
    REQUIRE(VideoCommon::UseOverlapWritebacks(0x0400D1D02399A000ULL));
    REQUIRE(VideoCommon::UseOverlapWritebacks(0x0400486024DEC000ULL));
    REQUIRE_FALSE(VideoCommon::UseOverlapWritebacks(0));
    REQUIRE_FALSE(VideoCommon::UseOverlapWritebacks(0x040074A01BF12000ULL));
}
