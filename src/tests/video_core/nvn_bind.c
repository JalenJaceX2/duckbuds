// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The replay's binding walk (vka_bind.c vka_walk) for storage buffers whose address is read from
// a table in memory (VKA_KIND_SBUF_IND, VKA_KIND_SBUF_UBO_IND: Ocarina of Time's materials).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vka_bind.h"
#include "nvn_direct_front.h"

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "%d: %s\n", __LINE__, #expr); exit(1); } } while (0)

enum { GUEST_BASE = 0x10000, ALIGN = 64 };
static _Alignas(64) uint8_t arena[4096];

uint64_t nvn_direct_guest_of(uint64_t host) { return GUEST_BASE + host - (uintptr_t)arena; }
uint64_t nvn_direct_host_of(uint64_t guest) { return (uintptr_t)arena + guest - GUEST_BASE; }

// Guest memory as the replay's cpu_read sees it: arena pointers or the game's addresses
static int rd(void* ctx, uint64_t addr, void* out, size_t n) {
    (void)ctx;
    if (addr >= GUEST_BASE && addr - GUEST_BASE + n <= sizeof arena) addr = nvn_direct_host_of(addr);
    if (addr < (uintptr_t)arena || addr - (uintptr_t)arena + n > sizeof arena) return 0;
    memcpy(out, (const void*)(uintptr_t)addr, n);
    return 1;
}

static int walk(const vka_bstate* s, int st, dmsh_vk_bind b, vka_res* r) {
    dmsh_vk_entry e = {.bind_count = 1};
    char why[200] = "";
    return vka_walk(s, st, &e, &b, ALIGN, 0, rd, NULL, r, 1, NULL, why, sizeof why);
}

int main(void) {
    static vka_bstate s;
    const uint64_t table = 0x100, uniforms = 0x200, buffer = GUEST_BASE + 0x1234, entry = 0x18;
    memcpy(arena + table + entry, &buffer, 8);                 // the table's entry: the buffer's game address
    const uint64_t table_guest = GUEST_BASE + table;
    memcpy(arena + uniforms + 0x40, &table_guest, 8);          // a uniform buffer holding the table's address
    s.ssbo[1][1] = (vka_range){(uintptr_t)arena + table, 0x40}; // the pixel stage's storage slot 1 points at the table
    s.ubo[1][2] = (vka_range){(uintptr_t)arena + uniforms, 0x100};
    const dmsh_vk_bind base = {.kind = VKA_KIND_SBUF_IND, .desc = 7 /* STORAGE_BUFFER */, .count = 1, .reg = 1, .soff = entry};
    vka_res r;

    // The address is the table entry's, bound from there (rounded down to the alignment) to the end of its chunk
    CHECK(walk(&s, 1, base, &r) == 1);
    CHECK(r.what == VKA_R_BUF && r.addr == (buffer & ~(uint64_t)(ALIGN - 1)) && r.size == 0);

    // The same table found through a uniform buffer word
    dmsh_vk_bind ubo = base;
    ubo.kind = VKA_KIND_SBUF_UBO_IND; ubo.reg = 2; ubo.off = 0x40;
    CHECK(walk(&s, 1, ubo, &r) == 1);
    CHECK(r.what == VKA_R_BUF && r.addr == (buffer & ~(uint64_t)(ALIGN - 1)) && r.size == 0);

    // A null entry, an unbound slot or an unreadable table binds nothing
    const uint64_t zero = 0;
    memcpy(arena + table + entry, &zero, 8);
    CHECK(walk(&s, 1, base, &r) == 1 && r.what == VKA_R_NULL);
    memcpy(arena + table + entry, &buffer, 8);
    CHECK(walk(&s, 0, base, &r) == 1 && r.what == VKA_R_NULL);
    s.ssbo[1][1].addr = 1;
    CHECK(walk(&s, 1, base, &r) == 1 && r.what == VKA_R_NULL);

    puts("nvn_bind_tests: ok");
    return 0;
}
