/* SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-License-Identifier: GPL-3.0-or-later */

/* ELF: the NVN direct library's variables of static storage (file scope and function scope, some
   two thousand, most of them upstream's) in output sections of their own, so nvn_direct_reset
   (nvn_reset.c) returns every one of them to its initial value when a game stops. Placed by the
   linker, so any compiler's objects are collected (GNU ld and lld augment their default layout
   with INSERT). Before .data and .bss: GNU ld gives an input section to the first statement
   that matches it, and .data.rel.ro and the thread-local sections come earlier. */

SECTIONS
{
    nvn_direct_data :
    {
        __start_nvn_direct_data = .;
        *libnvn_direct.a:*(.data .data.rel .data.rel.local)
        __stop_nvn_direct_data = .;
    }
}
INSERT BEFORE .data;

SECTIONS
{
    nvn_direct_bss (NOLOAD) :
    {
        __start_nvn_direct_bss = .;
        *libnvn_direct.a:*(.bss COMMON)
        __stop_nvn_direct_bss = .;
    }
}
INSERT BEFORE .bss;
