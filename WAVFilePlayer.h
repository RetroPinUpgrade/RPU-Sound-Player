#ifndef WAV_FILE_PLAYER_H
#define WAV_FILE_PLAYER_H

#include <stdint.h>
#include <stddef.h>
#include "ff.h"

#ifdef __cplusplus
extern "C" {
#endif

// Hardware and buffer configuration
#ifndef MAX_VOICES
#define MAX_VOICES                  14
#endif

#ifndef AUDIO_FREQ
#define AUDIO_FREQ                  44100
#endif

// 4096-byte ping-pong buffer per voice (2048 bytes per half = 512 stereo frames)
// 14 voices x 4KB = 56KB SRAM1
#ifndef VOICE_BUF_SIZE
#define VOICE_BUF_SIZE              4096
#endif
#define VOICE_BUF_HALF              (VOICE_BUF_SIZE / 2)
#define VOICE_BUF_SIZE_MASK         (VOICE_BUF_SIZE - 1)

// Hybrid track table: tracks 1..2048 live in SRAM2; 2049..4096 use FAT lookup
#define FAST_TRACK_MAP_SIZE         2048
#define MAX_SUPPORTED_TRACKS        4096

// TUID number space (disjoint from 16-bit track numbers 1..65535)
#define WAV_TUID_BASE               0x00010000UL

// Section attribute for SRAM2 placement on STM32F405
#ifndef WAV_SRAM2_ATTR
#if defined(__GNUC__) && (defined(STM32F405xx) || defined(STM32F4xx))
#define WAV_SRAM2_ATTR              __attribute__((section(".ram2")))
#else
#define WAV_SRAM2_ATTR
#endif
#endif

// Sound classes (RPU-SP API)
#define WAV_CLASS_SFX               0x00
#define WAV_CLASS_CALLOUT           0x01
#define WAV_CLASS_MUSIC             0x02
#define WAV_NUM_CLASSES             3

// Track Control codes (0x03 / 0x0D)
#define WAV_CTRL_PLAY_SOLO          0x00
#define WAV_CTRL_PLAY_POLY          0x01
#define WAV_CTRL_PAUSE              0x02
#define WAV_CTRL_RESUME             0x03
#define WAV_CTRL_STOP               0x04
#define WAV_CTRL_LOOP_ON            0x05
#define WAV_CTRL_LOOP_OFF           0x06
#define WAV_CTRL_LOAD               0x07

// Play Ex flags (0x60)
#define WAV_PLAYEX_LOOP             0x01
#define WAV_PLAYEX_LOCK             0x02
#define WAV_PLAYEX_SOLO_IN_CLASS    0x04
#define WAV_PLAYEX_WAIT_FOR_BAR     0x08
#define WAV_PLAYEX_CROSSFADE        0x10
#define WAV_PLAYEX_REPLACE_EARLIER  0x20
#define WAV_PLAYEX_PAUSE            0x40

// Loop Points flags (0x63)
#define WAV_LOOP_START_PLAYBACK     0x01
#define WAV_LOOP_START_AT_TOP       0x02
#define WAV_LOOP_PLAY_THROUGH_END   0x04
#define WAV_LOOP_ALERT_EACH_LOOP    0x08
#define WAV_LOOP_USE_TUID           0x10
#define WAV_LOOP_UPDATE_COUNT_ONLY  0x20
#define WAV_LOOP_UPDATE_NEXT_ONLY   0x40
#define WAV_LOOP_NEXT_IS_TUID       0x80

// Event flag masks (0x66)
#define WAV_EVTMASK_FINISHED        0x01
#define WAV_EVTMASK_LOOPED          0x02
#define WAV_EVTMASK_MUSIC_CHANGED   0x04
#define WAV_EVTMASK_DUCKING_CHANGE  0x08
#define WAV_EVTMASK_STARTED         0x10
#define WAV_EVTMASK_STAGED_PAUSED   0x20

// Set Soundtrack Folder action flags (0x67)
#define WAV_ST_ACTION_SWITCH        0x01
#define WAV_ST_ACTION_PASSTHROUGH   0x02
#define WAV_ST_ACTION_EXAMPLE       0x80

// Event IDs (0x8A)
#define WAV_EVT_TRACK_FINISHED      0x01
#define WAV_EVT_LOOPING             0x02
#define WAV_EVT_MUSIC_CHANGED       0x03
#define WAV_EVT_DUCKING_CHANGE      0x04
#define WAV_EVT_STARTED             0x05
#define WAV_EVT_STAGED_PAUSED       0x06

// Internal voice playbackFlags bitmask
#define WAV_FLAG_STEREO             0x0001
#define WAV_FLAG_PAUSED             0x0002
#define WAV_FLAG_LOCKED             0x0004
#define WAV_FLAG_WAIT_FOR_BAR       0x0008
#define WAV_FLAG_PLAY_THROUGH_END   0x0010
#define WAV_FLAG_ALERT_EACH_LOOP    0x0020
#define WAV_FLAG_ALERT_SENT         0x0040
#define WAV_FLAG_STOP_AFTER_FADE    0x0080
#define WAV_FLAG_CROSSFADE_OUT      0x0100
#define WAV_FLAG_LOOP_FADE_IN       0x0200

// Voice lifecycle states
#define WAV_STATE_FREE              0
#define WAV_STATE_ACTIVE            1
#define WAV_STATE_STOPPING          2

// Gain range (dB)
#define WAV_GAIN_MIN_DB             (-70)
#define WAV_GAIN_MAX_DB             (10)

// Callout priority queue and event queue sizes
#define WAV_CALLOUT_QUEUE_SIZE      8
#define WAV_EVENT_QUEUE_SIZE        24
#define WAV_MAX_ALT_SOUNDTRACKS     255

#define WAV_MAX_STORED_LOOPS        128
typedef struct {
    uint16_t trackNumber;
    uint8_t  loopNumber;
    uint8_t  loopsPerPhrase;
    uint32_t pointA;
    uint32_t pointB;
} WAVStoredLoop;

// SRAM2 direct-cluster map entry (8 bytes each x 2048 = 16KB)
typedef struct {
    uint32_t sclust;
    uint32_t fsize;
} WAVFileMapEntry;

// Voice slot structure
typedef struct {
    uint32_t numSamples;            // Total sample frames in PCM data
    uint32_t curSample;             // Current mixer playback frame
    uint32_t readSample;            // Current refiller file frame
    uint32_t dataStartOffset;       // Byte offset to start of PCM data chunk

    uint32_t voiceUniqueIndex;      // TUID (0x00010000..0xFFFFFFFF)
    uint32_t startIndexA;           // Mixer loop point A (sample frames)
    uint32_t endIndexB;             // Mixer loop point B (0xFFFFFFFF = EOF)
    uint32_t refillStartA;          // Refiller loop point A
    uint32_t refillEndB;            // Refiller loop point B
    uint32_t alertMarginSamples;    // Samples before B to fire loop alert

    // Pending A/B loop update (adopted at next B or EOF boundary)
    uint32_t pendingStartA;
    uint32_t pendingEndB;
    uint32_t pendingFadeInSamples;
    uint32_t pendingFadeOutSamples;
    uint32_t pendingAlertMarginSamples;
    uint32_t pendingNextTrack;
    uint16_t pendingFlags;
    uint8_t  pendingLoopCount;
    volatile uint8_t pendingLoopRefill;
    volatile uint8_t pendingLoopMix;
    uint8_t  refillPlayThroughEnd;

    // Phrase support
    uint8_t  loopsPerPhrase;
    uint8_t  phraseLoopsLeft;
    uint8_t  refillLoopsPerPhrase;
    uint8_t  refillPhraseLoopsLeft;
    uint8_t  pendingLoopsPerPhrase;    

    int32_t  currentGainQ16;        // Current track gain in Q16 dB
    int32_t  targetGainQ16;         // Target track gain in Q16 dB
    int32_t  fadeStepPerMsQ16;      // Gain delta per millisecond in Q16 dB
    uint32_t fadeInSamples;         // Loop fade-in duration (samples)
    uint32_t fadeOutSamples;        // Loop fade-out duration (samples)

    FIL      fil;                   // Raw FatFs file object
    uint16_t fileIndex;             // Track number (1..4096)
    uint32_t nextTrack;             // Staged track (1..65535) or TUID (0x00010000+)
    uint16_t playbackFlags;         // Internal playback attribute flags

    uint8_t  soundClass;            // WAV_CLASS_SFX / CALLOUT / MUSIC
    uint8_t  priority;              // 0x00..0xFF (higher = higher priority)
    uint8_t  numLoopsLeft;          // Mixer remaining loops (0xFF = infinite)
    uint8_t  refillLoopsLeft;       // Refiller remaining loops (0xFF = infinite)

    volatile uint8_t voiceInUse;    // WAV_STATE_FREE / ACTIVE / STOPPING
    volatile uint8_t halfReady[2];  // 1 if buffer half has unread audio
    uint16_t bufReadPos;            // Current byte offset inside buffer[]

    uint8_t  isFlashSource;         // 1 if reading from internal flash
    const uint8_t *flashBase;       // Base pointer to flash PCM data
    uint32_t flashDataLen;          // Length of flash PCM data in bytes

    uint8_t  buffer[VOICE_BUF_SIZE] __attribute__((aligned(4)));
} WAVFileVoice;

// Callback signatures for decoupled serial responses (0x84 and 0x8A)
typedef void (*WAVTrackReportCallback)(uint16_t trackNumber, uint8_t voiceIndex, uint8_t voiceStatus);
typedef void (*WAVEventCallback)(uint8_t eventId, uint16_t trackNumber, uint32_t tuid,
                                 uint8_t soundClass, uint8_t hasData, uint8_t eventData);

// Core lifecycle and storage functions
void WAVFile_Init(void);
void WAVFile_RegisterCallbacks(WAVTrackReportCallback reportCb, WAVEventCallback eventCb);
void WAVFile_ScanSoundtracks(void);
void WAVFile_BuildClusterMap(void);
uint8_t WAVFile_GetSoundtrackCount(void);
uint8_t WAVFile_SetSoundtrackFolder(uint8_t actionFlags, uint8_t folderNum);
uint16_t WAVFile_GetTrackCount(void);
uint8_t WAVFile_GetMaxVoices(void);

// WAV header parser (kept compatible with existing name)
uint8_t ParseWavHeader(FIL *file, WAVFileVoice *v);

// Legacy and flash playback entry points
uint8_t WAVFile_StartVoice(FIL fileToPlay, uint8_t priority);
uint8_t WAVFile_StartFlashVoice(const uint8_t *dataPtr, uint32_t dataLen);

// RPU-SP command handlers
uint8_t WAVFile_TrackControl(uint8_t controlCode, uint16_t trackNumber, uint8_t lockFlag);
void WAVFile_StopAllVoices(void);
void WAVFile_SetSystemVolume(int16_t gainDb);
uint8_t WAVFile_GetPlayingTracks(uint16_t *outTracks, uint8_t maxOut);
void WAVFile_SetTrackVolume(uint16_t trackNumber, int16_t gainDb);
void WAVFile_FadeTrack(uint16_t trackNumber, int16_t targetDb, uint16_t fadeTimeMs, uint8_t stopFlag);
void WAVFile_SetReporting(uint8_t enabled);

uint32_t WAVFile_PlayEx(uint16_t trackNumber, uint8_t soundClass, uint8_t flags,
                        int16_t gainDb, uint8_t priority);
void WAVFile_SetClassGain(uint8_t soundClass, int16_t gainDb, uint16_t fadeTimeMs);
void WAVFile_SetDuckRule(uint8_t duckedClass, uint8_t duckingClass, int8_t gainDb,
                         uint16_t attackMs, uint16_t releaseMs);
void WAVFile_SetLoopPoints(uint8_t flags, uint8_t loopCount, uint32_t pointA, uint32_t pointB,
                           uint16_t fadeInMs, uint16_t fadeOutMs, uint8_t loopsPerPhrase,
                           uint16_t alertMarginMs, uint32_t nextTrackOrTuid, uint32_t trackOrTuid);
uint8_t WAVFile_GetTrackLoopCount(uint16_t trackNumber);
uint8_t WAVFile_GetTrackLoop(uint16_t trackNumber, uint8_t loopIndex, WAVStoredLoop *outLoop);
void WAVFile_StopClass(uint8_t soundClass, uint16_t fadeTimeMs);
void WAVFile_SetEventFlags(uint8_t mask, uint8_t soundClass);

// Main-loop audio pump, fade tick, and event dispatcher
void WAVFile_RefillVoices(void);
void WAVFile_UpdateFades(uint32_t elapsedMs);
void WAVFile_ProcessEvents(void);

// Sample-by-sample mixer output (no SD I/O inside)
uint8_t WAVFile_GetNextSample(int32_t *leftChannel, int32_t *rightChannel);

// Direct voice slot access if needed by caller
WAVFileVoice *WAVFile_GetVoiceSlot(uint8_t slotNum);

void WAVFile_MixBlock(int32_t *mixBuffer, uint32_t stereoFrames);

#ifdef __cplusplus
}
#endif

#endif