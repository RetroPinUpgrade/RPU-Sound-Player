#include <Arduino.h>
#include "WAVtoDAC.h"
#include "WAVFilePlayer.h"

// Mixer accumulator buffer (lives in CCM RAM - fast, no DMA contention)
int32_t MixBuffer[DMA_BUF_SIZE / 2] __attribute__((section(".ccmram")));

volatile bool SoftLimiter = true;

static inline int16_t ApplyOutputLimiter(int32_t val) {
    if (SoftLimiter) {
        const int32_t threshold = SOFT_THRESH;
        const int32_t limit = 32767;

        int32_t absSample = val < 0 ? -val : val;
        if (absSample <= threshold) {
            return (int16_t)val;
        }

        int32_t over = absSample - threshold;
        if (over >= 16384) {
            absSample = limit;
        } else {
            int32_t soft = over - ((over * over) >> 15);
            absSample = threshold + soft;
        }

        return (int16_t)(val < 0 ? -absSample : absSample);
    } else {
        if (val > HARD_THRESH) val = HARD_THRESH;
        if (val < -HARD_THRESH) val = -HARD_THRESH;
        return (int16_t)val;
    }
}


void Mixer_Process(int16_t *dmaBuffer, uint16_t dmaBufferSize, bool halfTransfer, bool fullTransfer) {
    int16_t *outPtr;
    if (halfTransfer) {
        outPtr = &dmaBuffer[0];
    } else if (fullTransfer) {
        outPtr = &dmaBuffer[dmaBufferSize / 2];
    } else {
        return;
    }

    int stereoFrames = (dmaBufferSize / 2) / 2;

    WAVFile_MixBlock(MixBuffer, (uint32_t)stereoFrames);

    for (int i = 0; i < stereoFrames * 2; i++) {
        outPtr[i] = ApplyOutputLimiter(MixBuffer[i]);
    }
}
