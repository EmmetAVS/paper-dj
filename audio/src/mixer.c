#include "mixer.h"
#include "bpm.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- tuning ------------------------------------------------------------- */
#define EQ_LOW_HZ    250.0f
#define EQ_MID_HZ   1000.0f
#define EQ_MID_Q       0.7f
#define EQ_HIGH_HZ  4000.0f
#define FILTER_Q       0.9f    /* slight resonance, like a DJM filter */
#define GATE_MS        4.0f    /* fade for play/pause/jumps */
#define PLATTER_SEC    1.8     /* one platter revolution = 1.8 s of audio (33 1/3 rpm) */
#define SYNC_MAX_ADJ   0.02f   /* max speed offset used to hold phase */
#define DEFAULT_LOOP   4.0f    /* beats */

/* The 16 paper pads. Change the board by editing this table. */
const char *const PAD_MAP[16] = {
    "a.toggle",   "a.cue",      "b.toggle",   "b.cue",       /* 1-4  */
    "a.hotcue 1", "a.hotcue 2", "b.hotcue 1", "b.hotcue 2",  /* 5-8  */
    "a.loop",     "a.sync",     "b.loop",     "b.sync",      /* 9-12 */
    "a.fx",       "s1.play",    "b.fx",       "s2.play",     /* 13-16 */
};

static const float defaults[NPARAM] = { 0.8f,   0.0f,   0.0f,   0.0f, 1.0f, 0.0f,  0.0f, 0.5f };
static const float pmin[NPARAM]     = { 0.0f, -40.0f, -40.0f, -40.0f, 0.5f, -0.5f, -1.0f, 0.0f };
static const float pmax[NPARAM]     = { 1.0f,   6.0f,   6.0f,   6.0f, 1.5f,  0.5f,  1.0f, 1.0f };
static const char *param_names[NPARAM] = { "vol", "low", "mid", "high", "rate", "nudge", "filter", "fxamt" };

/* ---- beat-grid helpers --------------------------------------------------- */

static double beat_frames(const deck_t *d) {
    return d->bpm > 0 ? d->track.rate * 60.0 / d->bpm : d->track.rate * 0.5;  /* fallback: 120 BPM */
}
static double speed(const deck_t *d) { return d->cur[P_RATE] + d->cur[P_NUDGE] + d->sync_adj; }
static double eff_bpm(const deck_t *d) { return (d->bpm > 0 ? d->bpm : 120.0) * speed(d); }
static double beat_phase(const deck_t *d) {
    double b = (d->pos - d->grid) / beat_frames(d);
    return b - floor(b);
}
static double clamp_pos(const deck_t *d, double p) {
    double hi = d->track.frames > 1 ? (double)(d->track.frames - 2) : 0;
    return p < 0 ? 0 : (p > hi ? hi : p);
}
static double snap(const mixer_t *m, const deck_t *d, double p) {
    if (!m->quantize) return clamp_pos(d, p);
    double bf = beat_frames(d);
    return clamp_pos(d, d->grid + round((p - d->grid) / bf) * bf);
}
static double wrap_half(double x) { return x - floor(x + 0.5); }  /* -> [-0.5, 0.5) */

/* ---- filters ------------------------------------------------------------- */

static void build_eq(deck_t *d, int band) {
    float g = d->cur[P_LOW + band];
    d->eq_db[band] = g;
    if (band == 0) bq_lowshelf(&d->eq[0], OUT_RATE, EQ_LOW_HZ, g);
    if (band == 1) bq_peak(&d->eq[1], OUT_RATE, EQ_MID_HZ, EQ_MID_Q, g);
    if (band == 2) bq_highshelf(&d->eq[2], OUT_RATE, EQ_HIGH_HZ, g);
}

