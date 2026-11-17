// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace VideoCommon {

[[nodiscard]] constexpr bool UseOverlapWritebacks(u64 program_id) {
    return program_id == 0x0400D1D02399A000ULL || program_id == 0x0400486024DEC000ULL;
}

} // namespace VideoCommon
