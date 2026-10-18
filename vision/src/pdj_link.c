#include "pdj_link.h"

#include <arpa/inet.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_KEYS        32
#define MIN_INTERVAL_NS 16000000ull   /* coalesced values: at most ~60 sends/s */
#define PACKET_MAX      1200

typedef struct {
    char  key[24];
    float value;      /* newest value */
    float sent;       /* last value actually sent */
    float epsilon;    /* changes smaller than this are not worth a packet */
    int   dirty;
    int   ever_sent;
} slot_t;

struct pdj_link {
    int              fd;
    struct sockaddr_in addr;
    pthread_mutex_t  lock;
    slot_t           slots[MAX_KEYS];
    int              nslots;
    uint64_t         last_flush_ns;
    pdj_link_stats_t stats;
};

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

pdj_link_t *pdj_link_open(const char *host, int port) {
    pdj_link_t *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    l->fd = socket(AF_INET, SOCK_DGRAM, 0);
    l->addr.sin_family = AF_INET;
    l->addr.sin_port = htons((uint16_t)port);
    if (l->fd < 0 || inet_pton(AF_INET, host, &l->addr.sin_addr) != 1) {
        struct hostent *h = gethostbyname(host);
        if (l->fd < 0 || !h) { if (l->fd >= 0) close(l->fd); free(l); return NULL; }
        memcpy(&l->addr.sin_addr, h->h_addr_list[0], sizeof l->addr.sin_addr);
    }
    /* priority inheritance: if the low-priority model thread holds the lock,
     * the capture thread waiting on it boosts it instead of being blocked */
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&l->lock, &a);
    pthread_mutexattr_destroy(&a);
    return l;
}

void pdj_link_close(pdj_link_t *l) {
    if (!l) return;
    pdj_flush(l);
    close(l->fd);
    pthread_mutex_destroy(&l->lock);
    free(l);
}

static int send_packet(pdj_link_t *l, const char *buf, size_t n) {
    if (n == 0) return 0;
    ssize_t r = sendto(l->fd, buf, n, 0, (struct sockaddr *)&l->addr, sizeof l->addr);
    l->stats.packets++;
    return r < 0 ? -1 : 0;
}

/* lock held */
static int flush_locked(pdj_link_t *l) {
    char buf[PACKET_MAX];
    size_t n = 0;
    int rc = 0;
    for (int i = 0; i < l->nslots; i++) {
        slot_t *s = &l->slots[i];
        if (!s->dirty) continue;
        s->dirty = 0;
        if (s->ever_sent && fabsf(s->value - s->sent) < s->epsilon) continue;
        char item[48];
        int k = snprintf(item, sizeof item, "%s%s %.3f", n ? ";" : "", s->key, s->value);
        if (n + (size_t)k >= sizeof buf) {      /* packet full: send and start a new one */
            rc |= send_packet(l, buf, n);
            n = 0;
            k = snprintf(item, sizeof item, "%s %.3f", s->key, s->value);
        }
        memcpy(buf + n, item, (size_t)k);
        n += (size_t)k;
        s->sent = s->value;
        s->ever_sent = 1;
        l->stats.values_sent++;
    }
    rc |= send_packet(l, buf, n);
    l->last_flush_ns = now_ns();
    return rc;
}

int pdj_flush(pdj_link_t *l) {
    pthread_mutex_lock(&l->lock);
    int rc = flush_locked(l);
    pthread_mutex_unlock(&l->lock);
    return rc;
}

int pdj_tick(pdj_link_t *l) {
    pthread_mutex_lock(&l->lock);
    int rc = 0;
    if (now_ns() - l->last_flush_ns >= MIN_INTERVAL_NS) rc = flush_locked(l);
    pthread_mutex_unlock(&l->lock);
    return rc;
}

static void set_locked(pdj_link_t *l, const char *key, float value, float epsilon) {
    l->stats.updates++;
    for (int i = 0; i < l->nslots; i++) {
        if (strcmp(l->slots[i].key, key) == 0) {
            l->slots[i].value = value;
            l->slots[i].dirty = 1;
            return;
        }
    }
    if (l->nslots == MAX_KEYS) return;
    slot_t *s = &l->slots[l->nslots++];
    snprintf(s->key, sizeof s->key, "%s", key);
    s->value = value;
    s->epsilon = epsilon;
    s->dirty = 1;
    s->ever_sent = 0;
}

