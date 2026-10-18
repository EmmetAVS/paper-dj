#include "dsp.h"
#include "common.h"
#include <math.h>
#include <string.h>

void bq_reset(biquad_t *q) {
    memset(q, 0, sizeof *q);
    q->b0 = 1.0f;
}

static void set(biquad_t *q, double b0, double b1, double b2, double a0, double a1, double a2) {
    q->b0 = (float)(b0 / a0);
    q->b1 = (float)(b1 / a0);
    q->b2 = (float)(b2 / a0);
    q->a1 = (float)(a1 / a0);
    q->a2 = (float)(a2 / a0);
}

void bq_peak(biquad_t *q, float fs, float f0, float Q, float gain_db) {
    double A = pow(10.0, gain_db / 40.0), w0 = 2 * PDJ_PI * f0 / fs;
    double c = cos(w0), alpha = sin(w0) / (2 * Q);
    set(q, 1 + alpha * A, -2 * c, 1 - alpha * A, 1 + alpha / A, -2 * c, 1 - alpha / A);
}

void bq_lowshelf(biquad_t *q, float fs, float f0, float gain_db) {
    double A = pow(10.0, gain_db / 40.0), w0 = 2 * PDJ_PI * f0 / fs;
    double c = cos(w0), alpha = sin(w0) / 2 * sqrt(2.0), sa = 2 * sqrt(A) * alpha;
    set(q, A * ((A + 1) - (A - 1) * c + sa),
           2 * A * ((A - 1) - (A + 1) * c),
           A * ((A + 1) - (A - 1) * c - sa),
           (A + 1) + (A - 1) * c + sa,
           -2 * ((A - 1) + (A + 1) * c),
           (A + 1) + (A - 1) * c - sa);
}

void bq_highshelf(biquad_t *q, float fs, float f0, float gain_db) {
    double A = pow(10.0, gain_db / 40.0), w0 = 2 * PDJ_PI * f0 / fs;
    double c = cos(w0), alpha = sin(w0) / 2 * sqrt(2.0), sa = 2 * sqrt(A) * alpha;
    set(q, A * ((A + 1) + (A - 1) * c + sa),
           -2 * A * ((A - 1) + (A + 1) * c),
           A * ((A + 1) + (A - 1) * c - sa),
           (A + 1) - (A - 1) * c + sa,
           2 * ((A - 1) - (A + 1) * c),
           (A + 1) - (A - 1) * c - sa);
}

void bq_lowpass(biquad_t *q, float fs, float f0, float Q) {
    double w0 = 2 * PDJ_PI * f0 / fs, c = cos(w0), alpha = sin(w0) / (2 * Q);
    set(q, (1 - c) / 2, 1 - c, (1 - c) / 2, 1 + alpha, -2 * c, 1 - alpha);
}

void bq_highpass(biquad_t *q, float fs, float f0, float Q) {
    double w0 = 2 * PDJ_PI * f0 / fs, c = cos(w0), alpha = sin(w0) / (2 * Q);
    set(q, (1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + alpha, -2 * c, 1 - alpha);
}
