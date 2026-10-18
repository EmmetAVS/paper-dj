/* Song library and background loader. See library.h for the data flow. */
#include "library.h"
#include "bpm.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LIB_MAX      512
#define FILE_MAX     128
#define UPLOAD_MAX   (400u * 1024 * 1024)   /* ~36 min of 48 kHz stereo 16-bit */

typedef struct {
    char   file[FILE_MAX];
    double bpm;         /* 0 = not analyzed yet */
    double grid_s;      /* first beat, seconds */
    double len_s;
    double hint;        /* user's "about this BPM", 0 = none */
} entry_t;

struct library {
    pthread_mutex_t lock;
    pthread_cond_t  wake;
    char     dir[256];
    entry_t  e[LIB_MAX];
    int      n;
    int      scan;                    /* look for songs that still need analysis */
    char     want[NDECKS][FILE_MAX];  /* pending load request per deck ("" = none) */
    int      inflight[NDECKS];        /* a song is on its way to the render thread */
    int      state[NDECKS];           /* 0 idle, 1 loading, 2 error */
    char     msg[NDECKS][160];
    time_t   err_at[NDECKS];          /* errors fade from the page after a few seconds */
    char     loaded[NDECKS][FILE_MAX];/* song currently on each deck */
};

/* ---- helpers -------------------------------------------------------------- */

static int ends_wav(const char *f) {
    size_t n = strlen(f);
    return n > 4 && strcasecmp(f + n - 4, ".wav") == 0;
}

/* A plain file name inside the library: no paths, no hidden files, .wav. */
static int valid_name(const char *f) {
    size_t n = strlen(f);
    if (n == 0 || n >= FILE_MAX || f[0] == '.' || strchr(f, '/') || strchr(f, '\\')) return 0;
    for (size_t i = 0; i < n; i++) if ((unsigned char)f[i] < 32 || f[i] == ';') return 0;
    return ends_wav(f);
}

static int lib_path(const library_t *l, const char *file, char *out, size_t n) {
    return snprintf(out, n, "%s/%s", l->dir, file) < (int)n ? 0 : -1;
}

static int is_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void title_of(const char *file, char *out, size_t n) {
    snprintf(out, n, "%s", file);
    size_t k = strlen(out);
    if (k > 4 && strcasecmp(out + k - 4, ".wav") == 0) out[k - 4] = 0;
}

/* lock held */
static entry_t *find(library_t *l, const char *file, int create) {
    for (int i = 0; i < l->n; i++) if (strcmp(l->e[i].file, file) == 0) return &l->e[i];
    if (!create || l->n == LIB_MAX) return NULL;
    entry_t *en = &l->e[l->n++];
    memset(en, 0, sizeof *en);
    snprintf(en->file, sizeof en->file, "%s", file);
    return en;
}

/* lock held. Write to a temp file, then rename, so a crash never leaves half a file. */
static void save_cache(library_t *l) {
    char path[300], tmp[310];
    snprintf(path, sizeof path, "%s/.library.tsv", l->dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "# file\tbpm\tfirst_beat_s\tlength_s\tbpm_hint\n");
    for (int i = 0; i < l->n; i++)
        fprintf(f, "%s\t%.2f\t%.4f\t%.2f\t%.1f\n", l->e[i].file, l->e[i].bpm, l->e[i].grid_s,
                l->e[i].len_s, l->e[i].hint);
    fclose(f);
    rename(tmp, path);
}

static void load_cache(library_t *l) {
    char path[300], line[512];
    snprintf(path, sizeof path, "%s/.library.tsv", l->dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        entry_t *en = find(l, line, 1);
        if (en) sscanf(tab + 1, "%lf\t%lf\t%lf\t%lf", &en->bpm, &en->grid_s, &en->len_s, &en->hint);
    }
    fclose(f);
}

/* lock held */
static void save_last(library_t *l) {
    char path[300];
    snprintf(path, sizeof path, "%s/.last", l->dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int d = 0; d < NDECKS; d++) fprintf(f, "%c\t%s\n", 'a' + d, l->loaded[d]);
    fclose(f);
}

static void read_last(library_t *l) {
    char path[300], line[300];
    snprintf(path, sizeof path, "%s/.last", l->dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if ((line[0] == 'a' || line[0] == 'b') && line[1] == '\t' && valid_name(line + 2))
            snprintf(l->loaded[line[0] - 'a'], FILE_MAX, "%.127s", line + 2);
    }
    fclose(f);
}

/* Reads a WAV and finds its tempo and first beat, using the cache when it can.
 * hint > 0 overrides the stored hint (and is stored). No lock held on entry. */