/* One knob: left = low-pass sweeping down, right = high-pass sweeping up. */
static void build_filter(deck_t *d) {
    float x = d->cur[P_FILTER];
    d->filt_built = x;
    if (x < 0) bq_lowpass(&d->filt, OUT_RATE, 20000.0f * powf(60.0f / 20000.0f, -x), FILTER_Q);
    else       bq_highpass(&d->filt, OUT_RATE, 15.0f * powf(8000.0f / 15.0f, x), FILTER_Q);
}

/* ---- init ---------------------------------------------------------------- */

int mixer_init(mixer_t *m) {
    for (int i = 0; i < NDECKS; i++) {
        deck_t *d = &m->deck[i];
        d->pos = 0; d->vel = 0; d->cue = 0; d->pending_seek = -1; d->bend = 0;
        d->playing = 0; d->gate = 0;
        for (int h = 0; h < NHOTCUES; h++) d->hotcue[h] = -1;
        d->loop_on = 0; d->loop_beats = DEFAULT_LOOP;
        d->sync_on = 0; d->sync_adj = 0;
        d->fx_on = 0; d->scratching = 0;
        for (int p = 0; p < NPARAM; p++) d->target[p] = d->cur[p] = defaults[p];
        for (int b = 0; b < 3; b++) { bq_reset(&d->eq[b]); build_eq(d, b); }
        bq_reset(&d->filt); build_filter(d);
        memset(&d->echo, 0, sizeof d->echo);
        d->echo.buf = calloc(ECHO_FRAMES * 2, sizeof(float));
        if (!d->echo.buf) return -1;
    }
    for (int s = 0; s < NSAMPLES; s++) {
        m->sample[s].active = 0;
        m->sample[s].vol_target = m->sample[s].vol = 0.8f;
    }
    m->xfade_target = m->xfade = 0.5f;
    m->master_target = m->master = 0.9f;
    m->quantize = 1;
    m->sync_counter = 0;
    return 0;
}

void mixer_analyze_deck(mixer_t *m, int i) {
    deck_t *d = &m->deck[i];
    double fb;
    double bpm = bpm_detect(&d->track, &fb);
    if (bpm > 0) { d->bpm = bpm; d->grid = fb; }
    fprintf(stderr, "[deck %c] %s: %.2f BPM, first beat at %.3f s\n", 'A' + i, d->name,
            d->bpm, d->track.rate ? d->grid / d->track.rate : 0.0);
}

/* ---- command parsing ----------------------------------------------------- */

enum { NOVAL, NEEDVAL, OPTVAL };
static const struct { const char *name; int op; int val; } deck_verbs[] = {
    { "play", CMD_PLAY, NOVAL },       { "pause", CMD_PAUSE, NOVAL },   { "toggle", CMD_TOGGLE, NOVAL },
    { "cue", CMD_CUE, NOVAL },         { "setcue", CMD_SETCUE, NOVAL }, { "seek", CMD_SEEK, NEEDVAL },
    { "hotcue", CMD_HOTCUE, NEEDVAL }, { "hotcue.clear", CMD_HOTCUE_CLEAR, NEEDVAL },
    { "loop", CMD_LOOP, OPTVAL },      { "looplen", CMD_LOOPLEN, NEEDVAL },
    { "sync", CMD_SYNC, OPTVAL },      { "fx", CMD_FX, OPTVAL },
    { "scratch", CMD_SCRATCH, NEEDVAL }, { "platter", CMD_PLATTER, NEEDVAL }, { "jog", CMD_JOG, NEEDVAL },
    { "bpm", CMD_BPM, NEEDVAL },
};

