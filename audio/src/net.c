/* Networking: UDP control input and a tiny HTTP server that serves the phone
 * page, the live PCM stream, a /cmd endpoint for testing, and /stats. */
#include "engine.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_CLIENTS     8
#define STREAM_CHUNK    1024   /* frames per send (~21 ms) */
#define START_BACKLOG   2400   /* frames sent immediately on connect (50 ms) */

/* ------------------------------------------------------------------------ */
/* UDP control: "a.vol 0.7" style text, several per packet with ';' or '\n' */

void *udp_control_thread(void *arg) {
    engine_t *e = arg;
    set_realtime(e->prio_control, "control");

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)e->udp_port),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) < 0) {
        perror("udp bind");
        return NULL;
    }
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    fprintf(stderr, "[control] listening for UDP commands on port %d\n", e->udp_port);

    char buf[2048];
    cpu_meter_t meter = {0};
    while (atomic_load(&e->running)) {
        cpu_meter_tick(&meter, &e->stats.control_cpu_pm);
        ssize_t n = recv(s, buf, sizeof buf - 1, 0);
        if (n <= 0) continue;
        uint64_t t = now_ns();
        buf[n] = 0;
        atomic_fetch_add(&e->stats.via_udp, (uint64_t)engine_submit_text(e, buf, t));
    }
    close(s);
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* HTTP */

typedef struct { engine_t *e; int fd; } conn_t;

static int send_all(int fd, const void *p, size_t n) {
    const char *c = p;
    while (n) {
        ssize_t w = send(fd, c, n, 0);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        c += w; n -= (size_t)w;
    }
    return 0;
}

static void reply(int fd, const char *status, const char *type, const char *body, size_t len) {
    char h[512];
    int n = snprintf(h, sizeof h,
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
        status, type, len);
    send_all(fd, h, (size_t)n);
    if (len) send_all(fd, body, len);
}

static void url_decode(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') *o++ = ' ';
        else if (*s == '%' && s[1] && s[2]) {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else *o++ = *s;
    }
    *o = 0;
}

static void serve_stream(engine_t *e, int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    int sndbuf = 48 * 1024;  /* keep the kernel queue small: ~250 ms max hidden latency */
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);

    const char *h = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                    "X-Audio-Format: s16le;rate=48000;channels=2\r\n"
                    "X-Content-Type-Options: nosniff\r\nCache-Control: no-store\r\n"
                    "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
    if (send_all(fd, h, strlen(h)) < 0) return;

    atomic_fetch_add(&e->stats.clients, 1);
    uint64_t w = atomic_load_explicit(&e->ring.write_frames, memory_order_acquire);
    uint64_t r = w > START_BACKLOG ? w - START_BACKLOG : 0;

    while (atomic_load(&e->running)) {
        w = atomic_load_explicit(&e->ring.write_frames, memory_order_acquire);
        if (w - r > RING_FRAMES - 8192) r = w - START_BACKLOG;   /* fell too far behind: skip */
        if (w == r) {
            struct timespec ts = { 0, 2 * 1000000 };              /* wait ~2 ms for the next block */
            nanosleep(&ts, NULL);
            continue;
        }
        uint64_t n = w - r;
        if (n > STREAM_CHUNK) n = STREAM_CHUNK;
        uint32_t idx = (uint32_t)(r % RING_FRAMES);
        if (idx + n > RING_FRAMES) n = RING_FRAMES - idx;        /* don't cross the wrap */
        if (send_all(fd, &e->ring.pcm[2 * idx], (size_t)n * 4) < 0) break;
        r += n;
    }
    atomic_fetch_sub(&e->stats.clients, 1);
}

