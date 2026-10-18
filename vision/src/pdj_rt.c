#include "pdj_rt.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#ifdef __QNX__
#include <sys/neutrino.h>
#endif

static void ms_to_ts(double ms, struct timespec *ts) {
    ts->tv_sec = (time_t)(ms / 1000.0);
    ts->tv_nsec = (long)((ms - ts->tv_sec * 1000.0) * 1e6);
}

int pdj_rt_fifo(int prio, const char *who) {
    struct sched_param sp;
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = prio;
    int err = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (err) {
        fprintf(stderr, "[%s] SCHED_FIFO %d refused (%s); staying at normal priority\n", who, prio, strerror(err));
        return -1;
    }
    fprintf(stderr, "[%s] SCHED_FIFO priority %d\n", who, prio);
    return 0;
}

int pdj_rt_sporadic(int high, int low, double budget_ms, double period_ms, const char *who) {
    if (high <= low || budget_ms <= 0 || budget_ms >= period_ms) {
        fprintf(stderr, "[%s] bad sporadic settings (need high > low and 0 < budget < period)\n", who);
        return -1;
    }
#ifdef SCHED_SPORADIC
    struct sched_param sp;
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = high;
    sp.sched_ss_low_priority = low;
    sp.sched_ss_max_repl = 5;   /* must be < SS_REPL_MAX */
    ms_to_ts(period_ms, &sp.sched_ss_repl_period);
    ms_to_ts(budget_ms, &sp.sched_ss_init_budget);
    int err = pthread_setschedparam(pthread_self(), SCHED_SPORADIC, &sp);
    if (err) {
        fprintf(stderr, "[%s] SCHED_SPORADIC refused (%s); staying at normal priority\n", who, strerror(err));
        return -1;
    }
    fprintf(stderr, "[%s] SCHED_SPORADIC %d -> %d, budget %.1f ms every %.1f ms (%.0f%% of a core max at %d)\n",
            who, high, low, budget_ms, period_ms, 100.0 * budget_ms / period_ms, high);
    return 0;
#else
    (void)ms_to_ts;
    fprintf(stderr, "[%s] SCHED_SPORADIC not available on this OS; running at normal priority "
                    "(on QNX this thread gets %d -> %d with a %.0f/%.0f ms budget)\n",
            who, high, low, budget_ms, period_ms);
    return -1;
#endif
}

int pdj_rt_pin(unsigned mask, const char *who) {
#if defined(__QNX__)
    if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)mask) == -1) {
        fprintf(stderr, "[%s] runmask 0x%x failed (%s)\n", who, mask, strerror(errno));
        return -1;
    }
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    for (unsigned c = 0; c < 32; c++) if (mask & (1u << c)) CPU_SET(c, &set);
    int err = pthread_setaffinity_np(pthread_self(), sizeof set, &set);
    if (err) { fprintf(stderr, "[%s] affinity 0x%x failed (%s)\n", who, mask, strerror(err)); return -1; }
#else
    fprintf(stderr, "[%s] CPU pinning not supported here\n", who);
    return -1;
#endif
    fprintf(stderr, "[%s] pinned to CPU mask 0x%x\n", who, mask);
    return 0;
}

void pdj_rt_lock_memory(void) {
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        fprintf(stderr, "[rt] mlockall failed (%s); continuing\n", strerror(errno));
}

void pdj_rt_describe(const char *who) {
    int policy;
    struct sched_param sp;
    if (pthread_getschedparam(pthread_self(), &policy, &sp) != 0) return;
    const char *name = policy == SCHED_FIFO ? "FIFO" : policy == SCHED_RR ? "RR" :
#ifdef SCHED_SPORADIC
                       policy == SCHED_SPORADIC ? "SPORADIC" :
#endif
                       "OTHER";
    fprintf(stderr, "[%s] running %s priority %d\n", who, name, sp.sched_priority);
}

double pdj_thread_cpu_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}
