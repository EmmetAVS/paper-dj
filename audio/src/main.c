/* Paper DJ audio engine.
 *
 *   control (UDP / HTTP)  --cmd queue-->  render thread (SCHED_FIFO)  --ring-->  phone streams
 *
 * The render thread never blocks, allocates, or takes a lock: it wakes on an
 * absolute clock every 5.33 ms, applies queued commands, renders 256 frames
 * and publishes them. Everything else runs at lower priority, so heavy vision
 * work or a slow phone can never make the audio miss a deadline.
 */
#include "engine.h"

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static engine_t *g_engine;

int set_realtime(int prio, const char *who) {
    struct sched_param sp = { .sched_priority = prio };
    int err = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (err) {
        fprintf(stderr, "[%s] could not get SCHED_FIFO priority %d (%s); running at normal priority\n",
                who, prio, strerror(err));
        return -1;
    }
    fprintf(stderr, "[%s] running SCHED_FIFO priority %d\n", who, prio);
    return 0;
}

int engine_submit_text(engine_t *e, const char *text, uint64_t t_recv_ns) {
    char buf[2048];
    snprintf(buf, sizeof buf, "%s", text);
    int accepted = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, ";\n\r", &save); tok; tok = strtok_r(NULL, ";\n\r", &save)) {
        cmd_t c;
        if (cmd_parse(tok, &c) != 0) {
            fprintf(stderr, "[control] bad command: '%s'\n", tok);
            continue;
        }
        c.t_recv_ns = t_recv_ns;
        pthread_mutex_lock(&e->cmd_lock);
        int r = cmdq_push(&e->cmdq, &c);
        pthread_mutex_unlock(&e->cmd_lock);
        if (r == 0) accepted++;
        else atomic_fetch_add(&e->stats.cmds_dropped, 1);
    }
    return accepted;
}

/* The render thread never waits on this lock: if a stats reader holds it,
 * this update is simply skipped and the next one (~43 ms later) goes through. */
static void publish_snapshot(engine_t *e, float peak) {
    if (pthread_mutex_trylock(&e->snap_lock) != 0) return;
    mixer_snapshot(&e->mixer, &e->snap);
    e->snap.peak = peak;
    pthread_mutex_unlock(&e->snap_lock);
}

void engine_read_snapshot(engine_t *e, mixer_snapshot_t *out) {
    pthread_mutex_lock(&e->snap_lock);
    *out = e->snap;
    pthread_mutex_unlock(&e->snap_lock);
}

static void *render_thread(void *arg) {
    engine_t *e = arg;
    atomic_store(&e->stats.rt_render, set_realtime(e->prio_render, "render") == 0);

    int16_t block[BLOCK_FRAMES * 2];
    cpu_meter_t meter = {0};
    float peak_hold = 0.0f;
    uint64_t start = now_ns(), blocks = 0;

    while (atomic_load(&e->running)) {
        /* absolute deadline computed from the block count, so timing never drifts */
        uint64_t due = start + (blocks * BLOCK_FRAMES * 1000000000ull) / OUT_RATE;
        struct timespec ts = { (time_t)(due / 1000000000ull), (long)(due % 1000000000ull) };
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) { }

        uint64_t t0 = now_ns();
        uint64_t late = t0 > due ? t0 - due : 0;
        atomic_max_u64(&e->stats.max_late_ns, late);
        if (late > (uint64_t)BLOCK_FRAMES * 1000000000ull / OUT_RATE)
            atomic_fetch_add(&e->stats.deadline_misses, 1);

        /* apply everything that arrived since the last block */
        cmd_t c;
        while (cmdq_pop(&e->cmdq, &c) == 0) {
            mixer_apply(&e->mixer, &c);
            uint64_t lat = t0 > c.t_recv_ns ? t0 - c.t_recv_ns : 0;
            atomic_store(&e->stats.cmd_lat_ns_last, lat);
            atomic_max_u64(&e->stats.cmd_lat_ns_max, lat);
            atomic_fetch_add(&e->stats.cmds_applied, 1);
        }

        float peak = mixer_render(&e->mixer, block, BLOCK_FRAMES);
        peak_hold = peak > peak_hold ? peak : peak_hold * 0.95f;

        /* publish to the broadcast ring */
        uint64_t w = atomic_load_explicit(&e->ring.write_frames, memory_order_relaxed);
        uint32_t idx = (uint32_t)(w % RING_FRAMES);   /* RING_FRAMES is a multiple of BLOCK_FRAMES */
        memcpy(&e->ring.pcm[2 * idx], block, sizeof block);
        atomic_store_explicit(&e->ring.write_frames, w + BLOCK_FRAMES, memory_order_release);

        uint64_t dt = now_ns() - t0;
        atomic_store(&e->stats.render_ns_last, dt);
        atomic_max_u64(&e->stats.render_ns_max, dt);
        atomic_store(&e->stats.blocks, ++blocks);
        if ((blocks & 7) == 0) publish_snapshot(e, peak_hold);
        if ((blocks & 63) == 0) cpu_meter_tick(&meter, &e->stats.render_cpu_pm);
    }
    return NULL;
}

static int start_thread(pthread_t *th, void *(*fn)(void *), void *arg) {
    int err = pthread_create(th, NULL, fn, arg);
    if (err) fprintf(stderr, "pthread_create: %s\n", strerror(err));
    return err;
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) == (size_t)n) { b[n] = 0; *len = (size_t)n; }
    else { free(b); b = NULL; }
    fclose(f);
    return b;
}

