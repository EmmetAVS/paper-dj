/* Biquad filters (RBJ Audio EQ Cookbook) for the 3-band DJ EQ. */
#ifndef PDJ_DSP_H
#define PDJ_DSP_H

typedef struct {
    float b0, b1, b2, a1, a2;   /* normalized coefficients */
    float z1[2], z2[2];         /* per-channel state (transposed direct form II) */
} biquad_t;

void bq_reset(biquad_t *q);
void bq_lowshelf (biquad_t *q, float fs, float f0, float gain_db);
void bq_peak     (biquad_t *q, float fs, float f0, float Q, float gain_db);
void bq_highshelf(biquad_t *q, float fs, float f0, float gain_db);
void bq_lowpass  (biquad_t *q, float fs, float f0, float Q);
void bq_highpass (biquad_t *q, float fs, float f0, float Q);

static inline float bq_run(biquad_t *q, int ch, float x) {
    float y = q->b0 * x + q->z1[ch];
    q->z1[ch] = q->b1 * x - q->a1 * y + q->z2[ch];
    q->z2[ch] = q->b2 * x - q->a2 * y;
    return y;
}

#endif