/* Whole-process CPU use since the previous /stats call, per mille of one core. */
static int process_cpu_pm(void) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static uint64_t w0, c0;
    static int last;
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    uint64_t c = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec, w = now_ns();
    pthread_mutex_lock(&lock);
    if (w0 && w - w0 > 200000000ull) { last = (int)((c - c0) * 1000 / (w - w0)); w0 = w; c0 = c; }
    else if (!w0) { w0 = w; c0 = c; }
    int r = last;
    pthread_mutex_unlock(&lock);
    return r;
}

static void serve_stats(engine_t *e, int fd) {
    mixer_snapshot_t s;
    engine_read_snapshot(e, &s);
    stats_t *st = &e->stats;
    char b[6144];
    size_t cap = sizeof b;
    int n = snprintf(b, cap,
        "{\"rate\":%d,\"block_frames\":%d,\"realtime\":%d,\"clients\":%d,\"rt_streams\":%d,"
        "\"prio\":{\"render\":%d,\"control\":%d,\"stream\":%d},"
        "\"cpu_pct\":{\"render\":%.1f,\"control\":%.1f,\"process\":%.1f},"
        "\"blocks\":%llu,\"deadline_misses\":%llu,\"max_late_us\":%.1f,"
        "\"render_us\":%.1f,\"render_us_max\":%.1f,"
        "\"cmds\":%llu,\"cmds_dropped\":%llu,\"cmd_lat_us\":%.1f,\"cmd_lat_us_max\":%.1f,"
        "\"via\":{\"dev\":%llu,\"udp\":%llu,\"http\":%llu},"
        "\"xfade\":%.3f,\"master\":%.3f,\"peak\":%.3f,\"quantize\":%d,\"samples\":[",
        OUT_RATE, BLOCK_FRAMES, atomic_load(&st->rt_render), atomic_load(&st->clients),
        atomic_load(&st->rt_streams), e->prio_render, e->prio_control, e->prio_stream,
        atomic_load(&st->render_cpu_pm) / 10.0, atomic_load(&st->control_cpu_pm) / 10.0, process_cpu_pm() / 10.0,
        (unsigned long long)atomic_load(&st->blocks), (unsigned long long)atomic_load(&st->deadline_misses),
        atomic_load(&st->max_late_ns) / 1e3, atomic_load(&st->render_ns_last) / 1e3,
        atomic_load(&st->render_ns_max) / 1e3,
        (unsigned long long)atomic_load(&st->cmds_applied), (unsigned long long)atomic_load(&st->cmds_dropped),
        atomic_load(&st->cmd_lat_ns_last) / 1e3, atomic_load(&st->cmd_lat_ns_max) / 1e3,
        (unsigned long long)atomic_load(&st->via_dev), (unsigned long long)atomic_load(&st->via_udp),
        (unsigned long long)atomic_load(&st->via_http),
        s.xfade, s.master, s.peak, s.quantize);
    for (int k = 0; k < NSAMPLES; k++)
        n += snprintf(b + n, cap - (size_t)n, "%s{\"name\":\"%s\",\"playing\":%d,\"vol\":%.2f}",
                      k ? "," : "", s.sample_name[k], s.sample_active[k], s.sample_vol[k]);
    n += snprintf(b + n, cap - (size_t)n, "],\"decks\":[");
    for (int i = 0; i < NDECKS; i++) {
        n += snprintf(b + n, cap - (size_t)n,
            "%s{\"name\":\"%s\",\"playing\":%d,\"pos\":%.3f,\"len\":%.3f,\"cue\":%.3f,"
            "\"hotcues\":[%.3f,%.3f],\"bpm\":%.2f,\"eff_bpm\":%.2f,\"phase\":%.3f,"
            "\"loop\":%d,\"loop_beats\":%.2f,\"loop_in\":%.3f,\"loop_out\":%.3f,"
            "\"sync\":%d,\"fx\":%d,\"scratching\":%d,"
            "\"vol\":%.3f,\"low\":%.1f,\"mid\":%.1f,\"high\":%.1f,\"rate\":%.4f,\"nudge\":%.3f,"
            "\"filter\":%.3f,\"fxamt\":%.2f}",
            i ? "," : "", s.name[i], s.playing[i], s.pos_s[i], s.len_s[i], s.cue_s[i],
            s.hotcue_s[i][0], s.hotcue_s[i][1], s.bpm[i], s.eff_bpm[i], s.phase[i],
            s.loop_on[i], s.loop_beats[i], s.loop_in_s[i], s.loop_out_s[i],
            s.sync_on[i], s.fx_on[i], s.scratching[i],
            s.param[i][P_VOL], s.param[i][P_LOW], s.param[i][P_MID], s.param[i][P_HIGH],
            s.param[i][P_RATE], s.param[i][P_NUDGE], s.param[i][P_FILTER], s.param[i][P_FXAMT]);
    }
    n += snprintf(b + n, cap - (size_t)n, "]}");
    reply(fd, "200 OK", "application/json", b, (size_t)n);
}