int cmd_parse(const char *text, cmd_t *out) {
    char word[32] = {0};
    const char *p = text;
    while (*p == ' ' || *p == '\t') p++;
    size_t n = 0;
    while (*p && *p != ' ' && *p != '\t' && n < sizeof word - 1) word[n++] = *p++;
    if (n == 0) return -1;
    char *end;
    float v = strtof(p, &end);
    int has_value = end != p;

    memset(out, 0, sizeof *out);

    if (!strcasecmp(word, "pad")) {                       /* pad 1..16 -> board mapping */
        int k = (int)v;
        if (!has_value || k < 1 || k > 16) return -1;
        return cmd_parse(PAD_MAP[k - 1], out);
    }
    if (!strcasecmp(word, "xfade") || !strcasecmp(word, "master")) {
        if (!has_value) return -1;
        out->op = (word[0] | 0x20) == 'x' ? CMD_XFADE : CMD_MASTER;
        out->value = clampf(v, 0.0f, 1.0f);
        return 0;
    }
    if (!strcasecmp(word, "quantize")) {
        out->op = CMD_QUANTIZE;
        out->value = has_value ? (v != 0) : -1;
        return 0;
    }
    /* samples: s1.play / s2.stop / s1.vol 0.7 */
    if ((word[0] | 0x20) == 's' && word[1] >= '1' && word[1] < '1' + NSAMPLES && word[2] == '.') {
        out->deck = (uint8_t)(word[1] - '1');
        const char *verb = word + 3;
        if (!strcasecmp(verb, "play"))  { out->op = CMD_SAMPLE_PLAY; return 0; }
        if (!strcasecmp(verb, "stop"))  { out->op = CMD_SAMPLE_STOP; return 0; }
        if (!strcasecmp(verb, "vol") && has_value) { out->op = CMD_SAMPLE_VOL; out->value = clampf(v, 0, 1); return 0; }
        return -1;
    }
    /* decks: a.<verb> / b.<verb> */
    if (n < 3 || word[1] != '.') return -1;
    char dk = word[0] | 0x20;
    if (dk != 'a' && dk != 'b') return -1;
    out->deck = (uint8_t)(dk - 'a');
    const char *verb = word + 2;

    for (size_t i = 0; i < sizeof deck_verbs / sizeof *deck_verbs; i++) {
        if (strcasecmp(verb, deck_verbs[i].name)) continue;
        out->op = (uint8_t)deck_verbs[i].op;
        if (deck_verbs[i].val == NEEDVAL && !has_value) return -1;
        out->value = deck_verbs[i].val == OPTVAL ? (has_value ? (v != 0) : -1) : v;
        if (out->op == CMD_HOTCUE || out->op == CMD_HOTCUE_CLEAR) {
            int h = (int)v;
            if (h < 1 || h > NHOTCUES) return -1;
            out->param = (uint8_t)(h - 1);
        }
        if (out->op == CMD_SEEK && v < 0) return -1;
        if (out->op == CMD_LOOPLEN) out->value = clampf(v, 0.25f, 32.0f);
        if (out->op == CMD_BPM) out->value = clampf(v, 40.0f, 250.0f);
        return 0;
    }
    for (int i = 0; i < NPARAM; i++) {
        if (strcasecmp(verb, param_names[i])) continue;
        if (!has_value) return -1;
        out->op = CMD_PARAM;
        out->param = (uint8_t)i;
        out->value = clampf(v, pmin[i], pmax[i]);
        return 0;
    }
    return -1;
}

/* ---- applying commands --------------------------------------------------- */

static int want(int current, float value) { return value < 0 ? !current : value != 0; }

static void start_loop(const mixer_t *m, deck_t *d) {
    double len = d->loop_beats * beat_frames(d);
    double in = m->quantize ? snap(m, d, d->pos) : d->pos;
    if (in > d->pos + 1) in -= beat_frames(d) * ceil((in - d->pos) / beat_frames(d));  /* never start ahead */
    if (in < 0) in = 0;
    d->loop_in = in;
    d->loop_out = clamp_pos(d, in + len);
    d->loop_on = d->loop_out > d->loop_in + 64;
}