static int prepare(engine_t *e, const char *path, const char *file, double hint,
                   loaded_track_t *out, char *err, size_t errlen) {
    library_t *l = e->lib;
    memset(out, 0, sizeof *out);
    if (wav_load(path, &out->track, err, errlen) != 0) return -1;
    snprintf(out->file, sizeof out->file, "%s", file);
    title_of(file, out->name, sizeof out->name);
    double rate = out->track.rate;

    double cached_bpm = 0, cached_grid = 0, use_hint = hint;
    pthread_mutex_lock(&l->lock);
    entry_t *en = find(l, file, 0);
    if (en) {
        if (use_hint <= 0) use_hint = en->hint;
        /* bpm < 0 = analyzed, no tempo found (e.g. a short sound) */
        if (en->bpm != 0 && (hint <= 0 || hint == en->hint)) { cached_bpm = en->bpm; cached_grid = en->grid_s; }
    }
    pthread_mutex_unlock(&l->lock);

    if (cached_bpm != 0) {
        out->bpm = cached_bpm > 0 ? cached_bpm : 0;      /* 0 = the deck assumes 120 */
        out->grid = cached_bpm > 0 ? cached_grid * rate : 0;
    } else {
        double fb = 0, bpm;
        if (use_hint > 0) {
            bpm = bpm_detect_range(&out->track, use_hint - 2.0, use_hint + 2.0, &fb);
            if (bpm <= 0) { bpm = use_hint; fb = 0; }
        } else {
            bpm = bpm_detect(&out->track, &fb);
        }
        out->bpm = bpm;
        out->grid = fb;
        pthread_mutex_lock(&l->lock);
        en = find(l, file, 1);
        if (en) {
            en->bpm = bpm > 0 ? bpm : -1;
            en->grid_s = rate ? fb / rate : 0;
            en->len_s = rate ? (double)out->track.frames / rate : 0;
            if (hint > 0) en->hint = hint;
            save_cache(l);
        }
        pthread_mutex_unlock(&l->lock);
        fprintf(stderr, "[library] %s: %.2f BPM%s, first beat at %.3f s\n", file, bpm,
                use_hint > 0 ? " (from hint)" : "", rate ? fb / rate : 0.0);
    }
    return 0;
}

/* ---- startup -------------------------------------------------------------- */

int library_init(engine_t *e, const char *dir) {
    library_t *l = calloc(1, sizeof *l);
    if (!l) return -1;
    pthread_mutex_init(&l->lock, NULL);   /* priority inheritance by default on QNX */
    pthread_cond_init(&l->wake, NULL);
    snprintf(l->dir, sizeof l->dir, "%s", dir);
    mkdir(dir, 0755);
    load_cache(l);
    read_last(l);
    l->scan = 1;
    e->lib = l;
    fprintf(stderr, "[library] songs folder: %s (%d analyzed)\n", dir, l->n);
    return 0;
}

const char *library_remembered(engine_t *e, int deck) {
    library_t *l = e->lib;
    char path[400];
    if (!l || !l->loaded[deck][0] || lib_path(l, l->loaded[deck], path, sizeof path) || !is_file(path)) return NULL;
    return l->loaded[deck];
}

int library_startup_load(engine_t *e, int deck, const char *path, double hint) {
    library_t *l = e->lib;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char file[FILE_MAX];
    snprintf(file, sizeof file, "%s", base);

    loaded_track_t lt;
    char err[256];
    if (prepare(e, path, file, hint, &lt, err, sizeof err) != 0) {
        fprintf(stderr, "[load] %s\n", err);
        return -1;
    }
    mixer_install_track(&e->mixer, deck, &lt.track, lt.name, lt.bpm, lt.grid);
    track_free(&lt.track);   /* the deck's previous (empty) track */
    fprintf(stderr, "[deck %c] %s: %.2f BPM, first beat at %.3f s\n", 'A' + deck, lt.name, lt.bpm,
            e->mixer.deck[deck].track.rate ? lt.grid / e->mixer.deck[deck].track.rate : 0.0);
    char inlib[400];
    pthread_mutex_lock(&l->lock);
    if (lib_path(l, file, inlib, sizeof inlib) == 0 && is_file(inlib)) {
        snprintf(l->loaded[deck], FILE_MAX, "%s", file);
        save_last(l);
    }
    pthread_mutex_unlock(&l->lock);
    return 0;
}

/* ---- requests (any thread) ------------------------------------------------ */

static int deck_busy_now(engine_t *e, int deck) {
    mixer_snapshot_t s;
    engine_read_snapshot(e, &s);
    return s.playing[deck] || s.scratching[deck];
}

