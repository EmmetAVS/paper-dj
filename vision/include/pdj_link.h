/* Sends board commands from the vision process to dj_engine over UDP.
 *
 * Two message tiers, matching what a listener can hear:
 *  - Triggers (pads, scratch on/off) go out immediately, in order.
 *  - Slider values are coalesced: only the newest value per control is sent,
 *    at most ~60 times a second, and tiny changes are skipped. A jittery hand
 *    can never flood the engine.
 * Before any trigger is sent, pending slider values are flushed first, so the
 * engine always sees events in the order they happened (e.g. the last platter
 * angle arrives before "scratch 0").
 *
 * Thread-safe: the capture thread and the model thread can share one link.
 */
#ifndef PDJ_LINK_H
#define PDJ_LINK_H

#include <stdint.h>

typedef struct pdj_link pdj_link_t;

pdj_link_t *pdj_link_open(const char *host, int port);   /* NULL on failure */
void        pdj_link_close(pdj_link_t *l);

/* Triggers: sent now. */
int  pdj_pad(pdj_link_t *l, int pad);                     /* pad 1..16 */
int  pdj_send_now(pdj_link_t *l, const char *cmd);        /* any raw command, e.g. "a.looplen 8" */
int  pdj_scratch_begin(pdj_link_t *l, char deck);        /* hand lands on the platter ('a' or 'b') */
/* Hand angle while touching, in degrees, unwrapped (keeps counting past 360).
 * Only the change since the first angle after pdj_scratch_begin matters:
 * +360 = 1.8 s forward, negative = backwards. Coalesced. */
void pdj_scratch_angle(pdj_link_t *l, char deck, float unwrapped_deg);
int  pdj_scratch_end(pdj_link_t *l, char deck);          /* hand lifts */

/* Sliders: coalesced. key is e.g. "a.vol", "b.filter". */
void pdj_set(pdj_link_t *l, const char *key, float value);

/* Call once per processed frame: sends pending slider values if >= 16 ms since
 * the last send. pdj_flush sends them regardless. */
int  pdj_tick(pdj_link_t *l);
int  pdj_flush(pdj_link_t *l);

typedef struct {
    uint64_t packets;        /* UDP packets sent */
    uint64_t triggers;       /* immediate commands */
    uint64_t updates;        /* pdj_set calls */
    uint64_t values_sent;    /* slider values that actually went out */
} pdj_link_stats_t;
void pdj_link_stats(pdj_link_t *l, pdj_link_stats_t *out);

/* Turns raw angles in (-180, 180] (e.g. from atan2) into a continuous angle.
 * Keep accum/last_raw/have per hand; reset have = 0 at each touch-down. */
float pdj_unwrap_deg(float *accum, float *last_raw, int *have, float raw_deg);

/* ------------------------------------------------------------------------
 * Pad debouncer: turns per-frame "is this pad covered?" into clean presses.
 * A press fires after `press_frames` covered frames in a row; the pad re-arms
 * after `release_frames` uncovered frames, so holding a finger down never
 * repeats. */
typedef struct {
    uint8_t covered_run[16];
    uint8_t clear_run[16];
    uint8_t held[16];
    uint8_t press_frames, release_frames;
} pdj_pads_t;

void pdj_pads_init(pdj_pads_t *p, int press_frames, int release_frames);
/* covered[i] != 0 if pad i+1 is covered in this frame. Sends each new press
 * through the link and returns how many presses fired. */
int  pdj_pads_update(pdj_pads_t *p, const uint8_t covered[16], pdj_link_t *l);

#endif
