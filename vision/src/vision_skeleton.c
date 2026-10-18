/* vision_skeleton — the two-thread vision pipeline with the priority ladder wired up.
 *
 *   camera ──► [capture thread, FIFO 32] ──pad check every frame──► pad presses (sent now)
 *                       │
 *                 latest-frame mailbox (older frames are dropped, never queued)
 *                       ▼
 *              [model thread, SPORADIC 28 -> 8, 20 ms per 33 ms]
 *                       │  hand landmarks -> volume / filter / scratch
 *                       ▼
 *              coalesced slider values (<= 60/s) ──UDP──► dj_engine
 *
 * The three functions marked FAKE stand in for the real camera, pad detector and
 * hand model so the whole chain can be run and measured today. Replace them;
 * keep the threads, priorities and mailbox.
 */
#include "pdj_link.h"
#include "pdj_rt.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PI_F 3.14159265f

/* ------------------------------------------------------------------------ */
/* Frame and hand types */

typedef struct {
    uint64_t seq;
    uint64_t t_capture_ns;
    double   t;               /* seconds since start (drives the fake input) */
    uint8_t  covered[16];     /* pad occupancy from the cheap per-frame check */
    /* real version: pointer to the board-rectified image */
} frame_t;

typedef struct {
    int   present;
    float vol;                /* 0..1 from vertical hand position */
    float filter;             /* -1..1 from horizontal hand position */
    int   touching_platter;
    float platter_raw_deg;    /* hand angle around the platter centre, (-180, 180] */
} hand_t;

/* ------------------------------------------------------------------------ */
/* Config and shared state */

static struct {
    const char *host;
    int    port, fps, seconds, pin;
    double model_ms, budget_ms;
} cfg = { "127.0.0.1", 9000, 30, 0, 0, 15.0, 20.0 };

static pdj_link_t *link_;
static _Atomic int running = 1;
static uint64_t t0_ns;

/* latest-frame mailbox: the model always gets the newest frame */
static pthread_mutex_t mb_lock;
static pthread_cond_t  mb_cond = PTHREAD_COND_INITIALIZER;
static frame_t         mb_frame;
static int             mb_new;

static _Atomic uint64_t n_captured, n_modelled, n_dropped, n_presses;
static _Atomic uint64_t lat_sum_us, lat_max_us;
static _Atomic uint64_t cpu_capture_us, cpu_model_us;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------------------ */
/* FAKE 1: camera. Real version: QNX Sensor Framework frame -> warp to the board
 * using the ArUco homography (re-detect markers every N frames). */
static void fake_capture(frame_t *f, uint64_t seq) {
    f->seq = seq;
    f->t_capture_ns = now_ns();
    f->t = (f->t_capture_ns - t0_ns) / 1e9;
}

/* FAKE 2: pad occlusion check. Real version: compare each pad's centre patch
 * with the empty-board reference image; cheap enough for every frame.
 * Here: a scripted 8-second routine of pad presses (4 frames each). */
static void fake_pad_check(frame_t *f) {
    static const struct { double t; int pad; } script[] = {
        { 0.3, 1 },  /* play A */   { 0.8, 3 },  /* play B */   { 1.5, 12 }, /* sync B */
        { 2.5, 5 },  /* hot cue 1 A (store) */  { 3.5, 13 }, /* fx A on */
        { 5.0, 13 }, /* fx A off */ { 6.0, 14 }, /* sample 1 */ { 7.0, 5 },  /* hot cue 1 A (jump) */
    };
    memset(f->covered, 0, sizeof f->covered);
    double tl = fmod(f->t, 8.0), hold = 4.0 / cfg.fps;
    for (size_t i = 0; i < sizeof script / sizeof *script; i++)
        if (tl >= script[i].t && tl < script[i].t + hold) f->covered[script[i].pad - 1] = 1;
}

/* FAKE 3: hand model. Real version: palm detector (every few frames) + hand
 * landmark model on the crop, via ONNX Runtime, mapped to board coordinates.
 * Here: burns model_ms of CPU (preemptible), then produces smooth fake hands. */