static void engage_sync(mixer_t *m, int i) {
    deck_t *d = &m->deck[i], *o = &m->deck[1 - i];
    d->sync_on = 1;
    d->sync_seq = ++m->sync_counter;
    if (d->bpm <= 0 || o->bpm <= 0) return;
    float r = clampf((float)(eff_bpm(o) / d->bpm), 0.5f, 1.5f);
    d->target[P_RATE] = d->cur[P_RATE] = r;
    /* if both are playing, jump straight onto the other deck's beat */
    if (d->playing && o->playing && d->pending_seek < 0 && !d->scratching) {
        double err = wrap_half(beat_phase(o) - beat_phase(d));
        d->pending_seek = clamp_pos(d, d->pos + err * beat_frames(d) + 2 * GATE_MS * 0.001 * d->track.rate * r);
    }
}

void mixer_apply(mixer_t *m, const cmd_t *c) {
    switch (c->op) {
    case CMD_XFADE:    m->xfade_target = c->value; return;
    case CMD_MASTER:   m->master_target = c->value; return;
    case CMD_QUANTIZE: m->quantize = want(m->quantize, c->value); return;
    case CMD_SAMPLE_PLAY: {
        sampler_t *s = &m->sample[c->deck];
        if (s->track.frames > 1) { s->pos = 0; s->active = 1; }  /* retrigger from the top */
        return;
    }
    case CMD_SAMPLE_STOP: m->sample[c->deck].active = 0; return;
    case CMD_SAMPLE_VOL:  m->sample[c->deck].vol_target = c->value; return;
    default: break;
    }

    deck_t *d = &m->deck[c->deck];
    int loaded = d->track.frames > 1;
    switch (c->op) {
    case CMD_PLAY:   d->playing = loaded; break;
    case CMD_PAUSE:  d->playing = 0; break;
    case CMD_TOGGLE: d->playing = !d->playing && loaded; break;
    case CMD_CUE:
        /* CDJ-style: playing -> back to cue and stop; stopped -> set cue here */
        if (d->playing) { d->playing = 0; d->pending_seek = d->cue; d->loop_on = 0; }
        else d->cue = snap(m, d, d->pos);
        break;
    case CMD_SETCUE: d->cue = snap(m, d, d->pos); break;
    case CMD_SEEK:
        d->pending_seek = clamp_pos(d, c->value * (d->track.rate ? d->track.rate : OUT_RATE));
        d->loop_on = 0;
        break;
    case CMD_HOTCUE: {
        double *h = &d->hotcue[c->param];
        if (*h < 0) *h = snap(m, d, d->pos);            /* empty: store */
        else if (loaded) {                               /* set: jump there and play */
            d->pending_seek = *h;
            d->playing = 1;
            d->loop_on = 0;
        }
        break;
    }
    case CMD_HOTCUE_CLEAR: d->hotcue[c->param] = -1; break;
    case CMD_LOOP:
        if (want(d->loop_on, c->value)) { if (!d->loop_on) start_loop(m, d); }
        else d->loop_on = 0;
        break;
    case CMD_LOOPLEN:
        d->loop_beats = c->value;
        if (d->loop_on) d->loop_out = clamp_pos(d, d->loop_in + d->loop_beats * beat_frames(d));
        break;
    case CMD_SYNC:
        if (want(d->sync_on, c->value)) { if (!d->sync_on) engage_sync(m, c->deck); }
        else { d->sync_on = 0; d->sync_adj = 0; }      /* keep the matched tempo */
        break;
    case CMD_FX: d->fx_on = want(d->fx_on, c->value); break;
    case CMD_SCRATCH:
        if (c->value != 0 && !d->scratching) {
            d->scratching = 1; d->scr_anchored = 0; d->scr_target = d->pos;
        } else if (c->value == 0) d->scratching = 0;
        break;
    case CMD_PLATTER:
        if (!d->scratching) { d->scratching = 1; d->scr_anchored = 0; d->scr_target = d->pos; }
        if (!d->scr_anchored) {
            d->scr_anchored = 1; d->scr_anchor_deg = c->value; d->scr_anchor_pos = d->pos; d->scr_target = d->pos;
        } else {
            d->scr_target = d->scr_anchor_pos + (c->value - d->scr_anchor_deg) / 360.0 * PLATTER_SEC * d->track.rate;
        }
        break;
    case CMD_JOG: {
        double frames = c->value / 360.0 * PLATTER_SEC * d->track.rate;
        if (d->scratching) d->scr_target += frames;
        else d->bend += frames * 0.25;                  /* spinning the edge = gentle pitch bend */
        break;
    }
    case CMD_BPM: d->bpm = c->value; break;
    case CMD_PARAM: d->target[c->param] = c->value; break;
    }
}

