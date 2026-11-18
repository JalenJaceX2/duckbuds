// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The emulator shutting down in direct mode. Upstream's replay lives as long as
// its process (detached threads, the GPU until exit); in Lichii the emulation stops first and the
// device goes away while the process goes on.
// - nvn_direct_stop, before the guest's cores are suspended: a game thread inside a halted NVN
//   call (core/nvn_hook.cpp RunHaltedCall) waits on the host, where the kernel cannot suspend it
//   (a fence, room in the replay worker's queue, a present's fill). Those waits return now:
//   nothing more is replayed or submitted and every fence reads as done.
// - nvn_direct_park, before Lichii's device and the renderer objects the replay calls back into
//   (the ASTC decode) are destroyed: the replay worker finishes what it holds, pipeline builds
//   and speculative shader translations stop, the pipeline cache is saved a last time, the
//   completion thread runs what was submitted, and every thread of the replay and the front end
//   leaves and is joined (vka_thread). None of them uses the device or guest memory after that. A
//   thread that does not stop within its bound (stuck in the driver) ends the process instead:
//   the device must not go away under it.

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include "bananative/dm/gfx/vk_android/vka.h"
#include "nvn_direct.h"

void nvn_direct_present_wake(void); // nvn_present.c
int vka_rc_park(uint64_t timeout_ns); // nvn_recompile.c

static _Atomic int g_parked;

// ---------- The threads ----------
enum { NTHREAD = 64 };
typedef struct {
    pthread_t th;
    void* (*fn)(void*);
    void* arg;
    void (*wake)(void);
    _Atomic int state; // 0 free, 1 running, 2 returned (to be joined)
} thread_slot;
static thread_slot g_threads[NTHREAD];
static pthread_mutex_t g_threads_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_leaving;

int vka_leaving(void) { return atomic_load_explicit(&g_leaving, memory_order_acquire); }

static void* thread_main(void* arg) {
    thread_slot* s = arg;
    void* r = s->fn(s->arg);
    atomic_store(&s->state, 2);
    return r;
}

int vka_thread(void* (*fn)(void*), void* arg, void (*wake)(void)) {
    pthread_mutex_lock(&g_threads_mu);
    thread_slot* s = NULL;
    for (int i = 0; i < NTHREAD && !s; i++) if (!atomic_load(&g_threads[i].state)) s = &g_threads[i];
    if (!s) {
        pthread_mutex_unlock(&g_threads_mu);
        VRED("more than %d threads: one is not started", NTHREAD);
        return -1;
    }
    s->fn = fn; s->arg = arg; s->wake = wake;
    atomic_store(&s->state, 1);
    const int r = pthread_create(&s->th, NULL, thread_main, s);
    if (r) atomic_store(&s->state, 0);
    pthread_mutex_unlock(&g_threads_mu);
    return r;
}

// Every thread asked to leave, woken until it has returned (bounded), and joined; 1 = all left
static int threads_leave(uint64_t timeout_ns, int* running) {
    atomic_store(&g_leaving, 1);
    const uint64_t t0 = now_ns();
    for (;;) {
        void (*wake[NTHREAD])(void);
        int n = 0, left = 1;
        pthread_mutex_lock(&g_threads_mu);
        for (int i = 0; i < NTHREAD; i++) {
            if (atomic_load(&g_threads[i].state) != 1) continue;
            left = 0;
            if (g_threads[i].wake) wake[n++] = g_threads[i].wake;
        }
        pthread_mutex_unlock(&g_threads_mu);
        for (int i = 0; i < n; i++) wake[i](); // (outside the lock: a wake takes the thread's own)
        *running = 0;
        if (!left) for (int i = 0; i < NTHREAD; i++) *running += atomic_load(&g_threads[i].state) == 1;
        if (left || now_ns() - t0 >= timeout_ns) break;
        usleep(1000);
    }
    for (int i = 0; i < NTHREAD; i++) {
        if (atomic_load(&g_threads[i].state) != 2) continue;
        pthread_join(g_threads[i].th, NULL);
        atomic_store(&g_threads[i].state, 0);
    }
    return !*running;
}

// The shutdown should reach nvn_direct_park within seconds of the stop: if it does not, a line
// says so.
static void* watchdog_main(void* arg) {
    (void)arg;
    pthread_setname_np(pthread_self(), "vk-shutdown");
    for (int i = 0; i < 50; i++) {
        if (atomic_load(&g_parked)) return NULL;
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
    }
    VRED("shutdown: the device not released 5 s after the stop (Lichii's shutdown waits elsewhere)");
    return NULL;
}

void nvn_direct_stop(void) {
    if (vka_stopping()) return;
    vka_stop();
    vka_worker_wake();
    nvn_direct_present_wake();
    VLOG("the emulator stops: nothing more is replayed or submitted, every fence reads as done");
    vka_thread(watchdog_main, NULL, NULL);
}

void nvn_direct_park(void) {
    nvn_direct_stop();
    atomic_store(&g_parked, 1);
    // Bounded: a thread stuck in the driver must not keep the emulator from shutting down. The
    // device's own wait for idle follows (RendererVulkan's destructor).
    const uint64_t bound = 3000000000ull;
    const uint64_t t0 = now_ns();
    const int worker = vka_worker_idle(bound);
    const int builds = vka_pso_park(bound);
    const int translations = vka_rc_park(bound);
    const int completion = worker && builds && translations && vka_complete_stop(bound);
    int running = 0;
    const int left = completion && threads_leave(bound, &running);
    const double ms = (now_ns() - t0) / 1e6;
    if (left) {
        VLOG("parked for the shutdown in %.0f ms: the replay worker is idle, no pipeline is being built or shader translated, the pipeline cache is saved, every thread has left", ms);
        return;
    }
    VRED("parked for the shutdown in %.0f ms: the replay worker %s, pipeline builds %s, speculative translations %s, the completion thread %s, %d threads %s; the device is still in use: the process exits", ms,
         worker ? "is idle" : "is still replaying", builds ? "stopped" : "still running", translations ? "stopped" : "still running",
         !(worker && builds && translations) ? "was not stopped" : completion ? "has left" : "is still running", running, completion ? "still running" : "not asked to leave");
    // Not abort or exit: the replay's own fatal exit (dm_host_shutdown's abort) has been seen to hang
    // the process, and exit handlers and destructors would destroy the device under those threads.
    _exit(1);
}
