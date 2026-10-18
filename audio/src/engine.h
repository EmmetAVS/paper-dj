#ifndef PDJ_ENGINE_H
#define PDJ_ENGINE_H

#include "common.h"
#include "mixer.h"
#include <pthread.h>

typedef struct loaded_track loaded_track_t;   /* library.h */
typedef struct library library_t;

typedef struct {
    mixer_t          mixer;          /* owned by the render thread */
    cmdq_t           cmdq;           /* control -> render */
    pthread_mutex_t  cmd_lock;       /* serializes producers only */
    audio_ring_t     ring;           /* render -> phones */
    stats_t          stats;

    pthread_mutex_t  snap_lock;      /* render thread only ever try-locks this */
    mixer_snapshot_t snap;

    char            *index_html;     /* phone page, served at / */
    size_t           index_len;
    int              http_port;
    int              udp_port;
    int              prio_render;
    int              prio_control;
    int              prio_stream;
    const char      *dev_path;       /* QNX resource manager path, e.g. /dev/paperdj */
    library_t       *lib;            /* song library, or NULL (no -L) */
    _Atomic(loaded_track_t *) incoming[NDECKS];   /* loader -> render: song to swap in */
    _Atomic(loaded_track_t *) outgoing[NDECKS];   /* render -> loader: old or rejected song to free */
    _Atomic int      running;
} engine_t;

/* Parses text holding one or more commands separated by ';' or newlines and
 * queues them for the render thread. Returns how many were accepted. */
int  engine_submit_text(engine_t *e, const char *text, uint64_t t_recv_ns);
void engine_read_snapshot(engine_t *e, mixer_snapshot_t *out);

/* Threads (net.c) */
void *udp_control_thread(void *arg);
void *http_server_thread(void *arg);
/* resmgr.c: serves /dev/paperdj on QNX (needs root); does nothing elsewhere. */
void *resmgr_thread(void *arg);

/* Raises the calling thread to SCHED_FIFO at prio. 0 = ok. */
int  set_realtime(int prio, const char *who);

#endif
