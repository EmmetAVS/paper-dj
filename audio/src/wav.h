#ifndef PDJ_WAV_H
#define PDJ_WAV_H
#include <stddef.h>

/* A track held fully in memory as interleaved stereo float in [-1, 1]. */
typedef struct {
    float  *data;
    size_t  frames;
    int     rate;     /* the file's own sample rate; the deck resamples */
} track_t;

/* Loads a PCM WAV (16/24-bit int or 32-bit float, mono or stereo).
 * Returns 0 on success, -1 on failure with a message in err. */
int  wav_load(const char *path, track_t *t, char *err, size_t errlen);
void track_free(track_t *t);

#endif