/* ---- rendering ----------------------------------------------------------- */

static inline float soft_clip(float x) {
    const float knee = 0.7f;
    float a = fabsf(x);
    if (a <= knee) return x;
    float y = knee + (1.0f - knee) * tanhf((a - knee) / (1.0f - knee));
    return x < 0 ? -y : y;
}

static inline void read_frame(const track_t *t, double pos, float *l, float *r) {
    size_t i0 = (size_t)pos;
    float fr = (float)(pos - (double)i0);
    const float *s = t->data + 2 * i0;
    *l = s[0] + (s[2] - s[0]) * fr;
    *r = s[1] + (s[3] - s[1]) * fr;
}

/* Per-block control work: smoothing EQ/filter, sync, echo timing. */
static void block_update(mixer_t *m, int frames) {
    const float a_blk = 1.0f - expf(-(float)frames / (0.020f * OUT_RATE));
    for (int i = 0; i < NDECKS; i++) {
        deck_t *d = &m->deck[i], *o = &m->deck[1 - i];
        for (int b = 0; b < 3; b++) {
            float *cur = &d->cur[P_LOW + b], tgt = d->target[P_LOW + b];
            *cur += (tgt - *cur) * a_blk;
            if (fabsf(*cur - tgt) < 0.01f) *cur = tgt;
            if (fabsf(*cur - d->eq_db[b]) > 0.02f) build_eq(d, b);
        }
        float *f = &d->cur[P_FILTER];
        *f += (d->target[P_FILTER] - *f) * a_blk;
        if (fabsf(*f - d->target[P_FILTER]) < 0.001f) *f = d->target[P_FILTER];
        if (fabsf(*f - d->filt_built) > 0.002f) build_filter(d);

        /* sync: follow the other deck's tempo, and nudge speed to hold the phase.
         * If both decks are synced, the one that enabled sync last follows. */
        d->sync_adj = 0;
        int follow = d->sync_on && !(o->sync_on && o->sync_seq > d->sync_seq);
        if (follow && d->bpm > 0 && o->bpm > 0) {
            d->target[P_RATE] = clampf((float)((o->bpm * speed(o)) / d->bpm), 0.5f, 1.5f);
            if (d->gate > 0 && o->gate > 0 && !d->scratching && !o->scratching && d->pending_seek < 0) {
                double err = wrap_half(beat_phase(o) - beat_phase(d));   /* in beats */
                d->sync_adj = clampf((float)(err * 60.0 / d->bpm), -SYNC_MAX_ADJ, SYNC_MAX_ADJ);
            }
        }
    }
}

