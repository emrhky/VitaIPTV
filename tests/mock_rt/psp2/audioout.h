#ifndef MOCKRT_AUDIOOUT_H
#define MOCKRT_AUDIOOUT_H
enum { SCE_AUDIO_OUT_PORT_TYPE_MAIN = 0, SCE_AUDIO_OUT_PORT_TYPE_BGM = 1, SCE_AUDIO_OUT_PORT_TYPE_VOICE = 2 };
enum { SCE_AUDIO_OUT_MODE_MONO = 0, SCE_AUDIO_OUT_MODE_STEREO = 1 };
#define SCE_AUDIO_VOLUME_0DB 32768
#define SCE_AUDIO_VOLUME_FLAG_L_CH 1
#define SCE_AUDIO_VOLUME_FLAG_R_CH 2
int sceAudioOutOpenPort(int type, int len, int freq, int mode);
int sceAudioOutReleasePort(int port);
int sceAudioOutOutput(int port, const void *buf);
int sceAudioOutSetVolume(int port, int ch, int *vol);
#endif
