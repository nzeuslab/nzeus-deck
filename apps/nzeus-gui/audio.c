/* audio.c - the music engine: WAV + MP3 decoding, resampling (see audio.h).
 * MP3 decoding: libmad (GPL-2.0+, libmad/SOURCE.txt) - whole-number maths only:
 * the console's kernel emulates every floating-point instruction ("nofpu"), so
 * the per-sample work here never uses float/double either. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libmad/mad.h"
#include "audio.h"

enum { NONE, WAV, MP3 };
#define PCM_FRAMES 4096               /* decoded frames waiting to be played (stereo) */
#define MP3_FRAME_MAX 1152            /* the most frames one MP3 frame decodes to */

static struct {
    int kind, playing, finished;
    FILE *f;
    long data_start, data_end;        /* where the sound data is in the file */
    int rate, channels, bits;         /* the file's own format */
    /* MP3 (libmad: stream = the bytes, frame = one decoded frame, synth = its samples) */
    struct mad_stream stream;
    struct mad_frame frame;
    struct mad_synth synth;
    int mad_ready;                    /* the three above are set up (must be finished) */
    unsigned char in[16384 + MAD_BUFFER_GUARD];
    int in_len, in_eof;               /* bytes in in[]; the file's end (+ guard) is in it */
    int bitrate;                      /* kbit/s, from the first frame (for length + seeking) */
    /* decoded, stereo, waiting */
    int16_t pcm[PCM_FRAMES * 2];
    int pcm_count;
    uint32_t pos_fp;                  /* where we are in pcm[], 16.16 fixed point */
    uint64_t consumed_fp;             /* source frames already played, 16.16 (for the clock) */
    char path[256];                   /* the open file (audio_path) */
} A;
static unsigned revision;             /* discard pending output after a user changes playback */

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static int fail(char *err, int n, const char *why) { snprintf(err, n, "%s", why); audio_close(); return -1; }

