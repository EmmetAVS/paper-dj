#include "wav.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }

void track_free(track_t *t) {
    free(t->data);
    t->data = NULL;
    t->frames = 0;
}

int wav_load(const char *path, track_t *t, char *err, size_t errlen) {
    memset(t, 0, sizeof *t);
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errlen, "cannot open %s", path); return -1; }

    unsigned char hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        snprintf(err, errlen, "%s is not a RIFF/WAVE file", path);
        fclose(f);
        return -1;
    }

    int fmt_tag = 0, channels = 0, rate = 0, bits = 0, have_fmt = 0;
    unsigned char ch[8];
    while (fread(ch, 1, 8, f) == 8) {
        uint32_t size = rd32(ch + 4);
        if (!memcmp(ch, "fmt ", 4)) {
            unsigned char fmt[40] = {0};
            size_t n = size < sizeof fmt ? size : sizeof fmt;
            if (fread(fmt, 1, n, f) != n) break;
            if (size > n) fseek(f, (long)(size - n), SEEK_CUR);
            fmt_tag  = rd16(fmt);
            channels = rd16(fmt + 2);
            rate     = (int)rd32(fmt + 4);
            bits     = rd16(fmt + 14);
            if (fmt_tag == 0xFFFE && size >= 26) fmt_tag = rd16(fmt + 24); /* WAVE_FORMAT_EXTENSIBLE */
            have_fmt = 1;
        } else if (!memcmp(ch, "data", 4)) {
            if (!have_fmt) break;
            int ok = (fmt_tag == 1 && (bits == 16 || bits == 24)) || (fmt_tag == 3 && bits == 32);
            if (!ok || channels < 1 || channels > 2 || rate <= 0) {
                snprintf(err, errlen, "%s: unsupported format (tag %d, %d-bit, %d ch). Use 16-bit PCM WAV.",
                         path, fmt_tag, bits, channels);
                fclose(f);
                return -1;
            }
            int bps = bits / 8;
            size_t frames = size / (size_t)(bps * channels);
            unsigned char *raw = malloc(size);
            t->data = malloc(frames * 2 * sizeof(float));
            if (!raw || !t->data) { free(raw); track_free(t); snprintf(err, errlen, "out of memory"); fclose(f); return -1; }
            size_t got = fread(raw, 1, size, f);
            frames = got / (size_t)(bps * channels);
            for (size_t i = 0; i < frames; i++) {
                float s[2];
                for (int c = 0; c < channels; c++) {
                    const unsigned char *p = raw + (i * channels + c) * bps;
                    if (fmt_tag == 3) { float v; memcpy(&v, p, 4); s[c] = v; }
                    else if (bits == 16) s[c] = (int16_t)rd16(p) / 32768.0f;
                    else { int32_t v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8; s[c] = v / 8388608.0f; }
                }
                t->data[2 * i]     = s[0];
                t->data[2 * i + 1] = channels == 2 ? s[1] : s[0];
            }
            free(raw);
            t->frames = frames;
            t->rate = rate;
            fclose(f);
            return 0;
        } else {
            fseek(f, (long)(size + (size & 1)), SEEK_CUR);  /* chunks are word-aligned */
        }
    }
    snprintf(err, errlen, "%s: no audio data found", path);
    fclose(f);
    return -1;
}