float mixer_render(mixer_t *m, int16_t *out, int frames) {
    const float a_fast = 1.0f - expf(-1.0f / (0.010f * OUT_RATE));   /* 10 ms */
    const float a_rate = 1.0f - expf(-1.0f / (0.030f * OUT_RATE));   /* 30 ms */
    const float a_vel  = 1.0f - expf(-1.0f / (0.004f * OUT_RATE));   /* playhead inertia */
    const float a_del  = 1.0f - expf(-1.0f / (0.080f * OUT_RATE));   /* echo time glide */
    const float gate_step = 1.0f / (GATE_MS * 0.001f * OUT_RATE);
    const double scr_close = 0.015 * OUT_RATE;                        /* platter follow time */
    float peak = 0.0f;

    block_update(m, frames);

    float delay_target[NDECKS];
    for (int i = 0; i < NDECKS; i++)  /* echo: half a beat at the deck's current tempo */
        delay_target[i] = clampf((float)(30.0 / eff_bpm(&m->deck[i]) * OUT_RATE), 480.0f, ECHO_FRAMES - 4.0f);

    for (int n = 0; n < frames; n++) {
        m->xfade  += (m->xfade_target  - m->xfade)  * a_fast;
        m->master += (m->master_target - m->master) * a_fast;
        float g[NDECKS] = { cosf(m->xfade * (float)PDJ_PI * 0.5f), sinf(m->xfade * (float)PDJ_PI * 0.5f) };
        float L = 0.0f, R = 0.0f;

        for (int i = 0; i < NDECKS; i++) {
            deck_t *d = &m->deck[i];
            d->cur[P_VOL]   += (d->target[P_VOL]   - d->cur[P_VOL])   * a_fast;
            d->cur[P_FXAMT] += (d->target[P_FXAMT] - d->cur[P_FXAMT]) * a_fast;
            d->cur[P_RATE]  += (d->target[P_RATE]  - d->cur[P_RATE])  * a_rate;
            d->cur[P_NUDGE] += (d->target[P_NUDGE] - d->cur[P_NUDGE]) * a_rate;

            int open = (d->playing || d->scratching) && d->pending_seek < 0;
            if (open) { d->gate += gate_step; if (d->gate > 1.0f) d->gate = 1.0f; }
            else      { d->gate -= gate_step; if (d->gate < 0.0f) d->gate = 0.0f; }
            if (d->gate == 0.0f && d->pending_seek >= 0) {
                d->pos = d->pending_seek; d->pending_seek = -1;
                if (d->scratching) { d->scr_target = d->pos; d->scr_anchored = 0; }
            }

            float l = 0.0f, r = 0.0f;
            if (d->gate > 0.0f && d->track.frames > 1) {
                double ratio = (double)d->track.rate / OUT_RATE;
                double desired;
                if (d->scratching) {
                    desired = (d->scr_target - d->pos) / scr_close;
                    if (desired >  12 * ratio) desired =  12 * ratio;
                    if (desired < -12 * ratio) desired = -12 * ratio;
                } else {
                    desired = speed(d) * ratio;
                }
                d->vel += (desired - d->vel) * a_vel;

                d->pos = clamp_pos(d, d->pos);
                read_frame(&d->track, d->pos, &l, &r);
                for (int b = 0; b < 3; b++) { l = bq_run(&d->eq[b], 0, l); r = bq_run(&d->eq[b], 1, r); }
                l = bq_run(&d->filt, 0, l);
                r = bq_run(&d->filt, 1, r);

                double step = d->vel;
                if (d->bend != 0.0) { double x = d->bend * 0.0005; step += x; d->bend -= x; if (fabs(d->bend) < 0.01) d->bend = 0; }
                d->pos += step;

                if (d->loop_on && !d->scratching && d->pos >= d->loop_out)
                    d->pos -= d->loop_out - d->loop_in;
                if (!d->scratching && d->pos >= (double)(d->track.frames - 2)) {   /* end of track */
                    d->playing = 0; d->gate = 0.0f; d->pos = (double)(d->track.frames - 2); d->vel = 0;
                }
            } else {
                d->vel = 0;
            }

            float gain = d->cur[P_VOL] * d->gate * g[i];
            float ol = l * gain, or_ = r * gain;

            /* echo is post-fader, so pulling the fader down leaves the echo tail ringing */
            echo_t *e = &d->echo;
            if (d->fx_on || e->tail > 0) {
                e->send  += ((d->fx_on ? 1.0f : 0.0f) - e->send) * a_fast;
                e->delay += (delay_target[i] - e->delay) * a_del;
                float rp = (float)e->w - e->delay;
                if (rp < 0) rp += ECHO_FRAMES;
                uint32_t i0 = (uint32_t)rp, i1 = (i0 + 1) & (ECHO_FRAMES - 1);
                float fr = rp - (float)i0;
                float wl = e->buf[2 * i0] + (e->buf[2 * i1] - e->buf[2 * i0]) * fr;
                float wr = e->buf[2 * i0 + 1] + (e->buf[2 * i1 + 1] - e->buf[2 * i0 + 1]) * fr;
                float amt = d->cur[P_FXAMT];
                float fb = 0.30f + 0.35f * amt, wet = 0.35f + 0.45f * amt;
                e->buf[2 * e->w]     = ol * e->send + wl * fb;
                e->buf[2 * e->w + 1] = or_ * e->send + wr * fb;
                e->w = (e->w + 1) & (ECHO_FRAMES - 1);
                ol += wl * wet;
                or_ += wr * wet;
                if (d->fx_on) e->tail = 4 * OUT_RATE;   /* tail has decayed > 60 dB by the end */
                else e->tail--;
            } else if (e->delay == 0.0f) {
                e->delay = delay_target[i];
            }
            L += ol;
            R += or_;
        }

        /* one-shot samples go straight to the master, independent of the crossfader */
        for (int s = 0; s < NSAMPLES; s++) {
            sampler_t *sp = &m->sample[s];
            sp->vol += (sp->vol_target - sp->vol) * a_fast;
            if (!sp->active) continue;
            float l, r;
            read_frame(&sp->track, sp->pos, &l, &r);
            L += l * sp->vol;
            R += r * sp->vol;
            sp->pos += (double)sp->track.rate / OUT_RATE;
            if (sp->pos >= (double)(sp->track.frames - 2)) sp->active = 0;
        }

        L = soft_clip(L * m->master);
        R = soft_clip(R * m->master);
        float a = fabsf(L) > fabsf(R) ? fabsf(L) : fabsf(R);
        if (a > peak) peak = a;
        out[2 * n]     = (int16_t)lrintf(clampf(L, -1.0f, 1.0f) * 32767.0f);
        out[2 * n + 1] = (int16_t)lrintf(clampf(R, -1.0f, 1.0f) * 32767.0f);
    }
    return peak;
}

