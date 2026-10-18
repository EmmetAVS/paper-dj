/* Offline tempo + beat-grid detection, run once when a track is loaded. */
#ifndef PDJ_BPM_H
#define PDJ_BPM_H
#include "wav.h"

/* Returns the detected BPM (70-180) and writes the position of the first
 * beat, in source frames, to *first_beat. Returns 0 if detection failed. */
double bpm_detect(const track_t *t, double *first_beat);

/* Same, but only searches lo..hi BPM (no preference toward 120). Use it when
 * you know roughly how fast the song is: it locks both the exact tempo and the
 * beat positions. Returns 0 if detection failed. */
double bpm_detect_range(const track_t *t, double lo, double hi, double *first_beat);

#endif
