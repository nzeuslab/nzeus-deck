/* ALSA's write ioctl returns its frame count in x.result, not its return value.
 * Fake only the PCM device: exercise partial writes and recovery without sound. */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sound/asound.h>
#include "hw.h"

static struct { long result; int error; } replies[16];
static int reply_count, at, prepares, failures, opens, closes, drops, accept_all;
static const int16_t *wanted;
static size_t wanted_count;
static int16_t samples[16] = {1, 2, 3, 4, 5, 6, 7, 8};

int __real_open(const char *, int, ...);
int __real_close(int);
int __wrap_open(const char *path, int flags, ...)
{
    if (!strcmp(path, "fake-pcm")) { opens++; return 10001; }
    return __real_open(path, flags, 0600);
}
int __wrap_close(int fd) { if (fd == 10001) { closes++; return 0; } return __real_close(fd); }
int __wrap_ioctl(int fd, unsigned long request, ...)
{
    if (fd != 10001) { errno = ENODEV; return -1; }
    if (request == SNDRV_PCM_IOCTL_PREPARE) { prepares++; return 0; }
    if (request == SNDRV_PCM_IOCTL_DROP) { drops++; return 0; }
    if (request != SNDRV_PCM_IOCTL_WRITEI_FRAMES) return 0;
    va_list args;
    va_start(args, request);
    struct snd_xferi *x = va_arg(args, struct snd_xferi *);
    va_end(args);
    if (accept_all) { x->result = (long)x->frames; return 0; }   /* (the silence test: a card that takes everything) */
    if (wanted && (x->buf != wanted || x->frames != wanted_count)) failures++;
    if (at >= reply_count) { failures++; errno = EAGAIN; return -1; }
    int error = replies[at].error;
    long result = replies[at++].result;
    if (error) { errno = error; return -1; }
    x->result = result;
    if (result > 0 && wanted) { wanted += result * 2; wanted_count -= result; }
    return 0;
}
static void reset(void)
{
    static unsigned rate = 44100;
    hw_pcm_set_rate(rate = rate == 44100 ? 48000 : 44100);
    at = reply_count = prepares = 0;
    wanted = samples; wanted_count = 4;
}
static void reply(long result, int error)
{
    replies[reply_count].result = result;
    replies[reply_count++].error = error;
}
static void check(int ok, const char *label)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) failures++;
}
int main(void)
{
    hw_pcm_dev = "fake-pcm";
    reset(); reply(2, 0); reply(0, EAGAIN);
    check(hw_pcm_write(samples, 4) == 2 && at == 2, "partial write keeps the correct unaccepted suffix");
    reply(2, 0);
    check(hw_pcm_write(samples + 4, 2) == 2 && at == 3, "remaining samples can be retried without duplication");
    reset(); reply(0, EAGAIN);
    check(hw_pcm_write(samples, 4) == 0, "busy device consumes no samples");
    reset(); reply(0, EINTR); reply(4, 0);
    check(hw_pcm_write(samples, 4) == 4 && at == 2, "interrupted write retries the same samples");
    reset(); reply(0, EPIPE); reply(4, 0);
    check(hw_pcm_write(samples, 4) == 4 && prepares == 2, "underrun prepares the card and retries");
    reset(); reply(0, EPIPE); reply(0, EPIPE); reply(0, EPIPE);
    check(hw_pcm_write(samples, 4) == 0 && at == 3, "persistent underrun cannot spin forever");
    reset(); reply(0, 0);
    check(hw_pcm_write(samples, 4) == 0 && at == 1, "zero progress cannot spin forever");
    check(hw_pcm_write(samples, 0) == 0 && at == 1, "empty input does not touch ALSA");
    /* half a second of silence: the card is stopped and CLOSED (other programs
     * can use it); the next sound opens it again */
    {
        static int16_t quiet[2 * 1024];
        reset(); wanted = NULL; accept_all = 1;
        hw_pcm_write(samples, 4);                          /* (the device is open now) */
        int o = opens, c = closes, d = drops, taken = 1;
        for (int i = 0; i < 30; i++) taken &= hw_pcm_write(quiet, 1024) == 1024;   /* 0.7 s of silence */
        check(taken && closes == c + 1 && drops == d + 1 && opens == o,
              "0.5 s of silence: the card is stopped and closed once (others may use it)");
        check(hw_pcm_write(samples, 4) == 4 && opens == o + 1, "the next sound opens the card again");
        for (int i = 0; i < 30; i++) hw_pcm_write(quiet, 1024);   /* quiet again: closed */
        o = opens;
        for (int i = 0; i < 300; i++) hw_pcm_write(quiet, 1024);  /* 7 s more silence (nothing playing) */
        check(opens == o, "silence after the card was let go never opens it again");
        accept_all = 0;
    }
    return failures ? 1 : 0;
}
