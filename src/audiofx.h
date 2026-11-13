#ifndef AUDIOFX_H
#define AUDIOFX_H
/* 1.1: sound before it goes to the Vita (Select > Audio):
 *   boost    0-3: off, +3, +6, +9 dB, with a limiter that turns the gain down before a sound would clip
 *            (no hard clipping, no pumping on quiet channels)
 *   speaker  0/1: for the Vita's small speakers: the deep bass they cannot play (it only distorts) is cut
 *            below about 150 Hz and the voice range around 3 kHz lifted a little
 *   level    0/1 (1.2): the same loudness on every channel: the sound's average level is measured over a few
 *            seconds and brought towards one level (-20 dBFS), between -6 and +12 dB, a few dB a second at
 *            most (quickly in the first seconds of a channel); silence does not count (nothing is lifted
 *            towards a pause). The limiter above keeps the peaks.
 *   field    0-2 (1.3, stereo sound): the sound field.
 *            1 speakers: wider stereo for the Vita's speakers, which sit close together: the difference between
 *              the sides (above about 300 Hz) is made louder; what is in the middle (voices, mono) is not changed
 *            2 headphones: crossfeed: each ear also hears the other side's low and middle sounds a little, as from
 *              speakers in a room, so hard left / right sound is not tiring; mono sound is not changed.
 *              5.1 sound gets virtual surround too (vsurr.h, in the mixes down to stereo).
 * With all off the sound is not touched at all (exactly as before 1.1). Portable C (PC tests). */
#include <stdint.h>

typedef struct {
    int boost, speaker, level, rate, ch;
    float gain;                         /* the limiter's gain now (linear) */
    float lv_pow, lv_gain;              /* 1.2 level: the average power (mean square), its gain now */
    int lv_n;                           /* chunks of sound measured (the first seconds: faster) */
    float b[2][5];                      /* two biquads: high-pass, presence: b0 b1 b2 a1 a2 */
    float z[2][2][2];                   /* [filter][channel][state] */
    int field;                          /* 1.3: 0 off, 1 speakers (wider), 2 headphones (crossfeed) */
    float f_a;                          /* ... its one-pole filter's coefficient (speakers: 300 Hz, headphones: 700 Hz) */
    float f_z[2];                       /* ... its filter states */
} AudioFx;

void afx_init(AudioFx *fx);
void afx_set(AudioFx *fx, int boost, int speaker, int level);
int  afx_active(const AudioFx *fx);
/* in place: interleaved 16-bit, 1 or 2 channels */
void afx_process(AudioFx *fx, int16_t *pcm, int samples, int ch, int rate);

/* the settings shared by the players (tsplayer.c, player.c); afx_set_global from the UI thread */
void afx_set_global(int boost, int speaker, int level);
void afx_get_global(int *boost, int *speaker, int *level);
/* 1.3: the sound field (0 off, 1 speakers, 2 headphones), apart so the calls above stay as they were */
void afx_set_field(int field);
int  afx_get_field(void);
void afx_field(AudioFx *fx, int field);
#endif