/* ---------- opening ---------- */
static int open_wav(char *err, int n)
{
    unsigned char h[12], ck[8], fmt[16];
    if (fread(h, 1, 12, A.f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return fail(err, n, "Not a WAV file");
    int have_fmt = 0;
    while (fread(ck, 1, 8, A.f) == 8) {                      /* walk the chunks: "fmt ", then "data" */
        uint32_t size = rd32(ck + 4);
        if (!memcmp(ck, "fmt ", 4)) {
            if (size < 16 || fread(fmt, 1, 16, A.f) != 16) return fail(err, n, "The WAV file is damaged");
            fseek(A.f, (long)(size - 16 + (size & 1)), SEEK_CUR);
            if (rd16(fmt) != 1) return fail(err, n, "Only plain (PCM) WAV files are supported");
            A.channels = rd16(fmt + 2);
            A.rate = (int)rd32(fmt + 4);
            A.bits = rd16(fmt + 14);
            have_fmt = 1;
        } else if (!memcmp(ck, "data", 4)) {
            if (!have_fmt) break;
            A.data_start = ftell(A.f);
            A.data_end = A.data_start + (long)size;
            break;
        } else fseek(A.f, (long)(size + (size & 1)), SEEK_CUR);
    }
    if (!have_fmt || !A.data_start) return fail(err, n, "The WAV file is damaged");
    if ((A.bits != 8 && A.bits != 16) || A.channels < 1 || A.channels > 2 || A.rate < 4000 || A.rate > 96000)
        return fail(err, n, "This WAV format is not supported (8/16-bit, mono/stereo)");
    return 0;
}

/* (re)start libmad at the current file position */
static void mp3_restart(void)
{
    if (A.mad_ready) {
        mad_synth_finish(&A.synth);
        mad_frame_finish(&A.frame);
        mad_stream_finish(&A.stream);
    }
    mad_stream_init(&A.stream);
    mad_frame_init(&A.frame);
    mad_synth_init(&A.synth);
    A.mad_ready = 1;
    A.in_len = 0;
    A.in_eof = 0;
}

/* keep the bytes libmad has not used yet, top up in[] from the file and hand
 * it to libmad again; 0 = nothing more to decode */
static int mp3_fill_input(void)
{
    if (A.stream.next_frame) {                             /* drop what was decoded */
        int used = (int)(A.stream.next_frame - A.in);
        memmove(A.in, A.in + used, (size_t)(A.in_len - used));
        A.in_len -= used;
    }
    if (A.in_eof) return 0;                                /* (the end was already handed over) */
    int room = (int)sizeof A.in - MAD_BUFFER_GUARD - A.in_len;
    long left = A.data_end - ftell(A.f);
    if (room > left) room = left > 0 ? (int)left : 0;
    A.in_len += (int)fread(A.in + A.in_len, 1, (size_t)room, A.f);
    if (ftell(A.f) >= A.data_end || feof(A.f)) {           /* the end: libmad needs a few zero bytes after it */
        memset(A.in + A.in_len, 0, MAD_BUFFER_GUARD);
        A.in_len += MAD_BUFFER_GUARD;
        A.in_eof = 1;
    }
    if (A.in_len <= 0) return 0;
    mad_stream_buffer(&A.stream, A.in, (unsigned long)A.in_len);
    return A.in_len;
}

/* the next good MP3 frame into A.frame (header_only = 1: just its header); 0 = the end */
static int mp3_next_frame(int header_only)
{
    for (int tries = 0; tries < 64; tries++) {
        if (!A.stream.buffer || A.stream.error == MAD_ERROR_BUFLEN) {
            if (mp3_fill_input() == 0) return 0;
            A.stream.error = MAD_ERROR_NONE;
        }
        int r = header_only ? mad_header_decode(&A.frame.header, &A.stream) : mad_frame_decode(&A.frame, &A.stream);
        if (r == 0) return 1;
        if (A.stream.error == MAD_ERROR_BUFLEN) continue;          /* needs more bytes */
        if (!MAD_RECOVERABLE(A.stream.error)) return 0;            /* (out of memory: give up) */
        /* a damaged frame / not in step yet: libmad skips it, try the next one */
    }
    return 0;
}

static int open_mp3(char *err, int n)
{
    unsigned char id3[10];
    A.data_start = 0;
    if (fread(id3, 1, 10, A.f) == 10 && !memcmp(id3, "ID3", 3))  /* skip an ID3v2 tag at the start */
        A.data_start = 10 + (long)(((id3[6] & 127) << 21) | ((id3[7] & 127) << 14) | ((id3[8] & 127) << 7) | (id3[9] & 127));
    fseek(A.f, 0, SEEK_END);
    A.data_end = ftell(A.f);
    fseek(A.f, A.data_start, SEEK_SET);
    mp3_restart();
    /* the first real frame's header tells the format (layer III only, like before) */
    if (mp3_next_frame(1) && A.frame.header.layer == MAD_LAYER_III && A.frame.header.samplerate) {
        A.rate = (int)A.frame.header.samplerate;
        A.channels = MAD_NCHANNELS(&A.frame.header);
        A.bitrate = (int)(A.frame.header.bitrate / 1000);
        fseek(A.f, A.data_start, SEEK_SET);                  /* start again from the top */
        mp3_restart();
        return 0;
    }
    return fail(err, n, "Not an MP3 file (or damaged)");
}

int audio_open(const char *path, char *err, int errn)
{
    audio_close();
    A.f = fopen(path, "rb");
    if (!A.f) return fail(err, errn, "Could not read the file");
    const char *dot = strrchr(path, '.');
    int is_mp3 = dot && (!strcmp(dot, ".mp3") || !strcmp(dot, ".MP3"));
    int rc = is_mp3 ? open_mp3(err, errn) : open_wav(err, errn);
    if (rc != 0) return rc;
    A.kind = is_mp3 ? MP3 : WAV;
    snprintf(A.path, sizeof A.path, "%s", path);
    return 0;
}

void audio_close(void)
{
    revision++;
    if (A.mad_ready) {                                       /* (libmad keeps a little memory of its own) */
        mad_synth_finish(&A.synth);
        mad_frame_finish(&A.frame);
        mad_stream_finish(&A.stream);
    }
    if (A.f) fclose(A.f);
    memset(&A, 0, sizeof A);
}

/* ---------- decoding into pcm[] (always stereo) ---------- */
/* libmad's sample (fixed point, 1.0 = MAD_F_ONE) -> 16 bits: round, then clip */
static int16_t mad_to_s16(mad_fixed_t s)
{
    s += 1L << (MAD_F_FRACBITS - 16);
    if (s >= MAD_F_ONE) s = MAD_F_ONE - 1;
    else if (s < -MAD_F_ONE) s = -MAD_F_ONE;
    return (int16_t)(s >> (MAD_F_FRACBITS + 1 - 16));
}

static int decode_more(int16_t *dst, int max_frames)
{
    if (A.kind == WAV) {
        long left = A.data_end - ftell(A.f);
        int bytes_per = A.channels * A.bits / 8;
        int frames = (int)(left / bytes_per);
        if (frames > max_frames) frames = max_frames;
        if (frames > 1024) frames = 1024;
        if (frames <= 0) return 0;
        unsigned char raw[1024 * 4];
        int got = (int)fread(raw, (size_t)bytes_per, (size_t)frames, A.f);
        for (int i = 0; i < got; i++) {
            int16_t l, r;
            if (A.bits == 16) { l = (int16_t)rd16(raw + i * bytes_per); r = A.channels == 2 ? (int16_t)rd16(raw + i * bytes_per + 2) : l; }
            else { l = (int16_t)((raw[i * bytes_per] - 128) << 8); r = A.channels == 2 ? (int16_t)((raw[i * bytes_per + 1] - 128) << 8) : l; }
            dst[i * 2] = l;
            dst[i * 2 + 1] = r;
        }
        return got;
    }
    if (A.kind == MP3) {
        if (!mp3_next_frame(0)) return 0;                  /* the end (or nothing usable left) */
        mad_synth_frame(&A.synth, &A.frame);
        const struct mad_pcm *pcm = &A.synth.pcm;
        int samples = pcm->length;
        if (samples > max_frames) samples = max_frames;
        const mad_fixed_t *l = pcm->samples[0], *r = pcm->samples[pcm->channels == 2 ? 1 : 0];
        for (int i = 0; i < samples; i++) {
            dst[i * 2] = mad_to_s16(l[i]);
            dst[i * 2 + 1] = mad_to_s16(r[i]);
        }
        return samples;
    }
    return 0;
}

/* make sure pcm[] holds at least 2 frames from the current position */
static int refill(void)
{
    int at = (int)(A.pos_fp >> 16);
    if (at > 0) {                                          /* drop what has been played */
        int keep = A.pcm_count - at;
        if (keep < 0) keep = 0;
        memmove(A.pcm, A.pcm + at * 2, (size_t)keep * 2 * sizeof(int16_t));
        A.pcm_count = keep;
        A.pos_fp &= 0xFFFF;
    }
    while (A.pcm_count < PCM_FRAMES - MP3_FRAME_MAX && A.pcm_count < 1200) {
        int got = decode_more(A.pcm + A.pcm_count * 2, PCM_FRAMES - A.pcm_count);
        if (got <= 0) break;
        A.pcm_count += got;
    }
    return A.pcm_count;
}

int audio_out_rate = AUDIO_RATE;

void audio_render(int16_t *out, int frames)
{
    memset(out, 0, (size_t)frames * 2 * sizeof(int16_t));
    if (!A.kind || !A.playing) return;
    uint32_t step = (uint32_t)(((uint64_t)A.rate << 16) / audio_out_rate);   /* source frames per output frame (1.0 = as it is) */
    for (int i = 0; i < frames; i++) {
        if ((int)(A.pos_fp >> 16) + 1 >= A.pcm_count && refill() < 2) {    /* the end of the song */
            A.playing = 0;
            A.finished = 1;
            return;
        }
        int at = (int)(A.pos_fp >> 16);
        int32_t f = (int32_t)(A.pos_fp & 0xFFFF);
        const int16_t *a = A.pcm + at * 2, *b = a + 2;         /* straight line between two samples */
        out[i * 2] = (int16_t)(a[0] + (((b[0] - a[0]) * f) >> 16));
        out[i * 2 + 1] = (int16_t)(a[1] + (((b[1] - a[1]) * f) >> 16));
        A.pos_fp += step;
        A.consumed_fp += step;                             /* (whole numbers: no float per sample) */
    }
}

/* ---------- controls ---------- */
void audio_play(int on) { if (A.kind) { if (A.playing != on) revision++; A.playing = on; if (on) A.finished = 0; } }
unsigned audio_revision(void) { return revision; }
int audio_playing(void) { return A.playing; }
int audio_loaded(void) { return A.kind != NONE; }
int audio_finished(void) { return A.finished; }
double audio_position(void) { return A.rate ? (double)A.consumed_fp / 65536.0 / A.rate : 0; }
int audio_info(int *rate, int *channels) { *rate = A.rate; *channels = A.channels; return A.kind; }
const char *audio_path(void) { return A.path; }      /* ("" after audio_close: it clears A) */

double audio_length(void)
{
    if (A.kind == WAV) return (double)(A.data_end - A.data_start) / (A.channels * A.bits / 8) / A.rate;
    if (A.kind == MP3 && A.bitrate) return (double)(A.data_end - A.data_start) * 8.0 / (A.bitrate * 1000.0);
    return 0;
}

void audio_seek(double s)
{
    if (!A.kind) return;
    revision++;
    double len = audio_length();
    if (s < 0) s = 0;
    if (len > 0 && s > len - 0.5) s = len - 0.5;
    if (s < 0) s = 0;
    long off;
    if (A.kind == WAV) {
        int bytes_per = A.channels * A.bits / 8;
        off = A.data_start + (long)(s * A.rate) * bytes_per;
    } else {                                                 /* MP3: by bytes (exact for constant bit rate) */
        off = A.data_start + (long)(s * A.bitrate * 1000.0 / 8.0);
    }
    fseek(A.f, off, SEEK_SET);
    if (A.kind == MP3) mp3_restart();                      /* (libmad finds the next frame by itself) */
    A.pcm_count = 0;
    A.pos_fp = 0;
    A.consumed_fp = (uint64_t)(s * A.rate * 65536.0);
    A.finished = 0;
}
