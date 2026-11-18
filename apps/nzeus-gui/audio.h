#ifndef NZEUS_AUDIO_H
#define NZEUS_AUDIO_H
#include <stdint.h>

/* The music player's engine: plays one WAV or MP3 file.
 *
 * Sound leaves NZEUS the libretro way: every frame core.c calls
 * audio_render() for the next samples (44100 per second, stereo, 16 bit;
 * 48000 for 48 kHz songs) and hands them to the frontend (the ALSA card). The file is decoded a little at a time, so it
 * never has to fit in memory. Other sample rates (22050, 48000...) and mono
 * files are converted on the fly. */

#define AUDIO_RATE 44100
extern int audio_out_rate;   /* the rate audio_render() makes: AUDIO_RATE, or 48000 for 48 kHz songs */

int  audio_open(const char *path, char *err, int errn);   /* 0 = ok (paused at the start) */
void audio_close(void);
void audio_play(int on);                 /* play / pause */
int  audio_playing(void);
int  audio_loaded(void);
unsigned audio_revision(void);              /* changes on open/close, seek, play/pause */
int  audio_finished(void);               /* reached the end */
double audio_position(void);             /* seconds */
double audio_length(void);               /* seconds (MP3: estimated), 0 = unknown */
void audio_seek(double seconds);         /* jump (clamped to the file) */
int  audio_info(int *rate, int *channels);   /* the file's own format */
/* the file that is open ("" = none). There is one speaker: Media and WhatsApp's
 * voice messages check this to know whether the sound playing is still theirs. */
const char *audio_path(void);

/* fill `frames` stereo frames (2 * frames int16) - silence when not playing */
void audio_render(int16_t *out, int frames);

#endif