static void *conn_thread(void *arg) {
    conn_t c = *(conn_t *)arg;
    free(arg);
    engine_t *e = c.e;

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(c.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char req[4096];
    size_t len = 0;
    while (len < sizeof req - 1) {
        ssize_t n = recv(c.fd, req + len, sizeof req - 1 - len, 0);
        if (n <= 0) break;
        len += (size_t)n;
        req[len] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    req[len] = 0;

    char method[8] = {0}, path[2048] = {0};
    if (sscanf(req, "%7s %2047s", method, path) == 2) {
        uint64_t t = now_ns();
        if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
            if (e->index_html) reply(c.fd, "200 OK", "text/html; charset=utf-8", e->index_html, e->index_len);
            else reply(c.fd, "404 Not Found", "text/plain", "index.html not loaded\n", 22);
        } else if (!strcmp(path, "/stream")) {
            if (atomic_load(&e->stats.clients) >= MAX_CLIENTS)
                reply(c.fd, "503 Service Unavailable", "text/plain", "too many listeners\n", 19);
            else {
                /* this thread now carries audio to a listener: run it above vision */
                int rt = set_realtime(e->prio_stream, "stream") == 0;
                if (rt) atomic_fetch_add(&e->stats.rt_streams, 1);
                serve_stream(e, c.fd);
                if (rt) atomic_fetch_sub(&e->stats.rt_streams, 1);
            }
        } else if (!strncmp(path, "/cmd?c=", 7)) {
            char *q = path + 7;
            char *amp = strchr(q, '&');
            if (amp) *amp = 0;
            url_decode(q);
            int ok = engine_submit_text(e, q, t);
            atomic_fetch_add(&e->stats.via_http, (uint64_t)ok);
            reply(c.fd, ok ? "204 No Content" : "400 Bad Request", "text/plain", "", 0);
        } else if (!strcmp(path, "/stats")) {
            serve_stats(e, c.fd);
        } else {
            reply(c.fd, "404 Not Found", "text/plain", "not found\n", 10);
        }
    }
    close(c.fd);
    return NULL;
}

void *http_server_thread(void *arg) {
    engine_t *e = arg;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)e->http_port),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) < 0 || listen(s, 16) < 0) {
        perror("http bind");
        atomic_store(&e->running, 0);
        return NULL;
    }
    fprintf(stderr, "[http] phone page at http://<pi-ip>:%d/\n", e->http_port);

    while (atomic_load(&e->running)) {
        struct pollfd pfd = { .fd = s, .events = POLLIN };
        if (poll(&pfd, 1, 200) <= 0) continue;  /* timeout lets us notice shutdown */
        int fd = accept(s, NULL, NULL);
        if (fd < 0) continue;
        conn_t *c = malloc(sizeof *c);
        if (!c) { close(fd); continue; }
        c->e = e; c->fd = fd;
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&th, &at, conn_thread, c) != 0) { close(fd); free(c); }
        pthread_attr_destroy(&at);
    }
    close(s);
    return NULL;
}
