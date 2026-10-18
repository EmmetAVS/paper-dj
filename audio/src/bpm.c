/* Tempo detection by comb-filtering an onset envelope.
 *
 * 1. Mono energy per 2.7 ms hop -> log -> positive differences = onset strength.
 * 2. For each candidate BPM and each phase, sum the onset strength at every
 *    predicted beat. The best (BPM, phase) pair is the beat grid.
 * 3. Coarse search 70-180 BPM in 0.5 steps, then refine +-0.6 BPM in 0.01 steps.
 * A mild preference toward ~120 BPM resolves half/double-tempo ambiguity.
 */
#include "bpm.h"
#include <math.h>
#include <stdlib.h>

static double score(const float *on, size_t n, double period, double *best_phase) {
    double best = -1;
    int nph = (int)period;
    for (int ph = 0; ph < nph; ph++) {
        double s = 0;
        int cnt = 0;
        for (double p = ph; ; p += period, cnt++) {
            size_t k = (size_t)(p + 0.5);
            if (k >= n) break;
            s += on[k];
        }
        if (cnt) s /= cnt;
        if (s > best) { best = s; *best_phase = ph; }
    }
    return best;
}

static double tempo_weight(double bpm) {
    double o = log2(bpm / 120.0);
    return exp(-0.5 * (o / 0.9) * (o / 0.9));
}

static double detect(const track_t *t, double lo, double hi, int weighted, double *first_beat) {
    *first_beat = 0;
    if (!t->data || t->frames < (size_t)t->rate * 4) return 0;
    size_t hop = (size_t)(t->rate / 375);  /* 128 at 48 kHz */
    size_t n = t->frames / hop;
    float *env = malloc(n * sizeof *env), *on = malloc(n * sizeof *on);
    if (!env || !on) { free(env); free(on); return 0; }

    for (size_t i = 0; i < n; i++) {
        double e = 0;
        for (size_t j = i * hop; j < (i + 1) * hop; j++) {
            float m = 0.5f * (t->data[2 * j] + t->data[2 * j + 1]);
            e += m * m;
        }
        env[i] = (float)log(1e-9 + e);
    }
    on[0] = 0;
    for (size_t i = 1; i < n; i++) { float d = env[i] - env[i - 1]; on[i] = d > 0 ? d : 0; }
    /* remove the local average so sustained loud passages don't dominate */
    const int W = 32;
    double run = 0;
    float *tmp = env;  /* reuse */
    for (size_t i = 0; i < n; i++) {
        run += on[i];
        if (i >= (size_t)W) run -= on[i - W];
        double avg = run / (i + 1 < (size_t)W ? i + 1 : (size_t)W);
        tmp[i] = on[i] - (float)avg > 0 ? on[i] - (float)avg : 0;
    }
    for (size_t i = 0; i < n; i++) on[i] = tmp[i];

    const double fps = (double)t->rate / hop;   /* envelope frames per second */
    double best_bpm = 0, best_s = -1, best_ph = 0, ph;
    for (double b = lo; b <= hi; b += 0.5) {
        double s = score(on, n, 60.0 * fps / b, &ph) * (weighted ? tempo_weight(b) : 1.0);
        if (s > best_s) { best_s = s; best_bpm = b; best_ph = ph; }
    }
    double coarse = best_bpm;
    best_s = -1;
    for (double b = coarse - 0.6; b <= coarse + 0.6; b += 0.01) {
        double s = score(on, n, 60.0 * fps / b, &ph);
        if (s > best_s) { best_s = s; best_bpm = b; best_ph = ph; }
    }
    free(env);
    free(on);
    /* an onset at envelope frame i means energy rose between hops i-1 and i */
    double fb = (best_ph - 0.5) * hop;
    *first_beat = fb > 0 ? fb : 0;
    return round(best_bpm * 100.0) / 100.0;
}

double bpm_detect(const track_t *t, double *first_beat) {
    return detect(t, 70, 180, 1, first_beat);
}

double bpm_detect_range(const track_t *t, double lo, double hi, double *first_beat) {
    return detect(t, lo, hi, 0, first_beat);
}