static void fake_hand_model(const frame_t *f, hand_t hands[2]) {
    double start = pdj_thread_cpu_ms();
    volatile double sink = 0;
    while (pdj_thread_cpu_ms() - start < cfg.model_ms) sink += sqrt(sink + 1.0);

    float t = (float)f->t;
    hands[0].present = 1;                                   /* left hand: deck A volume */
    hands[0].vol = 0.55f + 0.25f * sinf(2 * PI_F * t / 4.0f);
    hands[0].filter = 0.0f;
    hands[0].touching_platter = 0;
    hands[1].present = 1;                                   /* right hand: filter sweep + a scratch */
    hands[1].vol = 0.8f;
    hands[1].filter = 0.5f * sinf(2 * PI_F * t / 6.0f);
    float tl = fmodf(t, 8.0f);
    hands[1].touching_platter = tl >= 4.0f && tl < 5.0f;
    hands[1].platter_raw_deg = 60.0f * sinf(2 * PI_F * 2.0f * tl);
}

/* ------------------------------------------------------------------------ */

static void *capture_thread(void *arg) {
    (void)arg;
    pdj_rt_fifo(PDJ_PRIO_CAPTURE, "capture");
    if (cfg.pin) pdj_rt_pin(0x7, "capture");               /* CPUs 0-2; leave CPU 3 to audio */

    pdj_pads_t pads;
    pdj_pads_init(&pads, 2, 3);                              /* press after 2 frames, re-arm after 3 */
    uint64_t period = 1000000000ull / (uint64_t)cfg.fps, start = now_ns(), seq = 0;
    double cpu0 = pdj_thread_cpu_ms();

    while (atomic_load(&running)) {
        uint64_t due = start + seq * period;
        struct timespec ts = { (time_t)(due / 1000000000ull), (long)(due % 1000000000ull) };
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) { }

        frame_t f;
        fake_capture(&f, seq++);
        fake_pad_check(&f);
        /* fast path: pad presses go out from this thread, without waiting for the model */
        atomic_fetch_add(&n_presses, (uint64_t)pdj_pads_update(&pads, f.covered, link_));

        pthread_mutex_lock(&mb_lock);
        if (mb_new) atomic_fetch_add(&n_dropped, 1);        /* model hadn't taken the last one */
        mb_frame = f;
        mb_new = 1;
        pthread_cond_signal(&mb_cond);
        pthread_mutex_unlock(&mb_lock);
        atomic_fetch_add(&n_captured, 1);
        atomic_store(&cpu_capture_us, (uint64_t)((pdj_thread_cpu_ms() - cpu0) * 1000));
    }
    pthread_mutex_lock(&mb_lock);
    pthread_cond_broadcast(&mb_cond);
    pthread_mutex_unlock(&mb_lock);
    return NULL;
}

static void *model_thread(void *arg) {
    (void)arg;
    pdj_rt_sporadic(PDJ_PRIO_MODEL_HI, PDJ_PRIO_MODEL_LO, cfg.budget_ms, 1000.0 / cfg.fps, "model");
    if (cfg.pin) pdj_rt_pin(0x7, "model");

    int scratching[2] = { 0, 0 }, have[2] = { 0, 0 };
    float accum[2] = { 0, 0 }, last_raw[2] = { 0, 0 };
    double cpu0 = pdj_thread_cpu_ms();

    while (atomic_load(&running)) {
        frame_t f;
        pthread_mutex_lock(&mb_lock);
        while (!mb_new && atomic_load(&running)) pthread_cond_wait(&mb_cond, &mb_lock);
        f = mb_frame;
        mb_new = 0;
        pthread_mutex_unlock(&mb_lock);
        if (!atomic_load(&running)) break;

        hand_t hands[2];
        fake_hand_model(&f, hands);

        for (int i = 0; i < 2; i++) {
            char d = (char)('a' + i), key[16];
            if (!hands[i].present) continue;
            snprintf(key, sizeof key, "%c.vol", d);
            pdj_set(link_, key, hands[i].vol);
            snprintf(key, sizeof key, "%c.filter", d);
            pdj_set(link_, key, hands[i].filter);

            if (hands[i].touching_platter && !scratching[i]) {
                scratching[i] = 1; have[i] = 0;
                pdj_scratch_begin(link_, d);
            }
            if (scratching[i] && hands[i].touching_platter)
                pdj_scratch_angle(link_, d, pdj_unwrap_deg(&accum[i], &last_raw[i], &have[i], hands[i].platter_raw_deg));
            if (!hands[i].touching_platter && scratching[i]) {
                scratching[i] = 0;
                pdj_scratch_end(link_, d);
            }
        }
        pdj_tick(link_);

        uint64_t lat = (now_ns() - f.t_capture_ns) / 1000;
        atomic_fetch_add(&lat_sum_us, lat);
        uint64_t m = atomic_load(&lat_max_us);
        while (lat > m && !atomic_compare_exchange_weak(&lat_max_us, &m, lat)) { }
        atomic_fetch_add(&n_modelled, 1);
        atomic_store(&cpu_model_us, (uint64_t)((pdj_thread_cpu_ms() - cpu0) * 1000));
    }
    return NULL;
}

