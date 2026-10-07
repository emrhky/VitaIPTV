/* Layout copied from vitasdk include/psp2/audiodec.h (docs.vitasdk.org) */
#ifndef MOCKRT_AUDIODEC_H
#define MOCKRT_AUDIODEC_H
#include <psp2/types.h>
#define SCE_AUDIODEC_ALIGNMENT_SIZE 0x100U
#define SCE_AUDIODEC_ROUND_UP(size) ((size + SCE_AUDIODEC_ALIGNMENT_SIZE - 1) & ~(SCE_AUDIODEC_ALIGNMENT_SIZE - 1))
#define SCE_AUDIODEC_WORD_LENGTH_16BITS 16
#define SCE_AUDIODEC_TYPE_AAC 0x1005U
#define SCE_AUDIODEC_AAC_MAX_ES_SIZE 1536
#define SCE_AUDIODEC_AAC_MAX_SAMPLES 2048
typedef struct { SceUInt32 size, totalStreams; } SceAudiodecInitStreamParam;
typedef struct { SceUInt32 size, totalCh; } SceAudiodecInitChParam;
typedef union { SceUInt32 size; SceAudiodecInitChParam at9; SceAudiodecInitStreamParam mp3, aac, celp; } SceAudiodecInitParam;
typedef struct { SceUInt32 size, isAdts, ch, samplingRate, isSbr; } SceAudiodecInfoAac;
typedef union { SceUInt32 size; uint8_t at9_like[0x1C]; SceAudiodecInfoAac aac; } SceAudiodecInfo;
typedef struct { SceUInt32 size; SceInt32 handle; SceUInt8 *pEs; SceUInt32 inputEsSize, maxEsSize; void *pPcm;
                 SceUInt32 outputPcmSize, maxPcmSize, wordLength; SceAudiodecInfo *pInfo; } SceAudiodecCtrl;
SceInt32 sceAudiodecInitLibrary(SceUInt32 codecType, SceAudiodecInitParam *pInitParam);
SceInt32 sceAudiodecTermLibrary(SceUInt32 codecType);
SceInt32 sceAudiodecCreateDecoder(SceAudiodecCtrl *pCtrl, SceUInt32 codecType);
SceInt32 sceAudiodecDeleteDecoder(SceAudiodecCtrl *pCtrl);
SceInt32 sceAudiodecDecode(SceAudiodecCtrl *pCtrl);
#endif