int library_request_load(engine_t *e, int deck, const char *file) {
    library_t *l = e->lib;
    char path[400];
    if (!l || deck < 0 || deck >= NDECKS) return -1;
    if (!valid_name(file) || lib_path(l, file, path, sizeof path) || !is_file(path)) {
        fprintf(stderr, "[library] no such song: '%s'\n", file);
        return -1;
    }
    pthread_mutex_lock(&l->lock);
    if (deck_busy_now(e, deck)) {
        l->state[deck] = 2;
        l->err_at[deck] = time(NULL);
        snprintf(l->msg[deck], sizeof l->msg[deck], "Deck %c is playing. Pause it, then load.", 'A' + deck);
        pthread_mutex_unlock(&l->lock);
        return -1;
    }
    snprintf(l->want[deck], FILE_MAX, "%s", file);
    l->state[deck] = 1;
    snprintf(l->msg[deck], sizeof l->msg[deck], "Loading %s", file);
    pthread_cond_signal(&l->wake);
    pthread_mutex_unlock(&l->lock);
    return 0;
}

int library_set_hint(engine_t *e, const char *file, double bpm) {
    library_t *l = e->lib;
    if (!l || !valid_name(file) || bpm < 0 || bpm > 300) return -1;
    pthread_mutex_lock(&l->lock);
    entry_t *en = find(l, file, 1);
    if (!en) { pthread_mutex_unlock(&l->lock); return -1; }
    en->hint = bpm;
    en->bpm = 0;                        /* re-analyze with the new hint */
    save_cache(l);
    l->scan = 1;
    int reload = -1;
    for (int d = 0; d < NDECKS; d++)
        if (strcmp(l->loaded[d], file) == 0 && !l->want[d][0]) reload = d;
    pthread_cond_signal(&l->wake);
    pthread_mutex_unlock(&l->lock);
    /* the song is on a stopped deck: load it again so the deck gets the new beat grid */
    if (reload >= 0 && !deck_busy_now(e, reload)) library_request_load(e, reload, file);
    return 0;
}

static int cmp_name(const void *a, const void *b) {
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

/* Lists the folder; caller frees *out and each name. */
static int list_dir(const char *dir, char ***out) {
    DIR *d = opendir(dir);
    *out = NULL;
    if (!d) return 0;
    int n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!valid_name(de->d_name)) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            char **g = realloc(*out, (size_t)cap * sizeof **out);
            if (!g) break;
            *out = g;
        }
        (*out)[n++] = strdup(de->d_name);
    }
    closedir(d);
    if (n) qsort(*out, (size_t)n, sizeof **out, cmp_name);
    return n;
}

static size_t json_str(char *b, size_t cap, const char *s) {
    size_t n = 0;
    if (n < cap) b[n++] = '"';
    for (; *s && n + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { b[n++] = '\\'; b[n++] = (char)c; }
        else if (c < 32) n += (size_t)snprintf(b + n, cap - n, "\\u%04x", c);
        else b[n++] = (char)c;
    }
    if (n < cap) b[n++] = '"';
    return n;
}

int library_json(engine_t *e, char *b, size_t cap) {
    library_t *l = e->lib;
    if (!l) return snprintf(b, cap, "{\"songs\":[],\"decks\":[],\"enabled\":0}");
    char **names;
    int count = list_dir(l->dir, &names);
    size_t n = (size_t)snprintf(b, cap, "{\"enabled\":1,\"songs\":[");
    pthread_mutex_lock(&l->lock);
    for (int i = 0; i < count && n + 400 < cap; i++) {
        entry_t *en = find(l, names[i], 0);
        n += (size_t)snprintf(b + n, cap - n, "%s{\"file\":", i ? "," : "");
        n += json_str(b + n, cap - n, names[i]);
        n += (size_t)snprintf(b + n, cap - n, ",\"bpm\":%.2f,\"len\":%.1f,\"hint\":%.1f}",
                              en ? en->bpm : 0, en ? en->len_s : 0, en ? en->hint : 0);
    }
    n += (size_t)snprintf(b + n, cap - n, "],\"decks\":[");
    static const char *st[] = { "idle", "loading", "error" };
    time_t now = time(NULL);
    for (int d = 0; d < NDECKS; d++)
        if (l->state[d] == 2 && now - l->err_at[d] >= 6) { l->state[d] = 0; l->msg[d][0] = 0; }
    for (int d = 0; d < NDECKS && n + 400 < cap; d++) {
        n += (size_t)snprintf(b + n, cap - n, "%s{\"file\":", d ? "," : "");
        n += json_str(b + n, cap - n, l->loaded[d]);
        n += (size_t)snprintf(b + n, cap - n, ",\"state\":\"%s\",\"msg\":", st[l->state[d]]);
        n += json_str(b + n, cap - n, l->msg[d]);
        n += (size_t)snprintf(b + n, cap - n, "}");
    }
    pthread_mutex_unlock(&l->lock);
    n += (size_t)snprintf(b + n, cap - n, "]}");
    for (int i = 0; i < count; i++) free(names[i]);
    free(names);
    return n < cap ? (int)n : (int)cap - 1;
}

