#ifndef WAV_TO_DAC_H
#define WAV_TO_DAC_H

#include <stdint.h>
#include <stdbool.h>
#include "WAVFilePlayer.h"

// Total output DMA buffer (int16_t stereo samples: 2048 total = 1024 per half = 512 stereo frames)
// Matches VOICE_BUF_HALF (2048 bytes = 512 stereo frames)
#define DMA_BUF_SIZE    2048

#define SOFT_THRESH     24575
#define MAX_INT16       32767
#define SOFT_OVERLOAD   48000
#define HARD_THRESH     32700

extern volatile bool SoftLimiter;

void Mixer_Process(int16_t *dmaBuffer, uint16_t dmaBufferSize, bool halfTransfer, bool fullTransfer);

#endif
