/* Song library: the WAVs in one folder (e.g. songs/), loaded onto decks while
 * the music keeps playing.
 *
 *   request (a.load <file>, any thread)
 *     -> loader thread (normal priority): read the WAV, find BPM + beat grid
 *     -> e->incoming[deck]  (atomic pointer)
 *     -> render thread swaps it in between two blocks, if the deck is stopped
 *     -> e->outgoing[deck]  (the old track, or the rejected new one)
 *     -> loader thread frees it
 * The render thread only exchanges two pointers per deck: it never waits,
 * allocates, frees or touches the library's lock.
 *
 * Files in the folder:
 *   *.wav          the songs (48 kHz stereo is fastest; other rates are resampled on play)
 *   .library.tsv   cached BPM, first beat, length, and any BPM hint the user typed
 *   .last          which song was last on each deck, loaded again at startup
 */
#ifndef PDJ_LIBRARY_H
#define PDJ_LIBRARY_H

#include "engine.h"

struct loaded_track {
    track_t track;
    char    file[128];      /* file name inside the library folder */
    char    name[64];       /* shown on the deck (file name without .wav) */
    double  bpm, grid;      /* grid = first beat, in source frames */
    int     swapped;        /* set by the render thread: 1 = now on the deck, 0 = rejected */
};

/* Startup, before the render thread runs. */
int         library_init(engine_t *e, const char *dir);
const char *library_remembered(engine_t *e, int deck);      /* file from .last, if it still exists */
/* Loads path onto the deck right now (startup only). hint > 0 = "about this BPM". */
int         library_startup_load(engine_t *e, int deck, const char *path, double hint);

/* Any thread. 0 = queued. Refused while the deck is playing. */
int  library_request_load(engine_t *e, int deck, const char *file);
/* Sets (bpm > 0) or clears (bpm = 0) the BPM hint for a song and re-analyzes it. */
int  library_set_hint(engine_t *e, const char *file, double bpm);
/* JSON: {"songs":[...],"decks":[...]}. Returns the length written. */
int  library_json(engine_t *e, char *buf, size_t cap);
/* Saves an uploaded WAV. pre/prelen = body bytes already read with the headers.
 * Writes the final file name to out. 0 = ok, else -1 with a message in out. */
int  library_save_upload(engine_t *e, const char *name, int fd, const char *pre, size_t prelen,
                         size_t total, char *out, size_t outlen);

/* Render thread, once per block. */
void library_poll_render(engine_t *e);
/* The loader thread. */
void *library_thread(void *arg);

#endif
