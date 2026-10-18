/* Shared constants, timing helpers and lock-free queues for the Paper DJ audio engine. */
#ifndef PDJ_COMMON_H
#define PDJ_COMMON_H

#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#define OUT_RATE      48000          /* output sample rate (Hz), stereo int16 */
#define BLOCK_FRAMES  256            /* frames per render block = 5.33 ms */
#define NDECKS        2

#define PDJ_PI 3.14159265358979323846

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

/* ---------------------------------------------------------------------------
 * Control command: parsed off the real-time thread, applied at block start.
 * ------------------------------------------------------------------------ */
enum cmd_op {
    CMD_PLAY, CMD_PAUSE, CMD_TOGGLE, CMD_CUE, CMD_SETCUE, CMD_SEEK,
    CMD_PARAM, CMD_XFADE, CMD_MASTER,
    CMD_HOTCUE, CMD_HOTCUE_CLEAR,          /* param = hot cue index */
    CMD_LOOP, CMD_LOOPLEN,                 /* LOOP value: -1 toggle, else 0/1 */
    CMD_SYNC, CMD_FX,                      /* value: -1 toggle, else 0/1 */
    CMD_SCRATCH, CMD_PLATTER, CMD_JOG,     /* hand on/off, absolute angle, relative angle (degrees) */
    CMD_BPM,
    CMD_SAMPLE_PLAY, CMD_SAMPLE_STOP, CMD_SAMPLE_VOL,  /* deck = sample index */
    CMD_QUANTIZE
};

/* Per-deck continuous parameters. */
enum deck_param { P_VOL, P_LOW, P_MID, P_HIGH, P_RATE, P_NUDGE, P_FILTER, P_FXAMT, NPARAM };

#define NHOTCUES  2
#define NSAMPLES  2

typedef struct {
    uint8_t  op;        /* enum cmd_op */
    uint8_t  deck;      /* 0 = A, 1 = B (or sample index) */
    uint8_t  param;     /* enum deck_param for CMD_PARAM, hot cue index for CMD_HOTCUE* */
    float    value;
    uint64_t t_recv_ns; /* when the command arrived (for latency stats) */
} cmd_t;

/* Single-consumer command ring. Producers serialize with a mutex (they are
 * not real-time); the render thread pops without ever taking a lock. */
#define CMDQ_CAP 1024  /* power of two */
typedef struct {
    cmd_t buf[CMDQ_CAP];
    _Atomic uint32_t head;  /* written by producer */
    _Atomic uint32_t tail;  /* written by consumer */
} cmdq_t;

static inline int cmdq_push(cmdq_t *q, const cmd_t *c) {
    uint32_t h = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&q->tail, memory_order_acquire);
    if (h - t >= CMDQ_CAP) return -1;  /* full */
    q->buf[h & (CMDQ_CAP - 1)] = *c;
    atomic_store_explicit(&q->head, h + 1, memory_order_release);
    return 0;
}

static inline int cmdq_pop(cmdq_t *q, cmd_t *out) {
    uint32_t t = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint32_t h = atomic_load_explicit(&q->head, memory_order_acquire);
    if (t == h) return -1;  /* empty */
    *out = q->buf[t & (CMDQ_CAP - 1)];
    atomic_store_explicit(&q->tail, t + 1, memory_order_release);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Broadcast audio ring: one writer (render thread), any number of readers
 * (one per phone). Readers track their own position and skip ahead if they
 * fall too far behind, so a slow phone can never stall the render thread.
 * ------------------------------------------------------------------------ */
#define RING_FRAMES (1u << 17)  /* ~2.7 s at 48 kHz */
typedef struct {
    int16_t pcm[RING_FRAMES * 2];      /* interleaved stereo */
    _Atomic uint64_t write_frames;     /* total frames ever written */
} audio_ring_t;

/* ---------------------------------------------------------------------------
 * Stats shared with the HTTP /stats endpoint.
 * ------------------------------------------------------------------------ */
typedef struct {
    _Atomic uint64_t blocks;
    _Atomic uint64_t deadline_misses;   /* woke up more than one block late */
    _Atomic uint64_t max_late_ns;
    _Atomic uint64_t render_ns_last;
    _Atomic uint64_t render_ns_max;
    _Atomic uint64_t cmds_applied;
    _Atomic uint64_t cmds_dropped;
    _Atomic uint64_t cmd_lat_ns_last;   /* arrival -> applied in a block */
    _Atomic uint64_t cmd_lat_ns_max;
    _Atomic int      clients;
    _Atomic int      rt_render;         /* 1 if render thread got SCHED_FIFO */
} stats_t;

static inline void atomic_max_u64(_Atomic uint64_t *a, uint64_t v) {
    uint64_t cur = atomic_load_explicit(a, memory_order_relaxed);
    while (v > cur && !atomic_compare_exchange_weak(a, &cur, v)) { }
}

#endif
