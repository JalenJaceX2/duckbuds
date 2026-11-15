// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <condition_variable>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/svc.h"
#include "core/memory.h"

namespace Kernel::Svc {

/// Used to output a message on a debug hardware unit - does nothing on a retail unit
Result OutputDebugString(Core::System& system, u64 address, u64 len) {
    R_SUCCEED_IF(len == 0);
    std::string msg_buffer(len, 0);
    GetCurrentMemory(system.Kernel()).ReadBlock(address, msg_buffer.data(), len);
    // The Duskbloods' fatal reason ('Allocate error heapName = M', DL_PANIC) likely comes here before
    // its handler writes 0xdeadba.. to address 0. The default log filter (Debug:Error) hides Info,
    // so such lines are logged as errors for that title.
    const auto* const process = system.ApplicationProcess();
    const bool fatal = process != nullptr && process->GetProgramId() == 0x0400D1D02399A000ULL &&
                       (msg_buffer.find("PANIC") != std::string::npos ||
                        msg_buffer.find("rror") != std::string::npos ||
                        msg_buffer.find("bort") != std::string::npos ||
                        msg_buffer.find("ssert") != std::string::npos);
    if (fatal) {
        LOG_ERROR(Debug_Emulated, "guest: {}", msg_buffer);
    } else {
        LOG_INFO(Debug_Emulated, "{}", msg_buffer);
    }
    R_SUCCEED();
}

Result OutputDebugString64(Core::System& system, uint64_t debug_str, uint64_t len) {
    R_RETURN(OutputDebugString(system, debug_str, len));
}

Result OutputDebugString64From32(Core::System& system, uint32_t debug_str, uint32_t len) {
    R_RETURN(OutputDebugString(system, debug_str, len));
}

} // namespace Kernel::Svc
