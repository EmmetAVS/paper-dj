/* Two-deck DJ mixer: transport, hot cues, beat loops, sync, scratch,
 * 3-band EQ, one-knob filter, echo FX, two one-shot samples, crossfader. */
#ifndef PDJ_MIXER_H
#define PDJ_MIXER_H

#include "common.h"
#include "dsp.h"
#include "wav.h"

#define ECHO_FRAMES (1u << 17)          /* echo delay line, ~2.7 s */

typedef struct {
    float   *buf;                       /* stereo interleaved */
    uint32_t w;
    float    delay;                     /* smoothed delay in frames */
    float    send;                      /* smoothed 0..1 input amount */
    int      tail;                      /* frames left to keep running after FX off */
} echo_t;

typedef struct {
    track_t  track;
    char     name[64];

    /* transport */
    double   pos;                       /* playhead in source frames */
    double   vel;                       /* playhead speed, source frames per output frame */
    double   cue;
    double   pending_seek;              /* < 0 = none; applied once the gate fades out */
    double   bend;                      /* jog pitch-bend frames still to apply */
    int      playing;
    float    gate;                      /* 0..1 click-free start/stop envelope */

    /* beat grid */
    double   bpm;                       /* tempo at rate 1.0 */
    double   grid;                      /* first beat, source frames */

    double   hotcue[NHOTCUES];          /* < 0 = empty */

    int      loop_on;
    double   loop_in, loop_out;
    float    loop_beats;

    int      sync_on;
    uint32_t sync_seq;                  /* later-enabled deck follows the other */
    float    sync_adj;                  /* phase-correction speed offset */

    int      fx_on;
    echo_t   echo;

    int      scratching;
    int      scr_anchored;
    double   scr_target, scr_anchor_pos;
    float    scr_anchor_deg;

    float    target[NPARAM];            /* values set by commands */
    float    cur[NPARAM];               /* smoothed values actually used */
    float    eq_db[3];                  /* EQ gains the filters are built for */
    biquad_t eq[3];                     /* low shelf, mid peak, high shelf */
    float    filt_built;                /* filter knob value the filter is built for */
    biquad_t filt;
} deck_t;

typedef struct {
    track_t track;
    char    name[64];
    double  pos;
    int     active;
    float   vol_target, vol;
} sampler_t;

typedef struct {
    deck_t    deck[NDECKS];
    sampler_t sample[NSAMPLES];
    float     xfade_target, xfade;      /* 0 = all A, 1 = all B */
    float     master_target, master;
    int       quantize;                 /* snap cues / loops to the beat grid */
    uint32_t  sync_counter;
} mixer_t;

/* Read-only copy of the state for the /stats endpoint. */
typedef struct {
    double pos_s[NDECKS], len_s[NDECKS], cue_s[NDECKS];
    double hotcue_s[NDECKS][NHOTCUES];
    double loop_in_s[NDECKS], loop_out_s[NDECKS];
    double bpm[NDECKS], eff_bpm[NDECKS], phase[NDECKS];
    int    playing[NDECKS], loop_on[NDECKS], sync_on[NDECKS], fx_on[NDECKS], scratching[NDECKS];
    float  loop_beats[NDECKS];
    float  param[NDECKS][NPARAM];
    int    sample_active[NSAMPLES];
    float  sample_vol[NSAMPLES];
    char   sample_name[NSAMPLES][64];
    float  xfade, master, peak;
    int    quantize;
    char   name[NDECKS][64];
} mixer_snapshot_t;

/* Allocates effect buffers. Call once, before the render thread starts. */
int   mixer_init(mixer_t *m);
/* Call after a deck's track is loaded: detects BPM and the beat grid. */
void  mixer_analyze_deck(mixer_t *m, int deck);
/* Re-detects around a known tempo (+-2 BPM) to lock the exact BPM and beat grid. */
void  mixer_analyze_deck_hint(mixer_t *m, int deck, double bpm_hint);
/* Parses one text command like "a.vol 0.7", "pad 5" or "xfade 0.3". 0 = ok. */
int   cmd_parse(const char *text, cmd_t *out);
void  mixer_apply(mixer_t *m, const cmd_t *c);
/* Renders `frames` stereo frames of int16 into out. Returns the peak level. */
float mixer_render(mixer_t *m, int16_t *out, int frames);
void  mixer_snapshot(const mixer_t *m, mixer_snapshot_t *s);

/* What each of the 16 paper pads does (index 0 = pad 1). */
extern const char *const PAD_MAP[16];

#endif