void mixer_snapshot(const mixer_t *m, mixer_snapshot_t *s) {
    for (int i = 0; i < NDECKS; i++) {
        const deck_t *d = &m->deck[i];
        double rate = d->track.rate ? d->track.rate : OUT_RATE;
        s->pos_s[i] = d->pos / rate;
        s->len_s[i] = (double)d->track.frames / rate;
        s->cue_s[i] = d->cue / rate;
        for (int h = 0; h < NHOTCUES; h++) s->hotcue_s[i][h] = d->hotcue[h] < 0 ? -1 : d->hotcue[h] / rate;
        s->loop_on[i] = d->loop_on;
        s->loop_beats[i] = d->loop_beats;
        s->loop_in_s[i] = d->loop_in / rate;
        s->loop_out_s[i] = d->loop_out / rate;
        s->bpm[i] = d->bpm;
        s->eff_bpm[i] = eff_bpm(d);
        s->phase[i] = beat_phase(d);
        s->playing[i] = d->playing;
        s->sync_on[i] = d->sync_on;
        s->fx_on[i] = d->fx_on;
        s->scratching[i] = d->scratching;
        memcpy(s->param[i], d->target, sizeof d->target);
        memcpy(s->name[i], d->name, sizeof d->name);
    }
    for (int k = 0; k < NSAMPLES; k++) {
        s->sample_active[k] = m->sample[k].active;
        s->sample_vol[k] = m->sample[k].vol_target;
        memcpy(s->sample_name[k], m->sample[k].name, sizeof m->sample[k].name);
    }
    s->xfade = m->xfade_target;
    s->master = m->master_target;
    s->quantize = m->quantize;
}