/* Keeps letters, digits and a few safe symbols; everything else becomes '_'. Always .wav. */
static void sanitize(const char *in, char *out, size_t n) {
    char tmp[FILE_MAX];
    size_t k = 0;
    for (; *in && k < 100; in++) {
        unsigned char c = (unsigned char)*in;
        tmp[k++] = (isalnum(c) || strchr(" ._-()&,'+!", c) || c >= 0x80) ? (char)c : '_';
    }
    tmp[k] = 0;
    char *dot = strrchr(tmp, '.');
    if (dot) *dot = 0;                                  /* drop any extension */
    char *s = tmp;
    while (*s == '.' || *s == ' ') s++;                 /* no hidden files */
    if (!*s) s = "song";
    snprintf(out, n, "%.100s.wav", s);
}

int library_save_upload(engine_t *e, const char *name, int fd, const char *pre, size_t prelen,
                        size_t total, char *out, size_t outlen) {
    library_t *l = e->lib;
    if (!l) { snprintf(out, outlen, "library disabled (start the engine with -L)"); return -1; }
    if (total < 44 || total > UPLOAD_MAX) { snprintf(out, outlen, "bad size"); return -1; }
    char file[FILE_MAX], path[400], tmp[420];
    sanitize(name, file, sizeof file);
    lib_path(l, file, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s/.upload-%d.tmp", l->dir, (int)getpid() ^ (int)time(NULL) ^ fd);

    int w = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (w < 0) { snprintf(out, outlen, "cannot write: %s", strerror(errno)); return -1; }
    size_t got = 0;
    char head[12] = {0};
    if (prelen > total) prelen = total;
    if (prelen) {
        memcpy(head, pre, prelen < 12 ? prelen : 12);
        if (write(w, pre, prelen) != (ssize_t)prelen) goto fail_io;
        got = prelen;
    }
    char *buf = malloc(64 * 1024);
    if (!buf) goto fail_io;
    while (got < total) {
        size_t want = total - got < 64 * 1024 ? total - got : 64 * 1024;
        ssize_t r = recv(fd, buf, want, 0);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; free(buf); goto fail_short; }
        if (got < 12) memcpy(head + got, buf, (size_t)r < 12 - got ? (size_t)r : 12 - got);
        if (write(w, buf, (size_t)r) != r) { free(buf); goto fail_io; }
        got += (size_t)r;
    }
    free(buf);
    close(w);
    if (memcmp(head, "RIFF", 4) != 0 || memcmp(head + 8, "WAVE", 4) != 0) {
        unlink(tmp);
        snprintf(out, outlen, "not a WAV file");
        return -1;
    }
    if (rename(tmp, path) != 0) { unlink(tmp); snprintf(out, outlen, "rename failed: %s", strerror(errno)); return -1; }

    pthread_mutex_lock(&l->lock);
    entry_t *en = find(l, file, 1);
    if (en) { double h = en->hint; memset(en, 0, sizeof *en); snprintf(en->file, sizeof en->file, "%s", file); en->hint = h; }
    save_cache(l);
    l->scan = 1;
    pthread_cond_signal(&l->wake);
    pthread_mutex_unlock(&l->lock);
    fprintf(stderr, "[library] added %s (%.1f MB)\n", file, total / 1048576.0);
    snprintf(out, outlen, "%s", file);
    return 0;

fail_io:
    snprintf(out, outlen, "write failed: %s", strerror(errno));
    close(w); unlink(tmp);
    return -1;
fail_short:
    snprintf(out, outlen, "upload interrupted");
    close(w); unlink(tmp);
    return -1;
}

/* ---- render thread -------------------------------------------------------- */

void library_poll_render(engine_t *e) {
    for (int d = 0; d < NDECKS; d++) {
        if (!atomic_load_explicit(&e->incoming[d], memory_order_relaxed)) continue;
        if (atomic_load_explicit(&e->outgoing[d], memory_order_acquire)) continue;  /* loader not done with the last one */
        loaded_track_t *lt = atomic_exchange_explicit(&e->incoming[d], NULL, memory_order_acq_rel);
        if (!lt) continue;
        lt->swapped = !mixer_deck_busy(&e->mixer, d);
        if (lt->swapped) mixer_install_track(&e->mixer, d, &lt->track, lt->name, lt->bpm, lt->grid);
        atomic_store_explicit(&e->outgoing[d], lt, memory_order_release);
    }
}