void pdj_set(pdj_link_t *l, const char *key, float value) {
    pthread_mutex_lock(&l->lock);
    set_locked(l, key, value, 0.004f);
    pthread_mutex_unlock(&l->lock);
}

int pdj_send_now(pdj_link_t *l, const char *cmd) {
    pthread_mutex_lock(&l->lock);
    int rc = flush_locked(l);                 /* keep event order */
    rc |= send_packet(l, cmd, strlen(cmd));
    l->stats.triggers++;
    pthread_mutex_unlock(&l->lock);
    return rc;
}

int pdj_pad(pdj_link_t *l, int pad) {
    if (pad < 1 || pad > 16) return -1;
    char cmd[16];
    snprintf(cmd, sizeof cmd, "pad %d", pad);
    return pdj_send_now(l, cmd);
}

int pdj_scratch_begin(pdj_link_t *l, char deck) {
    /* the engine anchors on the first angle that follows, so absolute camera
     * angles work directly: only the change since touch-down moves the record */
    char cmd[32];
    snprintf(cmd, sizeof cmd, "%c.scratch 1", deck);
    pthread_mutex_lock(&l->lock);
    char key[16];
    snprintf(key, sizeof key, "%c.platter", deck);
    for (int i = 0; i < l->nslots; i++)        /* new touch: forget the old angle */
        if (strcmp(l->slots[i].key, key) == 0) { l->slots[i].dirty = 0; l->slots[i].ever_sent = 0; }
    pthread_mutex_unlock(&l->lock);
    return pdj_send_now(l, cmd);
}

void pdj_scratch_angle(pdj_link_t *l, char deck, float deg) {
    char key[16];
    snprintf(key, sizeof key, "%c.platter", deck);
    pthread_mutex_lock(&l->lock);
    set_locked(l, key, deg, 0.25f);
    pthread_mutex_unlock(&l->lock);
}

int pdj_scratch_end(pdj_link_t *l, char deck) {
    char cmd[16];
    snprintf(cmd, sizeof cmd, "%c.scratch 0", deck);
    return pdj_send_now(l, cmd);              /* flushes the last angle first */
}

void pdj_link_stats(pdj_link_t *l, pdj_link_stats_t *out) {
    pthread_mutex_lock(&l->lock);
    *out = l->stats;
    pthread_mutex_unlock(&l->lock);
}

/* ------------------------------------------------------------------------ */

void pdj_pads_init(pdj_pads_t *p, int press_frames, int release_frames) {
    memset(p, 0, sizeof *p);
    p->press_frames = (uint8_t)(press_frames < 1 ? 1 : press_frames);
    p->release_frames = (uint8_t)(release_frames < 1 ? 1 : release_frames);
}

int pdj_pads_update(pdj_pads_t *p, const uint8_t covered[16], pdj_link_t *l) {
    int fired = 0;
    for (int i = 0; i < 16; i++) {
        if (covered[i]) {
            p->clear_run[i] = 0;
            if (p->covered_run[i] < 255) p->covered_run[i]++;
            if (!p->held[i] && p->covered_run[i] >= p->press_frames) {
                p->held[i] = 1;
                if (l) pdj_pad(l, i + 1);
                fired++;
            }
        } else {
            p->covered_run[i] = 0;
            if (p->clear_run[i] < 255) p->clear_run[i]++;
            if (p->held[i] && p->clear_run[i] >= p->release_frames) p->held[i] = 0;
        }
    }
    return fired;
}

float pdj_unwrap_deg(float *accum, float *last_raw, int *have, float raw_deg) {
    if (!*have) { *have = 1; *last_raw = raw_deg; *accum = raw_deg; return *accum; }
    float d = raw_deg - *last_raw;
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    *last_raw = raw_deg;
    *accum += d;
    return *accum;
}
