/* Struct layout copied from vitasdk include/psp2/videodec.h (docs.vitasdk.org) */
#ifndef MOCKRT_VIDEODEC_H
#define MOCKRT_VIDEODEC_H
#include <psp2/types.h>
typedef enum { SCE_VIDEODEC_TYPE_HW_AVCDEC = 0x1001 } SceVideodecType;
typedef enum { SCE_AVCDEC_PIXELFORMAT_RGBA8888 = 0x00, SCE_AVCDEC_PIXELFORMAT_RGBA565 = 0x01, SCE_AVCDEC_PIXELFORMAT_RGBA5551 = 0x02,
               SCE_AVCDEC_PIXELFORMAT_YUV420_RASTER = 0x10, SCE_AVCDEC_PIXELFORMAT_YUV420_PACKED_RASTER = 0x20 } SceAvcdecPixelFormat;
typedef struct { uint32_t size, horizontal, vertical, numOfRefFrames, numOfStreams; } SceVideodecQueryInitInfoHwAvcdec;
typedef union { uint8_t reserved[32]; SceVideodecQueryInitInfoHwAvcdec hwAvc; } SceVideodecQueryInitInfo;
typedef struct { uint32_t upper, lower; } SceVideodecTimeStamp;
typedef struct { uint32_t horizontal, vertical, numOfRefFrames; } SceAvcdecQueryDecoderInfo;
typedef struct { uint32_t frameMemSize; } SceAvcdecDecoderInfo;
typedef struct { void *pBuf; uint32_t size; } SceAvcdecBuf;
typedef struct { uint32_t handle; SceAvcdecBuf frameBuf; } SceAvcdecCtrl;
typedef struct { SceVideodecTimeStamp pts, dts; SceAvcdecBuf es; } SceAvcdecAu;
typedef struct { uint32_t numUnitsInTick, timeScale; uint8_t fixedFrameRateFlag, aspectRatioIdc; uint16_t sarWidth, sarHeight;
                 uint8_t colourPrimaries, transferCharacteristics, matrixCoefficients, videoFullRangeFlag, picStruct[2], ctType;
                 SceVideodecTimeStamp pts; } SceAvcdecInfo;
typedef struct { uint8_t alpha, cscCoefficient, reserved[14]; } SceAvcdecFrameOptionRGBA;
typedef union { uint8_t reserved[16]; SceAvcdecFrameOptionRGBA rgba; } SceAvcdecFrameOption;
typedef struct { uint32_t pixelType, framePitch, frameWidth, frameHeight, horizontalSize, verticalSize;
                 uint32_t frameCropLeftOffset, frameCropRightOffset, frameCropTopOffset, frameCropBottomOffset;
                 SceAvcdecFrameOption opt; void *pPicture[2]; } SceAvcdecFrame;
typedef struct { uint32_t size; SceAvcdecFrame frame; SceAvcdecInfo info; } SceAvcdecPicture;
typedef struct { uint32_t numOfOutput, numOfElm; SceAvcdecPicture **pPicture; } SceAvcdecArrayPicture;
int sceVideodecInitLibrary(SceVideodecType codec, const SceVideodecQueryInitInfoHwAvcdec *initInfo);
int sceVideodecTermLibrary(SceVideodecType codec);
int sceAvcdecQueryDecoderMemSize(SceVideodecType codec, const SceAvcdecQueryDecoderInfo *query, SceAvcdecDecoderInfo *decoderInfo);
int sceAvcdecCreateDecoder(SceVideodecType codec, SceAvcdecCtrl *decoder, const SceAvcdecQueryDecoderInfo *query);
int sceAvcdecDeleteDecoder(SceAvcdecCtrl *decoder);
int sceAvcdecDecode(const SceAvcdecCtrl *decoder, const SceAvcdecAu *au, SceAvcdecArrayPicture *array_picture);
#endif
