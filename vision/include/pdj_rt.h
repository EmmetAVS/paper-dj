/* Real-time helpers for the vision process.
 *
 * The priority ladder (higher number always wins on QNX):
 *   50 FIFO   audio render            (dj_engine)
 *   45 FIFO   command receiver        (dj_engine)
 *   40 FIFO   phone audio streams     (dj_engine)
 *   32 FIFO   camera capture + pads   (vision)   <- PDJ_PRIO_CAPTURE
 *   28 -> 8   hand-tracking model     (vision)   <- sporadic, PDJ_PRIO_MODEL_HI / _LO
 *   10        everything else (shell, logging, web page)
 * All values stay <= 63, the default limit for non-root processes on QNX.
 */
#ifndef PDJ_RT_H
#define PDJ_RT_H

#define PDJ_PRIO_CAPTURE   32
#define PDJ_PRIO_MODEL_HI  28
#define PDJ_PRIO_MODEL_LO   8

/* Calling thread -> SCHED_FIFO at prio. Returns 0 on success. */
int  pdj_rt_fifo(int prio, const char *who);

/* Calling thread -> SCHED_SPORADIC: runs at `high` for up to budget_ms of CPU
 * in every period_ms, then drops to `low` until the budget refills. This is
 * how the heavy model gets to run fast without ever starving anything above it.
 * Needs budget_ms < period_ms and high > low. Where sporadic scheduling isn't
 * available (e.g. Linux) it falls back to normal scheduling and says so. */
int  pdj_rt_sporadic(int high, int low, double budget_ms, double period_ms, const char *who);

/* Restricts the calling thread to the CPUs in `mask` (bit 0 = CPU 0).
 * QNX: ThreadCtl(_NTO_TCTL_RUNMASK). Linux: affinity. Returns 0 on success. */
int  pdj_rt_pin(unsigned mask, const char *who);

/* Locks all memory so page faults can't stall a real-time thread. */
void pdj_rt_lock_memory(void);

/* Prints the calling thread's current policy and priority. */
void pdj_rt_describe(const char *who);

/* CPU time used by the calling thread, in milliseconds. */
double pdj_thread_cpu_ms(void);

#endif
