/* /dev/paperdj: the DJ board as a QNX resource manager.
 *
 *   echo "pad 5" > /dev/paperdj           any engine command (one per line or ';')
 *   echo "a.vol 0.6; b.filter -0.3" > /dev/paperdj
 *   cat /dev/paperdj                      live status of both decks
 *
 * Every open/write/read on the file is a QNX message from the caller straight
 * to this thread (the kernel routes the path here). No sockets, no polling.
 * While it handles a message, this thread runs at the caller's priority
 * (QNX priority inheritance through message passing), so a write from vision
 * at priority 32 is handled at 32 and a shell `echo` at 10.
 *
 * Creating a path needs root. As a normal user the engine logs a note and
 * carries on with UDP and HTTP only. */
#include "engine.h"
#include <stdio.h>
#include <string.h>

#ifdef __QNX__
#include <errno.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/neutrino.h>

static engine_t *g_e;
static resmgr_connect_funcs_t connect_funcs;
static resmgr_io_funcs_t io_funcs;
static iofunc_attr_t attr;

static int status_text(char *b, size_t cap) {
    mixer_snapshot_t s;
    engine_read_snapshot(g_e, &s);
    stats_t *st = &g_e->stats;
    int n = snprintf(b, cap,
        "Paper DJ   render: %s %d   deadline misses: %llu   commands: %llu   listeners: %d\n",
        atomic_load(&st->rt_render) ? "SCHED_FIFO" : "NOT real-time", g_e->prio_render,
        (unsigned long long)atomic_load(&st->deadline_misses),
        (unsigned long long)atomic_load(&st->cmds_applied), atomic_load(&st->clients));
    for (int i = 0; i < NDECKS && n < (int)cap; i++) {
        int p = (int)s.pos_s[i], l = (int)s.len_s[i];
        n += snprintf(b + n, cap - (size_t)n,
            "Deck %c  %-7s  %-30.30s  %d:%02d / %d:%02d  %6.2f BPM  vol %.2f  filter %+.2f%s%s%s%s\n",
            'A' + i, s.playing[i] ? "playing" : "paused", s.name[i], p / 60, p % 60, l / 60, l % 60,
            s.eff_bpm[i], s.param[i][P_VOL], s.param[i][P_FILTER],
            s.loop_on[i] ? "  LOOP" : "", s.sync_on[i] ? "  SYNC" : "",
            s.fx_on[i] ? "  FX" : "", s.scratching[i] ? "  SCRATCH" : "");
    }
    if (n < (int)cap)
        n += snprintf(b + n, cap - (size_t)n, "crossfader %.2f   master %.2f\n", s.xfade, s.master);
    return n < (int)cap ? n : (int)cap - 1;
}

static int pdj_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb) {
    int st = iofunc_write_verify(ctp, msg, ocb, NULL);
    if (st != EOK) return st;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;

    size_t n = _IO_WRITE_GET_NBYTES(msg);
    char buf[2048];
    size_t take = n < sizeof buf - 1 ? n : sizeof buf - 1;
    ssize_t got = resmgr_msgget(ctp, buf, take, sizeof msg->i);
    if (got < 0) return errno;
    buf[got] = 0;
    if (!engine_submit_text(g_e, buf, now_ns())) return EINVAL;  /* nothing parsed: tell the writer */

    _IO_SET_WRITE_NBYTES(ctp, n);
    ocb->attr->flags |= IOFUNC_ATTR_MTIME | IOFUNC_ATTR_CTIME;
    return EOK;
}

static int pdj_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb) {
    int st = iofunc_read_verify(ctp, msg, ocb, NULL);
    if (st != EOK) return st;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;

    static char text[1024];   /* one handler thread, so one buffer is enough */
    static int len;
    if (ocb->offset == 0) len = status_text(text, sizeof text);   /* fresh status per open */

    size_t nbytes = 0;
    if (ocb->offset < len) {
        nbytes = (size_t)(len - ocb->offset);
        if (nbytes > _IO_READ_GET_NBYTES(msg)) nbytes = _IO_READ_GET_NBYTES(msg);
    }
    MsgReply(ctp->rcvid, (long)nbytes, text + ocb->offset, nbytes);
    ocb->offset += (off_t)nbytes;
    if (nbytes) ocb->attr->flags |= IOFUNC_ATTR_ATIME;
    return _RESMGR_NOREPLY;
}

void *resmgr_thread(void *arg) {
    engine_t *e = g_e = arg;
    dispatch_t *dpp = dispatch_create();
    if (!dpp) { perror("[resmgr] dispatch_create"); return NULL; }

    resmgr_attr_t ra;
    memset(&ra, 0, sizeof ra);
    ra.nparts_max = 1;
    ra.msg_max_size = 2048;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs, _RESMGR_IO_NFUNCS, &io_funcs);
    io_funcs.read = pdj_read;
    io_funcs.write = pdj_write;
    iofunc_attr_init(&attr, S_IFCHR | 0666, NULL, NULL);   /* anyone may read/write */

    if (resmgr_attach(dpp, &ra, e->dev_path, _FTYPE_ANY, 0, &connect_funcs, &io_funcs, &attr) == -1) {
        fprintf(stderr, "[resmgr] %s not created (%s); run as root to enable it\n",
                e->dev_path, strerror(errno));
        return NULL;
    }
    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    if (!ctp) { perror("[resmgr] dispatch_context_alloc"); return NULL; }
    fprintf(stderr, "[resmgr] %s ready:  echo \"pad 1\" > %s   |   cat %s\n",
            e->dev_path, e->dev_path, e->dev_path);

    for (;;) {
        dispatch_context_t *r = dispatch_block(ctp);   /* wait for the next message */
        if (!r) { if (errno == EINTR) continue; perror("[resmgr] dispatch_block"); break; }
        ctp = r;
        dispatch_handler(ctp);
    }
    return NULL;
}

#else  /* not QNX: no resource managers, UDP and HTTP still work */

void *resmgr_thread(void *arg) { (void)arg; return NULL; }

#endif