/* ---- loader thread -------------------------------------------------------- */

/* lock held. Picks one song in the folder that has never been analyzed. */
static int next_unanalyzed(library_t *l, char *file) {
    char **names;
    int count = list_dir(l->dir, &names), found = 0;
    for (int i = 0; i < count; i++) {
        entry_t *en = find(l, names[i], 0);
        if (!found && (!en || en->bpm == 0)) { snprintf(file, FILE_MAX, "%s", names[i]); found = 1; }
        free(names[i]);
    }
    free(names);
    return found;
}

void *library_thread(void *arg) {
    engine_t *e = arg;
    library_t *l = e->lib;
    char file[FILE_MAX], path[400], err[256];

    pthread_mutex_lock(&l->lock);
    while (atomic_load(&e->running)) {
        int busy = 0;

        /* 1. songs coming back from the render thread */
        for (int d = 0; d < NDECKS; d++) {
            loaded_track_t *lt = atomic_exchange_explicit(&e->outgoing[d], NULL, memory_order_acq_rel);
            if (!lt) continue;
            l->inflight[d] = 0;
            if (lt->swapped) {
                snprintf(l->loaded[d], FILE_MAX, "%s", lt->file);
                save_last(l);
                l->state[d] = 0;
                snprintf(l->msg[d], sizeof l->msg[d], "%s", "");
                fprintf(stderr, "[library] deck %c: %s\n", 'A' + d, lt->name);
            } else {
                l->state[d] = 2;
                l->err_at[d] = time(NULL);
                snprintf(l->msg[d], sizeof l->msg[d], "Deck %c started playing before %s finished loading.",
                         'A' + d, lt->name);
            }
            pthread_mutex_unlock(&l->lock);
            track_free(&lt->track);           /* old song, or the rejected new one */
            free(lt);
            pthread_mutex_lock(&l->lock);
            busy = 1;
        }

        /* 2. one load request */
        for (int d = 0; d < NDECKS && !busy; d++) {
            if (!l->want[d][0] || l->inflight[d]) continue;
            snprintf(file, sizeof file, "%s", l->want[d]);
            l->want[d][0] = 0;
            l->inflight[d] = 1;
            pthread_mutex_unlock(&l->lock);

            loaded_track_t *lt = malloc(sizeof *lt);
            int ok = lt && lib_path(l, file, path, sizeof path) == 0 &&
                     prepare(e, path, file, 0, lt, err, sizeof err) == 0;
            if (ok) atomic_store_explicit(&e->incoming[d], lt, memory_order_release);

            pthread_mutex_lock(&l->lock);
            if (!ok) {
                free(lt);
                l->inflight[d] = 0;
                l->state[d] = 2;
                l->err_at[d] = time(NULL);
                snprintf(l->msg[d], sizeof l->msg[d], "Couldn't load %s", file);
                fprintf(stderr, "[library] %s: %s\n", file, lt ? err : "out of memory");
            }
            busy = 1;
        }

        /* 3. analyze songs nobody has loaded yet, so the list shows their BPM */
        if (!busy && l->scan) {
            if (next_unanalyzed(l, file)) {
                pthread_mutex_unlock(&l->lock);
                loaded_track_t tmp;
                if (lib_path(l, file, path, sizeof path) == 0 && prepare(e, path, file, 0, &tmp, err, sizeof err) == 0)
                    track_free(&tmp.track);
                else {
                    fprintf(stderr, "[library] %s: %s\n", file, err);
                    pthread_mutex_lock(&l->lock);
                    entry_t *en = find(l, file, 1);
                    if (en) en->bpm = -1;      /* unreadable: don't retry forever */
                    pthread_mutex_unlock(&l->lock);
                }
                pthread_mutex_lock(&l->lock);
                busy = 1;
            } else {
                l->scan = 0;
            }
        }

        if (!busy) {
            /* nothing to do: sleep until a request, or briefly while a swap is pending */
            int waiting = 0;
            for (int d = 0; d < NDECKS; d++) waiting |= l->inflight[d];
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            long add = waiting ? 10000000L : 250000000L;
            ts.tv_nsec += add;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&l->wake, &l->lock, &ts);
        }
    }
    pthread_mutex_unlock(&l->lock);
    return NULL;
}