static void on_signal(int sig) {
    (void)sig;
    if (g_engine) atomic_store(&g_engine->running, 0);
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s [-a deckA.wav] [-b deckB.wav] [-1 sample1.wav] [-2 sample2.wav]\n"
        "          [-A bpmA] [-B bpmB] [-p http_port] [-u udp_port]\n"
        "          [-w web/index.html] [-R render_prio] [-C control_prio] [-S stream_prio]\n"
        "          [-d /dev/paperdj]\n"
        "defaults: -p 8080 -u 9000 -w web/index.html -R 50 -C 45 -S 40 -d /dev/paperdj; BPM is auto-detected\n          (-A/-B 124 = \"about 124\": locks the exact tempo and beats within +-2 BPM)\n", argv0);
}

static int load_wav(track_t *t, char *name, size_t namelen, const char *path) {
    char err[256];
    if (wav_load(path, t, err, sizeof err) != 0) {
        fprintf(stderr, "[load] %s\n", err);
        return -1;
    }
    const char *base = strrchr(path, '/');
    snprintf(name, namelen, "%s", base ? base + 1 : path);
    fprintf(stderr, "[load] %s: %.1f s at %d Hz\n", name, (double)t->frames / t->rate, t->rate);
    return 0;
}

int main(int argc, char **argv) {
    engine_t *e = calloc(1, sizeof *e);
    if (!e) return 1;
    g_engine = e;
    e->http_port = 8080;
    e->udp_port = 9000;
    e->prio_render = 50;     /* below 64 so it works as a non-root user on QNX */
    e->prio_control = 45;
    e->prio_stream = 40;     /* sending audio to phones outranks vision (32 and below) */
    const char *paths[NDECKS] = { NULL, NULL };
    const char *samples[NSAMPLES] = { NULL, NULL };
    double bpm_override[NDECKS] = { 0, 0 };
    const char *web = "web/index.html";
    e->dev_path = "/dev/paperdj";

    int opt;
    while ((opt = getopt(argc, argv, "a:b:1:2:A:B:p:u:w:R:C:S:d:h")) != -1) {
        switch (opt) {
        case 'a': paths[0] = optarg; break;
        case 'b': paths[1] = optarg; break;
        case '1': samples[0] = optarg; break;
        case '2': samples[1] = optarg; break;
        case 'A': bpm_override[0] = atof(optarg); break;
        case 'B': bpm_override[1] = atof(optarg); break;
        case 'p': e->http_port = atoi(optarg); break;
        case 'u': e->udp_port = atoi(optarg); break;
        case 'w': web = optarg; break;
        case 'R': e->prio_render = atoi(optarg); break;
        case 'C': e->prio_control = atoi(optarg); break;
        case 'S': e->prio_stream = atoi(optarg); break;
        case 'd': e->dev_path = optarg; break;
        default: usage(argv[0]); return opt == 'h' ? 0 : 1;
        }
    }

    if (mixer_init(&e->mixer) != 0) { fprintf(stderr, "out of memory\n"); return 1; }
    for (int i = 0; i < NDECKS; i++) {
        deck_t *d = &e->mixer.deck[i];
        snprintf(d->name, sizeof d->name, "(empty)");
        if (paths[i] && load_wav(&d->track, d->name, sizeof d->name, paths[i]) == 0) {
            if (bpm_override[i] > 0) mixer_analyze_deck_hint(&e->mixer, i, bpm_override[i]);
            else mixer_analyze_deck(&e->mixer, i);
        }
    }
    for (int k = 0; k < NSAMPLES; k++) {
        sampler_t *sp = &e->mixer.sample[k];
        snprintf(sp->name, sizeof sp->name, "(empty)");
        if (samples[k]) load_wav(&sp->track, sp->name, sizeof sp->name, samples[k]);
    }
    e->index_html = read_file(web, &e->index_len);
    if (!e->index_html) fprintf(stderr, "[http] warning: could not read %s\n", web);

    pthread_mutex_init(&e->cmd_lock, NULL);   /* QNX mutexes use priority inheritance by default */
    pthread_mutex_init(&e->snap_lock, NULL);
    publish_snapshot(e, 0.0f);
    atomic_store(&e->running, 1);

    signal(SIGPIPE, SIG_IGN);   /* a phone disconnecting must not kill the engine */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        fprintf(stderr, "[engine] mlockall failed (%s); continuing\n", strerror(errno));

    pthread_t th_render, th_udp, th_http;
    if (start_thread(&th_render, render_thread, e) ||
        start_thread(&th_udp, udp_control_thread, e) ||
        start_thread(&th_http, http_server_thread, e)) return 1;
    pthread_t th_rm;   /* /dev/paperdj (QNX, root only); blocks forever, so never joined */
    if (start_thread(&th_rm, resmgr_thread, e) == 0) pthread_detach(th_rm);

    fprintf(stderr, "[engine] running: %d Hz, %d-frame blocks (%.2f ms). Ctrl-C to stop.\n",
            OUT_RATE, BLOCK_FRAMES, BLOCK_FRAMES * 1000.0 / OUT_RATE);
    pthread_join(th_render, NULL);
    pthread_join(th_udp, NULL);
    pthread_join(th_http, NULL);

    stats_t *s = &e->stats;
    fprintf(stderr, "[engine] stopped. blocks=%llu deadline_misses=%llu max_late=%.1f us render_max=%.1f us\n",
            (unsigned long long)atomic_load(&s->blocks), (unsigned long long)atomic_load(&s->deadline_misses),
            atomic_load(&s->max_late_ns) / 1e3, atomic_load(&s->render_ns_max) / 1e3);
    return 0;
}