static void on_signal(int s) { (void)s; atomic_store(&running, 0); }

static void usage(const char *a) {
    fprintf(stderr,
        "usage: %s [-H engine_host] [-P udp_port] [-f fps] [-m fake_model_ms] [-b model_budget_ms]\n"
        "          [-t seconds] [-c]\n"
        "defaults: -H 127.0.0.1 -P 9000 -f 30 -m 15 -b 20; -t 0 runs until Ctrl-C;\n"
        "          -c pins vision to CPUs 0-2 so CPU 3 is left to the audio engine\n", a);
}

int main(int argc, char **argv) {
    int opt;
    while ((opt = getopt(argc, argv, "H:P:f:m:b:t:ch")) != -1) {
        switch (opt) {
        case 'H': cfg.host = optarg; break;
        case 'P': cfg.port = atoi(optarg); break;
        case 'f': cfg.fps = atoi(optarg); break;
        case 'm': cfg.model_ms = atof(optarg); break;
        case 'b': cfg.budget_ms = atof(optarg); break;
        case 't': cfg.seconds = atoi(optarg); break;
        case 'c': cfg.pin = 1; break;
        default: usage(argv[0]); return opt == 'h' ? 0 : 1;
        }
    }
    if (cfg.fps < 1 || cfg.budget_ms >= 1000.0 / cfg.fps) {
        fprintf(stderr, "budget (%.1f ms) must be shorter than one frame (%.1f ms)\n", cfg.budget_ms, 1000.0 / cfg.fps);
        return 1;
    }
    link_ = pdj_link_open(cfg.host, cfg.port);
    if (!link_) { fprintf(stderr, "cannot reach %s:%d\n", cfg.host, cfg.port); return 1; }

    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&mb_lock, &a);
    pthread_mutexattr_destroy(&a);

    pdj_rt_lock_memory();
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    t0_ns = now_ns();

    pthread_t th_cap, th_model;
    pthread_create(&th_model, NULL, model_thread, NULL);
    pthread_create(&th_cap, NULL, capture_thread, NULL);

    fprintf(stderr, "[vision] %d fps, fake model %.0f ms/frame, sending to %s:%d\n",
            cfg.fps, cfg.model_ms, cfg.host, cfg.port);
    uint64_t last_cap = 0, last_mod = 0;
    for (int sec = 1; atomic_load(&running); sec++) {
        sleep(1);
        uint64_t cap = atomic_load(&n_captured), mod = atomic_load(&n_modelled);
        pdj_link_stats_t ls;
        pdj_link_stats(link_, &ls);
        fprintf(stderr,
            "[vision] capture %3llu fps | model %3llu fps | dropped %llu | latency avg %.1f ms max %.1f ms | "
            "pads %llu | sent %llu pkts (%llu of %llu slider updates) | cpu capture %.0f ms model %.0f ms\n",
            (unsigned long long)(cap - last_cap), (unsigned long long)(mod - last_mod),
            (unsigned long long)atomic_load(&n_dropped),
            mod ? atomic_load(&lat_sum_us) / 1000.0 / mod : 0.0, atomic_load(&lat_max_us) / 1000.0,
            (unsigned long long)atomic_load(&n_presses), (unsigned long long)ls.packets,
            (unsigned long long)ls.values_sent, (unsigned long long)ls.updates,
            atomic_load(&cpu_capture_us) / 1000.0, atomic_load(&cpu_model_us) / 1000.0);
        last_cap = cap; last_mod = mod;
        if (cfg.seconds && sec >= cfg.seconds) atomic_store(&running, 0);
    }
    pthread_join(th_cap, NULL);
    pthread_join(th_model, NULL);
    pdj_link_close(link_);
    return 0;
}
