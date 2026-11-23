#include "WAVFilePlayer.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

// Q14 linear gain lookup table for -70 dB to +10 dB (0 dB = 16384 at index 70)
static const uint16_t GainTableQ14[81] = {
    5, 6, 7, 7, 8, 9, 10, 12, 13, 15,
    16, 18, 21, 23, 26, 29, 33, 37, 41, 46,
    52, 58, 65, 73, 82, 92, 103, 116, 130, 146,
    164, 184, 206, 231, 260, 291, 327, 367, 412, 462,
    518, 581, 652, 732, 821, 921, 1034, 1160, 1301, 1460,
    1638, 1838, 2063, 2314, 2597, 2914, 3269, 3668, 4115, 4618,
    5181, 5813, 6523, 7318, 8211, 9213, 10338, 11599, 13014, 14602,
    16384, 18383, 20626, 23143, 25967, 29135, 32690, 36679, 41155, 46176,
    51811
};

// SRAM2 fast cluster lookup table for tracks 1..2048
static WAVFileMapEntry FileMap[FAST_TRACK_MAP_SIZE] WAV_SRAM2_ATTR;
static FATFS *MainFS = NULL;

// Active voices
WAVFileVoice Voices[MAX_VOICES];
static uint32_t NextTUID = 1;
static uint32_t WAVCurrentRefillVoice = 0;

// Global system volume (Q16 dB)
static int32_t SystemVolumeQ16 = 0;

// Reporting and callbacks
static uint8_t ReportingEnabled = 0;
static uint8_t ClassEventMask[WAV_NUM_CLASSES] = { 0, 0, 0 };
static WAVTrackReportCallback TrackReportCb = NULL;
static WAVEventCallback EventCb = NULL;

// Soundtrack folder state
static uint8_t AltSoundtrackNumbers[WAV_MAX_ALT_SOUNDTRACKS];
static uint8_t AltSoundtrackCount = 0;
static uint8_t CurrentSoundtrackFolder = 0;
static char ActiveDirPath[80] = "/";
static uint16_t TotalPlayableTracks = 0;

// Per-class gain and ducking state
typedef struct {
    int32_t currentGainQ16;
    int32_t targetGainQ16;
    int32_t fadeStepPerMsQ16;
    int32_t currentDuckQ16;
    uint8_t isDucked;
} ClassState;

typedef struct {
    uint8_t enabled;
    int8_t  attenuationDb;
    uint16_t attackMs;
    uint16_t releaseMs;
} DuckRule;

static ClassState Classes[WAV_NUM_CLASSES];
static DuckRule DuckRules[WAV_NUM_CLASSES][WAV_NUM_CLASSES];

// Callout priority queue
typedef struct {
    uint8_t  inUse;
    uint16_t trackNumber;
    uint8_t  flags;
    int16_t  gainDb;
    uint8_t  priority;
} CalloutQueueEntry;

static CalloutQueueEntry CalloutQueue[WAV_CALLOUT_QUEUE_SIZE];

// Deferred event queue so mixer never stalls on serial TX
typedef struct {
    uint8_t  isReport;
    uint8_t  eventId;
    uint16_t trackNumber;
    uint32_t tuid;
    uint8_t  soundClass;
    uint8_t  voiceIndex;
    uint8_t  hasData;
    uint8_t  eventData;
} QueuedEvent;


static QueuedEvent EventQueue[WAV_EVENT_QUEUE_SIZE];
static volatile uint8_t EventQueueHead = 0;
static volatile uint8_t EventQueueTail = 0;

// --- Internal Helper Functions ---

static int32_t ClampDbQ16(int32_t valQ16) {
    int32_t minQ16 = (WAV_GAIN_MIN_DB * 65536);
    int32_t maxQ16 = (WAV_GAIN_MAX_DB * 65536);
    if (valQ16 < minQ16) return minQ16;
    if (valQ16 > maxQ16) return maxQ16;
    return valQ16;
}

static uint32_t GainQ16ToLinearQ14(int32_t dbQ16) {
    if (dbQ16 <= ((WAV_GAIN_MIN_DB * 65536))) {
        return 0;
    }
    if (dbQ16 >= ((WAV_GAIN_MAX_DB * 65536))) {
        return GainTableQ14[80];
    }
    int32_t shifted = dbQ16 - ((WAV_GAIN_MIN_DB * 65536));
    int32_t idx = shifted >> 16;
    int32_t frac = shifted & 0xFFFF;
    if (idx < 0) return 0;
    if (idx >= 80) return GainTableQ14[80];
    uint32_t g0 = GainTableQ14[idx];
    uint32_t g1 = GainTableQ14[idx + 1];
    return g0 + (uint32_t)((((int32_t)(g1 - g0)) * frac) >> 16);
}

static void PushTrackReport(uint16_t trackNumber, uint8_t voiceIndex, uint8_t voiceStatus) {
    if (!ReportingEnabled || trackNumber==0) return;
    uint8_t nextHead = (uint8_t)((EventQueueHead + 1) % WAV_EVENT_QUEUE_SIZE);
    if (nextHead == EventQueueTail) return;
    QueuedEvent *e = &EventQueue[EventQueueHead];
    e->isReport = 1;
    e->trackNumber = trackNumber;
    e->voiceIndex = voiceIndex;
    e->eventData = voiceStatus;
    EventQueueHead = nextHead;
}

static void PushEvent(uint8_t eventId, uint16_t trackNumber, uint32_t tuid,
                      uint8_t soundClass, uint8_t hasData, uint8_t eventData) {
    if (soundClass >= WAV_NUM_CLASSES) return;
    uint8_t maskBit = 0;
    switch (eventId) {
        case WAV_EVT_TRACK_FINISHED: maskBit = WAV_EVTMASK_FINISHED; break;
        case WAV_EVT_LOOPING:        maskBit = WAV_EVTMASK_LOOPED; break;
        case WAV_EVT_MUSIC_CHANGED:  maskBit = WAV_EVTMASK_MUSIC_CHANGED; break;
        case WAV_EVT_DUCKING_CHANGE: maskBit = WAV_EVTMASK_DUCKING_CHANGE; break;
        case WAV_EVT_STARTED:        maskBit = WAV_EVTMASK_STARTED; break;
        default: return;
    }
    if ((ClassEventMask[soundClass] & maskBit) == 0) return;

    uint8_t nextHead = (uint8_t)((EventQueueHead + 1) % WAV_EVENT_QUEUE_SIZE);
    if (nextHead == EventQueueTail) return;
    QueuedEvent *e = &EventQueue[EventQueueHead];
    e->isReport = 0;
    e->eventId = eventId;
    e->trackNumber = trackNumber;
    e->tuid = tuid;
    e->soundClass = soundClass;
    e->hasData = hasData;
    e->eventData = eventData;
    EventQueueHead = nextHead;
}

static int CaseInsensitivePrefix(const char *str, const char *prefix) {
    while (*prefix) {
        if (tolower((unsigned char)*str) != tolower((unsigned char)*prefix)) {
            return 0;
        }
        str++;
        prefix++;
    }
    return 1;
}

static uint8_t ParseTrackPrefix(const char *fname, uint16_t *outTrack) {
    if (!isdigit((unsigned char)fname[0])) return 0;
    long val = strtol(fname, NULL, 10);
    if (val < 1 || val > MAX_SUPPORTED_TRACKS) return 0;
    *outTrack = (uint16_t)val;
    return 1;
}

static uint8_t IsClassPlaying(uint8_t soundClass) {
    for (uint8_t i = 0; i < MAX_VOICES; i++) {
        if (Voices[i].voiceInUse == WAV_STATE_ACTIVE &&
            Voices[i].soundClass == soundClass &&
            !(Voices[i].playbackFlags & (WAV_FLAG_PAUSED | WAV_FLAG_WAIT_FOR_BAR))) {
            return 1;
        }
    }
    return 0;
}

static void TriggerMusicBarWaiters(void) {
    for (uint8_t i = 0; i < MAX_VOICES; i++) {
        WAVFileVoice *v = &Voices[i];
        if (v->voiceInUse == WAV_STATE_ACTIVE && (v->playbackFlags & WAV_FLAG_WAIT_FOR_BAR)) {
            v->playbackFlags &= (uint16_t)(~WAV_FLAG_WAIT_FOR_BAR);
            v->playbackFlags &= (uint16_t)(~WAV_FLAG_PAUSED);
            PushTrackReport(v->fileIndex, i, 0x01);
            PushEvent(WAV_EVT_STARTED, v->fileIndex, v->voiceUniqueIndex, v->soundClass, 0, 0);
        }
    }
}

static void TriggerNextTrack(uint16_t nextTrack) {
    if (nextTrack == 0) return;
    for (uint8_t i = 0; i < MAX_VOICES; i++) {
        WAVFileVoice *v = &Voices[i];
        if (v->voiceInUse == WAV_STATE_ACTIVE &&
            v->fileIndex == nextTrack &&
            (v->playbackFlags & WAV_FLAG_PAUSED)) {
            v->playbackFlags &= (uint16_t)(~WAV_FLAG_PAUSED);
            PushTrackReport(v->fileIndex, i, 0x01);
            PushEvent(WAV_EVT_STARTED, v->fileIndex, v->voiceUniqueIndex, v->soundClass, 0, 0);
            break;
        }
    }
}

// Open track using SRAM2 fast cluster map (1..2048) or FAT directory search (2049..4096)
static uint8_t OpenTrackFile(uint16_t trackNumber, FIL *outFil) {
    if (trackNumber < 1 || trackNumber > MAX_SUPPORTED_TRACKS) return 0;

    // Fast path: direct cluster open from SRAM2 table
    if (trackNumber <= FAST_TRACK_MAP_SIZE && MainFS != NULL) {
        WAVFileMapEntry *entry = &FileMap[trackNumber - 1];
        if (entry->sclust != 0 && entry->fsize > 0) {
            memset(outFil, 0, sizeof(FIL));
            outFil->obj.fs = MainFS;
            outFil->obj.sclust = entry->sclust;
            outFil->obj.objsize = entry->fsize;
            outFil->flag = FA_READ;
            outFil->obj.id = MainFS->id;
            return 1;
        }
    }

    // Fallback path: scan active directory via FatFs
    DIR dir;
    FILINFO fno;
    if (f_opendir(&dir, ActiveDirPath) != FR_OK) return 0;

    uint8_t found = 0;
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
        if (fno.fattrib & AM_DIR) continue;
        uint16_t id = 0;
        if (ParseTrackPrefix(fno.fname, &id) && id == trackNumber) {
            char fullPath[160];
            if (ActiveDirPath[0] == '/' && ActiveDirPath[1] == '\0') {
                snprintf(fullPath, sizeof(fullPath), "/%.158s", fno.fname);
            } else {
                snprintf(fullPath, sizeof(fullPath), "%.79s/%.79s", ActiveDirPath, fno.fname);
            }
            if (f_open(outFil, fullPath, FA_READ) == FR_OK) {
                found = 1;
            }
            break;
        }
    }
    f_closedir(&dir);
    return found;
}

static void FillVoiceHalf(WAVFileVoice *v, uint8_t halfIdx) {
    uint8_t *dst = v->buffer + (halfIdx ? VOICE_BUF_HALF : 0);
    uint32_t bytesNeeded = VOICE_BUF_HALF;
    uint32_t bytesFilled = 0;
    uint32_t bytesPerSample = (v->playbackFlags & WAV_FLAG_STEREO) ? 4 : 2;

    uint32_t endPoint = v->numSamples;
    if (v->endIndexB != 0xFFFFFFFF && v->endIndexB > 0 && v->endIndexB < v->numSamples) {
        endPoint = v->endIndexB;
    }

    while (bytesFilled < bytesNeeded) {
        uint32_t stopSample = (v->refillLoopsLeft > 0) ? endPoint :
            ((v->playbackFlags & WAV_FLAG_PLAY_THROUGH_END) ? v->numSamples : endPoint);

        if (v->readSample >= stopSample) {
            if (v->refillLoopsLeft > 0) {
                if (v->refillLoopsLeft != 0xFF) {
                    v->refillLoopsLeft--;
                }
                uint32_t loopA = (v->startIndexA < v->numSamples) ? v->startIndexA : 0;
                v->readSample = loopA;
                if (!v->isFlashSource) {
                    f_lseek(&v->fil, v->dataStartOffset + (loopA * bytesPerSample));
                }
                continue;
            } else {
                // Pad remainder with silence
                memset(dst + bytesFilled, 0, bytesNeeded - bytesFilled);
                break;
            }
        }

        uint32_t samplesAvail = stopSample - v->readSample;
        uint32_t bytesAvail = samplesAvail * bytesPerSample;
        uint32_t toRead = bytesNeeded - bytesFilled;
        if (toRead > bytesAvail) toRead = bytesAvail;

        if (v->isFlashSource) {
            uint32_t offset = v->readSample * bytesPerSample;
            if (offset + toRead > v->flashDataLen) {
                toRead = (offset < v->flashDataLen) ? (v->flashDataLen - offset) : 0;
            }
            if (toRead > 0) {
                memcpy(dst + bytesFilled, v->flashBase + offset, toRead);
            }
            bytesFilled += toRead;
            v->readSample += toRead / bytesPerSample;
            if (toRead == 0) {
                memset(dst + bytesFilled, 0, bytesNeeded - bytesFilled);
                break;
            }
        } else {
            UINT br = 0;
            if (f_read(&v->fil, dst + bytesFilled, toRead, &br) != FR_OK || br == 0) {
                memset(dst + bytesFilled, 0, bytesNeeded - bytesFilled);
                break;
            }
            bytesFilled += br;
            v->readSample += br / bytesPerSample;
        }
    }

    v->halfReady[halfIdx] = 1;
}

static int8_t AllocateVoiceSlot(uint8_t soundClass, uint8_t priority) {
    // 1. Prefer a free voice slot
    for (uint8_t i = 0; i < MAX_VOICES; i++) {
        if (Voices[i].voiceInUse == WAV_STATE_FREE) {
            return (int8_t)i;
        }
    }

    // 2. For SFX, steal an unlocked SFX voice with lower priority
    if (soundClass == WAV_CLASS_SFX) {
        int8_t victim = -1;
        uint8_t lowestPrio = priority;
        for (uint8_t i = 0; i < MAX_VOICES; i++) {
            if (Voices[i].voiceInUse == WAV_STATE_ACTIVE &&
                Voices[i].soundClass == WAV_CLASS_SFX &&
                !(Voices[i].playbackFlags & WAV_FLAG_LOCKED) &&
                Voices[i].priority < lowestPrio) {
                lowestPrio = Voices[i].priority;
                victim = (int8_t)i;
            }
        }
        if (victim != -1) {
            if (!Voices[victim].isFlashSource) {
                f_close(&Voices[victim].fil);
            }
            Voices[victim].voiceInUse = WAV_STATE_FREE;
            return victim;
        }
    }

    // 3. For Music or Callout, steal any unlocked non-music voice if needed
    if (soundClass != WAV_CLASS_SFX) {
        for (uint8_t i = 0; i < MAX_VOICES; i++) {
            if (Voices[i].voiceInUse == WAV_STATE_ACTIVE &&
                Voices[i].soundClass == WAV_CLASS_SFX &&
                !(Voices[i].playbackFlags & WAV_FLAG_LOCKED)) {
                if (!Voices[i].isFlashSource) {
                    f_close(&Voices[i].fil);
                }
                Voices[i].voiceInUse = WAV_STATE_FREE;
                return (int8_t)i;
            }
        }
    }

    return -1;
}

static void StartVoiceInternal(int8_t voiceIdx, FIL *openedFil, uint16_t trackNumber,
                               uint8_t soundClass, uint8_t flags, int16_t gainDb, uint8_t priority) {
    WAVFileVoice *v = &Voices[voiceIdx];
    v->voiceInUse = WAV_STATE_FREE;

    v->fil = *openedFil;
    v->isFlashSource = 0;
    v->flashBase = NULL;
    v->flashDataLen = 0;
    v->fileIndex = trackNumber;
    v->soundClass = soundClass;
    v->priority = priority;
    v->nextTrack = 0;
    v->playbackFlags = 0;
    v->curSample = 0;
    v->readSample = 0;
    v->numSamples = 0;
    v->startIndexA = 0;
    v->endIndexB = 0xFFFFFFFF;
    v->alertMarginSamples = 0;
    v->fadeInSamples = 0;
    v->fadeOutSamples = 0;
    v->bufReadPos = 0;
    v->halfReady[0] = 0;
    v->halfReady[1] = 0;

    if (!ParseWavHeader(&v->fil, v)) {
        f_close(&v->fil);
        return;
    }

    v->dataStartOffset = (uint32_t)f_tell(&v->fil);

    if (flags & WAV_PLAYEX_LOOP) {
        v->numLoopsLeft = 0xFF;
        v->refillLoopsLeft = 0xFF;
    } else {
        v->numLoopsLeft = 0;
        v->refillLoopsLeft = 0;
    }

    if (flags & WAV_PLAYEX_LOCK) v->playbackFlags |= WAV_FLAG_LOCKED;
    if (flags & WAV_PLAYEX_PAUSE) v->playbackFlags |= WAV_FLAG_PAUSED;
    if (flags & WAV_PLAYEX_WAIT_FOR_BAR) {
        v->playbackFlags |= (WAV_FLAG_WAIT_FOR_BAR | WAV_FLAG_PAUSED);
    }

    int32_t clampedQ16 = ClampDbQ16((((int32_t)gainDb) * 65536));
    v->currentGainQ16 = clampedQ16;
    v->targetGainQ16 = clampedQ16;
    v->fadeStepPerMsQ16 = 0;

    v->voiceUniqueIndex = NextTUID++;
    if (NextTUID == 0) NextTUID = 1;

    // Pre-fill both halves so mixer has immediate audio without stalling
    FillVoiceHalf(v, 0);
    FillVoiceHalf(v, 1);

    v->voiceInUse = WAV_STATE_ACTIVE;

    if (!(v->playbackFlags & WAV_FLAG_PAUSED)) {
        PushTrackReport(v->fileIndex, (uint8_t)voiceIdx, 0x01);
        PushEvent(WAV_EVT_STARTED, v->fileIndex, v->voiceUniqueIndex, v->soundClass, 0, 0);
        if (soundClass == WAV_CLASS_MUSIC) {
            PushEvent(WAV_EVT_MUSIC_CHANGED, v->fileIndex, v->voiceUniqueIndex, WAV_CLASS_MUSIC, 0, 0);
        }
    }
}

static void EnqueueCallout(uint16_t trackNumber, uint8_t flags, int16_t gainDb, uint8_t priority) {
    int8_t slot = -1;
    uint8_t lowestPrio = priority;
    for (uint8_t i = 0; i < WAV_CALLOUT_QUEUE_SIZE; i++) {
        if (!CalloutQueue[i].inUse) {
            slot = (int8_t)i;
            break;
        }
    }
    // If queue is full, replace lowest-priority queued item if new one is higher
    if (slot == -1) {
        for (uint8_t i = 0; i < WAV_CALLOUT_QUEUE_SIZE; i++) {
            if (CalloutQueue[i].priority < lowestPrio) {
                lowestPrio = CalloutQueue[i].priority;
                slot = (int8_t)i;
            }
        }
    }
    if (slot != -1) {
        CalloutQueue[slot].inUse = 1;
        CalloutQueue[slot].trackNumber = trackNumber;
        CalloutQueue[slot].flags = flags;
        CalloutQueue[slot].gainDb = gainDb;
        CalloutQueue[slot].priority = priority;
    }
}

static void PopNextCallout(void) {
    if (IsClassPlaying(WAV_CLASS_CALLOUT)) return;

    int8_t bestIdx = -1;
    uint8_t bestPrio = 0;
    for (uint8_t i = 0; i < WAV_CALLOUT_QUEUE_SIZE; i++) {
        if (CalloutQueue[i].inUse) {
            if (bestIdx == -1 || CalloutQueue[i].priority > bestPrio) {
                bestPrio = CalloutQueue[i].priority;
                bestIdx = (int8_t)i;
            }
        }
    }
    if (bestIdx == -1) return;

    CalloutQueueEntry item = CalloutQueue[bestIdx];
    CalloutQueue[bestIdx].inUse = 0;

    FIL fil;
    if (!OpenTrackFile(item.trackNumber, &fil)) return;

    int8_t voiceIdx = AllocateVoiceSlot(WAV_CLASS_CALLOUT, item.priority);
    if (voiceIdx < 0) {
        f_close(&fil);
        return;
    }
    StartVoiceInternal(voiceIdx, &fil, item.trackNumber, WAV_CLASS_CALLOUT,
                       item.flags, item.gainDb, item.priority);
}

// --- Public API Implementation ---

void WAVFile_Init(void) {
    memset(Voices, 0, sizeof(Voices));
    memset(Classes, 0, sizeof(Classes));
    memset(DuckRules, 0, sizeof(DuckRules));
    memset(CalloutQueue, 0, sizeof(CalloutQueue));
    memset(EventQueue, 0, sizeof(EventQueue));

    for (uint8_t i = 0; i < MAX_VOICES; i++) {
        Voices[i].voiceInUse = WAV_STATE_FREE;
        Voices[i].endIndexB = 0xFFFFFFFF;
    }

    SystemVolumeQ16 = 0;
    ReportingEnabled = 0;
    EventQueueHead = 0;
    EventQueueTail = 0;
    NextTUID = 1;
    WAVCurrentRefillVoice = 0;
    CurrentSoundtrackFolder = 0;
    strcpy(ActiveDirPath, "/");
}

void WAVFile_RegisterCallbacks(WAVTrackReportCallback reportCb, WAVEventCallback eventCb) {
    TrackReportCb = reportCb;
    EventCb = eventCb;
}

void WAVFile_ScanSoundtracks(void) {
    DIR dir;
    FILINFO fno;
    AltSoundtrackCount = 0;

    if (f_opendir(&dir, "/") != FR_OK) return;

    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
        if (!(fno.fattrib & AM_DIR)) continue;
        if (CaseInsensitivePrefix(fno.fname, "AltSoundtrack")) {
            const char *numStr = fno.fname + 13;
            if (isdigit((unsigned char)numStr[0])) {
                long val = strtol(numStr, NULL, 10);
                if (val >= 1 && val <= 255 && AltSoundtrackCount < WAV_MAX_ALT_SOUNDTRACKS) {
                    AltSoundtrackNumbers[AltSoundtrackCount++] = (uint8_t)val;
                }
            }
        }
    }
    f_closedir(&dir);

    // Sort folder numbers numerically ascending
    for (uint8_t i = 0; i < AltSoundtrackCount; i++) {
        for (uint8_t j = (uint8_t)(i + 1); j < AltSoundtrackCount; j++) {
            if (AltSoundtrackNumbers[j] < AltSoundtrackNumbers[i]) {
                uint8_t tmp = AltSoundtrackNumbers[i];
                AltSoundtrackNumbers[i] = AltSoundtrackNumbers[j];
                AltSoundtrackNumbers[j] = tmp;
            }
        }
    }
}

void WAVFile_BuildClusterMap(void) {
    DIR dir;
    FILINFO fno;
    FIL tempFil;

    memset(FileMap, 0, sizeof(FileMap));
    TotalPlayableTracks = 0;

    if (f_opendir(&dir, ActiveDirPath) != FR_OK) return;

    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
        if (fno.fattrib & AM_DIR) continue;
        uint16_t id = 0;
        if (ParseTrackPrefix(fno.fname, &id)) {
            TotalPlayableTracks++;
            if (id <= FAST_TRACK_MAP_SIZE) {
                char fullPath[160];
                if (ActiveDirPath[0] == '/' && ActiveDirPath[1] == '\0') {
                    snprintf(fullPath, sizeof(fullPath), "/%.158s", fno.fname);
                } else {
                    snprintf(fullPath, sizeof(fullPath), "%.79s/%.79s", ActiveDirPath, fno.fname);
                }
                if (f_open(&tempFil, fullPath, FA_READ) == FR_OK) {
                    FileMap[id - 1].sclust = tempFil.obj.sclust;
                    FileMap[id - 1].fsize = (uint32_t)tempFil.obj.objsize;
                    if (MainFS == NULL) MainFS = tempFil.obj.fs;
                    f_close(&tempFil);
                }
            }
        }
    }
    f_closedir(&dir);
}

uint8_t WAVFile_GetSoundtrackCount(void) {
    return (uint8_t)(AltSoundtrackCount + 1);
}

uint8_t WAVFile_SetSoundtrackFolder(uint8_t folderNum) {
    if (folderNum == 0) {
        CurrentSoundtrackFolder = 0;
        strcpy(ActiveDirPath, "/");
        WAVFile_StopAllVoices();
        WAVFile_BuildClusterMap();
        return 1;
    }

    if (folderNum > AltSoundtrackCount) return 0;

    uint8_t targetNum = AltSoundtrackNumbers[folderNum - 1];
    DIR dir;
    FILINFO fno;
    uint8_t found = 0;

    if (f_opendir(&dir, "/") != FR_OK) return 0;
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
        if (!(fno.fattrib & AM_DIR)) continue;
        if (CaseInsensitivePrefix(fno.fname, "AltSoundtrack")) {
            long val = strtol(fno.fname + 13, NULL, 10);
            if (val == targetNum) {
                snprintf(ActiveDirPath, sizeof(ActiveDirPath), "/%.78s", fno.fname);
                CurrentSoundtrackFolder = folderNum;
                found = 1;
                break;
            }
        }
    }
    f_closedir(&dir);

    if (found) {
        WAVFile_StopAllVoices();
        WAVFile_BuildClusterMap();
    }
    return found;
}

uint16_t WAVFile_GetTrackCount(void) {
    return TotalPlayableTracks;
}

uint8_t WAVFile_GetMaxVoices(void) {
    return MAX_VOICES;
}

uint8_t ParseWavHeader(FIL *file, WAVFileVoice *v) {
    uint8_t buffer[12];
    UINT br = 0;

    if (f_read(file, buffer, 12, &br) != FR_OK || br != 12) return 0;
    if (buffer[0] != 'R' || buffer[1] != 'I' || buffer[2] != 'F' || buffer[3] != 'F') return 0;
    if (buffer[8] != 'W' || buffer[9] != 'A' || buffer[10] != 'V' || buffer[11] != 'E') return 0;

    uint32_t dataSize = 0;
    uint32_t numChannels = 0;
    uint32_t sampleRate = 0;
    uint16_t audioFormat = 0;
    uint16_t bitsPerSample = 0;

    while (f_read(file, buffer, 8, &br) == FR_OK && br == 8) {
        uint32_t chunkSize = (uint32_t)buffer[4] |
                             ((uint32_t)buffer[5] << 8) |
                             ((uint32_t)buffer[6] << 16) |
                             ((uint32_t)buffer[7] << 24);

        if (buffer[0] == 'f' && buffer[1] == 'm' && buffer[2] == 't' && buffer[3] == ' ') {
            uint8_t fmtData[16];
            if (f_read(file, fmtData, 16, &br) != FR_OK || br != 16) return 0;

            audioFormat = (uint16_t)(fmtData[0] | (fmtData[1] << 8));
            numChannels = (uint32_t)(fmtData[2] | (fmtData[3] << 8));
            sampleRate = (uint32_t)fmtData[4] |
                         ((uint32_t)fmtData[5] << 8) |
                         ((uint32_t)fmtData[6] << 16) |
                         ((uint32_t)fmtData[7] << 24);
            bitsPerSample = (uint16_t)(fmtData[14] | (fmtData[15] << 8));

            if (chunkSize > 16) {
                f_lseek(file, f_tell(file) + (chunkSize - 16));
            }
        } else if (buffer[0] == 'd' && buffer[1] == 'a' && buffer[2] == 't' && buffer[3] == 'a') {
            dataSize = chunkSize;
            break;
        } else {
            f_lseek(file, f_tell(file) + chunkSize);
        }
    }

    if (audioFormat != 1 || sampleRate != AUDIO_FREQ ||
        (numChannels != 1 && numChannels != 2) ||
        dataSize == 0 || bitsPerSample != 16) {
        return 0;
    }

    if (numChannels == 2) {
        v->playbackFlags |= WAV_FLAG_STEREO;
    } else {
        v->playbackFlags &= (uint16_t)(~WAV_FLAG_STEREO);
    }

    v->numSamples = dataSize / (numChannels * 2);
    return 1;
}

uint8_t WAVFile_StartVoice(FIL fileToPlay, uint8_t priority) {
    int8_t voiceIdx = AllocateVoiceSlot(WAV_CLASS_SFX, priority);
    if (voiceIdx < 0) return 0;
    StartVoiceInternal(voiceIdx, &fileToPlay, 0, WAV_CLASS_SFX, 0, 0, priority);
    return (Voices[voiceIdx].voiceInUse == WAV_STATE_ACTIVE) ? 1 : 0;
}

uint8_t WAVFile_StartFlashVoice(const uint8_t *dataPtr, uint32_t dataLen) {
    if (dataPtr == NULL || dataLen < 44) return 0;
    int8_t voiceIdx = AllocateVoiceSlot(WAV_CLASS_SFX, 255);
    if (voiceIdx < 0) return 0;

    WAVFileVoice *v = &Voices[voiceIdx];
    v->voiceInUse = WAV_STATE_FREE;
    v->isFlashSource = 1;
    v->fileIndex = 0;
    v->soundClass = WAV_CLASS_SFX;
    v->priority = 255;
    v->nextTrack = 0;
    v->playbackFlags = WAV_FLAG_STEREO;
    v->curSample = 0;
    v->readSample = 0;
    v->startIndexA = 0;
    v->endIndexB = 0xFFFFFFFF;
    v->numLoopsLeft = 0;
    v->refillLoopsLeft = 0;
    v->alertMarginSamples = 0;
    v->fadeInSamples = 0;
    v->fadeOutSamples = 0;
    v->currentGainQ16 = 0;
    v->targetGainQ16 = 0;
    v->fadeStepPerMsQ16 = 0;
    v->bufReadPos = 0;
    v->halfReady[0] = 0;
    v->halfReady[1] = 0;

    // Parse RIFF header in flash memory if present
    uint32_t pcmOffset = 0;
    uint32_t pcmLen = dataLen;
    if (dataPtr[0] == 'R' && dataPtr[1] == 'I' && dataPtr[2] == 'F' && dataPtr[3] == 'F' &&
        dataPtr[8] == 'W' && dataPtr[9] == 'A' && dataPtr[10] == 'V' && dataPtr[11] == 'E') {
        uint32_t pos = 12;
        uint16_t numChannels = 2;
        while (pos + 8 <= dataLen) {
            uint32_t chunkSize = (uint32_t)dataPtr[pos + 4] |
                                 ((uint32_t)dataPtr[pos + 5] << 8) |
                                 ((uint32_t)dataPtr[pos + 6] << 16) |
                                 ((uint32_t)dataPtr[pos + 7] << 24);
            if (dataPtr[pos] == 'f' && dataPtr[pos + 1] == 'm' &&
                dataPtr[pos + 2] == 't' && dataPtr[pos + 3] == ' ' && pos + 24 <= dataLen) {
                numChannels = (uint16_t)(dataPtr[pos + 10] | (dataPtr[pos + 11] << 8));
            } else if (dataPtr[pos] == 'd' && dataPtr[pos + 1] == 'a' &&
                       dataPtr[pos + 2] == 't' && dataPtr[pos + 3] == 'a') {
                pcmOffset = pos + 8;
                pcmLen = (pcmOffset + chunkSize <= dataLen) ? chunkSize : (dataLen - pcmOffset);
                break;
            }
            pos += 8 + chunkSize;
        }
        if (numChannels == 1) {
            v->playbackFlags &= (uint16_t)(~WAV_FLAG_STEREO);
        }
    }

    uint32_t bytesPerSample = (v->playbackFlags & WAV_FLAG_STEREO) ? 4 : 2;
    v->flashBase = dataPtr + pcmOffset;
    v->flashDataLen = pcmLen;
    v->numSamples = pcmLen / bytesPerSample;
    v->voiceUniqueIndex = NextTUID++;
    if (NextTUID == 0) NextTUID = 1;

    FillVoiceHalf(v, 0);
    FillVoiceHalf(v, 1);
    v->voiceInUse = WAV_STATE_ACTIVE;
    return 1;
}

uint8_t WAVFile_TrackControl(uint8_t controlCode, uint16_t trackNumber, uint8_t lockFlag) {
    switch (controlCode) {
        case WAV_CTRL_PLAY_SOLO: {
            WAVFile_StopAllVoices();
            uint8_t flags = lockFlag ? WAV_PLAYEX_LOCK : 0;
            return (WAVFile_PlayEx(trackNumber, WAV_CLASS_SFX, flags, 0, 128) != 0) ? 1 : 0;
        }
        case WAV_CTRL_PLAY_POLY: {
            uint8_t flags = lockFlag ? WAV_PLAYEX_LOCK : 0;
            return (WAVFile_PlayEx(trackNumber, WAV_CLASS_SFX, flags, 0, 128) != 0) ? 1 : 0;
        }
        case WAV_CTRL_LOAD: {
            uint8_t flags = WAV_PLAYEX_PAUSE | (lockFlag ? WAV_PLAYEX_LOCK : 0);
            return (WAVFile_PlayEx(trackNumber, WAV_CLASS_SFX, flags, 0, 128) != 0) ? 1 : 0;
        }
        case WAV_CTRL_PAUSE:
        case WAV_CTRL_RESUME:
        case WAV_CTRL_STOP:
        case WAV_CTRL_LOOP_ON:
        case WAV_CTRL_LOOP_OFF: {
            uint8_t affected = 0;
            for (uint8_t i = 0; i < MAX_VOICES; i++) {
                WAVFileVoice *v = &Voices[i];
                if (v->voiceInUse != WAV_STATE_ACTIVE || v->fileIndex != trackNumber) continue;
                affected = 1;
                if (controlCode == WAV_CTRL_PAUSE) {
                    v->playbackFlags |= WAV_FLAG_PAUSED;
                } else if (controlCode == WAV_CTRL_RESUME) {
                    v->playbackFlags &= (uint16_t)(~WAV_FLAG_PAUSED);
                } else if (controlCode == WAV_CTRL_STOP) {
                    v->voiceInUse = WAV_STATE_STOPPING;
                    PushTrackReport(v->fileIndex, i, 0x00);
                } else if (controlCode == WAV_CTRL_LOOP_ON) {
                    v->numLoopsLeft = 0xFF;
                    v->refillLoopsLeft = 0xFF;
                } else if (controlCode == WAV_CTRL_LOOP_OFF) {
                    v->numLoopsLeft = 0;
                    v->refillLoopsLeft = 0;
                }
            }
            return affected;
        }
        default:
            return 0;
    }
}

void WAVFile_StopAllVoices(void) {
    for (uint8_t i = 0; i < MAX_VOICES; i++) {
        if (Voices[i].voiceInUse != WAV_STATE_FREE) {
            if (Voices[i].voiceInUse == WAV_STATE_ACTIVE) {
                PushTrackReport(Voices[i].fileIndex, i, 0x00);
            }
            if (!Voices[i].isFlashSource) {
                f_close(&Voices[i].fil);
            }
            Voices[i].voiceInUse = WAV_STATE_FREE;
        }
    }
    memset(CalloutQueue, 0, sizeof(CalloutQueue));
}

void WAVFile_SetSystemVolume(int16_t gainDb) {
    SystemVolumeQ16 = ClampDbQ16((((int32_t)gainDb) * 65536));
}

uint8_t WAVFile_GetPlayingTracks(uint16_t *outTracks, uint8_t maxOut) {
    uint8_t count = 0;
    for (uint8_t i = 0; i < MAX_VOICES && count < maxOut; i++) {
        if (Voices[i].voiceInUse == WAV_STATE_ACTIVE && Voices[i].fileIndex > 0) {
            outTracks[count++] = Voices[i].fileIndex;
        }
    }
    return count;
}

void WAVFile_SetTrackVolume(uint16_t trackNumber, int16_t gainDb) {
    int32_t gainQ16 = ClampDbQ16((((int32_t)gainDb) * 65536));
    for (uint8_t i = 0; i < MAX_VOICES; i++) {
        if (Voices[i].voiceInUse == WAV_STATE_ACTIVE && Voices[i].fileIndex == trackNumber) {
            Voices[i].currentGainQ16 = gainQ16;
            Voices[i].targetGainQ16 = gainQ16;
            Voices[i].fadeStepPerMsQ16 = 0;
        }
    }
}

void WAVFile_FadeTrack(uint16_t trackNumber, int16_t targetDb, uint16_t fadeTimeMs, uint8_t stopFlag) {
    int32_t targetQ16 = ClampDbQ16((((int32_t)targetDb) * 65536));
    for (uint8_t i = 0; i < MAX_VOICES; i++) {
        WAVFileVoice *v = &Voices[i];
        if (v->voiceInUse == WAV_STATE_ACTIVE && v->fileIndex == trackNumber) {
            if (fadeTimeMs == 0) {
                v->currentGainQ16 = targetQ16;
                v->targetGainQ16 = targetQ16;
                v->fadeStepPerMsQ16 = 0;
                if (stopFlag) {
                    v->voiceInUse = WAV_STATE_STOPPING;
                    PushTrackReport(v->fileIndex, i, 0x00);
                }
            } else {
                v->targetGainQ16 = targetQ16;
                v->fadeStepPerMsQ16 = (targetQ16 - v->currentGainQ16) / (int32_t)fadeTimeMs;
                if (v->fadeStepPerMsQ16 == 0 && targetQ16 != v->currentGainQ16) {
                    v->fadeStepPerMsQ16 = (targetQ16 > v->currentGainQ16) ? 1 : -1;
                }
                if (stopFlag) {
                    v->playbackFlags |= WAV_FLAG_STOP_AFTER_FADE;
                } else {
                    v->playbackFlags &= (uint16_t)(~WAV_FLAG_STOP_AFTER_FADE);
                }
            }
        }
    }
}

void WAVFile_SetReporting(uint8_t enabled) {
    ReportingEnabled = enabled ? 1 : 0;
}

uint32_t WAVFile_PlayEx(uint16_t trackNumber, uint8_t soundClass, uint8_t flags,
                        int16_t gainDb, uint8_t priority) {
    if (soundClass >= WAV_NUM_CLASSES) return 0;

    // Callout priority handling (when not paused/staged)
    if (soundClass == WAV_CLASS_CALLOUT && !(flags & WAV_PLAYEX_PAUSE)) {
        for (uint8_t i = 0; i < MAX_VOICES; i++) {
            WAVFileVoice *v = &Voices[i];
            if (v->voiceInUse == WAV_STATE_ACTIVE && v->soundClass == WAV_CLASS_CALLOUT &&
                !(v->playbackFlags & WAV_FLAG_PAUSED)) {
                if (v->priority >= priority) {
                    EnqueueCallout(trackNumber, flags, gainDb, priority);
                    return 0;
                } else {
                    v->voiceInUse = WAV_STATE_STOPPING;
                    PushTrackReport(v->fileIndex, i, 0x00);
                }
            }
        }
    }

    // Replace earlier instance of same track if requested
    if (flags & WAV_PLAYEX_REPLACE_EARLIER) {
        for (uint8_t i = 0; i < MAX_VOICES; i++) {
            if (Voices[i].voiceInUse == WAV_STATE_ACTIVE && Voices[i].fileIndex == trackNumber) {
                Voices[i].voiceInUse = WAV_STATE_STOPPING;
                PushTrackReport(Voices[i].fileIndex, i, 0x00);
            }
        }
    }

    // Music always replaces active music (or crossfades if 0x10 is set); solo-in-class kills same class
    if ((flags & WAV_PLAYEX_SOLO_IN_CLASS) ||
        (soundClass == WAV_CLASS_MUSIC && !(flags & WAV_PLAYEX_PAUSE))) {
        for (uint8_t i = 0; i < MAX_VOICES; i++) {
            WAVFileVoice *v = &Voices[i];
            if (v->voiceInUse == WAV_STATE_ACTIVE && v->soundClass == soundClass &&
                !(v->playbackFlags & WAV_FLAG_PAUSED)) {
                if ((flags & WAV_PLAYEX_CROSSFADE) && (flags & WAV_PLAYEX_SOLO_IN_CLASS)) {
                    // Default 500ms crossfade out
                    int32_t targetQ16 = (WAV_GAIN_MIN_DB * 65536);
                    v->targetGainQ16 = targetQ16;
                    v->fadeStepPerMsQ16 = (targetQ16 - v->currentGainQ16) / 500;
                    v->playbackFlags |= (WAV_FLAG_STOP_AFTER_FADE | WAV_FLAG_CROSSFADE_OUT);
                } else {
                    v->voiceInUse = WAV_STATE_STOPPING;
                    PushTrackReport(v->fileIndex, i, 0x00);
                }
            }
        }
    }

    FIL fil;
    if (!OpenTrackFile(trackNumber, &fil)) return 0;

    int8_t voiceIdx = AllocateVoiceSlot(soundClass, priority);
    if (voiceIdx < 0) {
        f_close(&fil);
        return 0;
    }

    StartVoiceInternal(voiceIdx, &fil, trackNumber, soundClass, flags, gainDb, priority);
    return (Voices[voiceIdx].voiceInUse == WAV_STATE_ACTIVE) ? Voices[voiceIdx].voiceUniqueIndex : 0;
}

void WAVFile_SetClassGain(uint8_t soundClass, int16_t gainDb, uint16_t fadeTimeMs) {
    if (soundClass >= WAV_NUM_CLASSES) return;
    int32_t targetQ16 = ClampDbQ16((((int32_t)gainDb) * 65536));
    ClassState *cs = &Classes[soundClass];

    if (fadeTimeMs == 0) {
        cs->currentGainQ16 = targetQ16;
        cs->targetGainQ16 = targetQ16;
        cs->fadeStepPerMsQ16 = 0;
    } else {
        cs->targetGainQ16 = targetQ16;
        cs->fadeStepPerMsQ16 = (targetQ16 - cs->currentGainQ16) / (int32_t)fadeTimeMs;
        if (cs->fadeStepPerMsQ16 == 0 && targetQ16 != cs->currentGainQ16) {
            cs->fadeStepPerMsQ16 = (targetQ16 > cs->currentGainQ16) ? 1 : -1;
        }
    }
}

void WAVFile_SetDuckRule(uint8_t duckedClass, uint8_t duckingClass, int8_t gainDb,
                         uint16_t attackMs, uint16_t releaseMs) {
    if (duckedClass >= WAV_NUM_CLASSES || duckingClass >= WAV_NUM_CLASSES) return;
    DuckRule *r = &DuckRules[duckedClass][duckingClass];
    r->enabled = (gainDb != 0) ? 1 : 0;
    r->attenuationDb = gainDb;
    r->attackMs = attackMs;
    r->releaseMs = releaseMs;
}

void WAVFile_SetLoopPoints(uint8_t flags, uint8_t loopCount, uint32_t pointA, uint32_t pointB,
                           uint16_t fadeInMs, uint16_t fadeOutMs, uint16_t alertMarginMs,
                           uint16_t nextTrack, uint32_t trackOrTuid) {
    uint8_t useTuid = (flags & WAV_LOOP_USE_TUID) ? 1 : 0;
    uint8_t countOnly = (flags & WAV_LOOP_UPDATE_COUNT_ONLY) ? 1 : 0;
    uint8_t nextOnly = (flags & WAV_LOOP_UPDATE_NEXT_ONLY) ? 1 : 0;

    for (uint8_t i = 0; i < MAX_VOICES; i++) {
        WAVFileVoice *v = &Voices[i];
        if (v->voiceInUse != WAV_STATE_ACTIVE) continue;

        if (useTuid) {
            if (v->voiceUniqueIndex != trackOrTuid) continue;
        } else {
            if (v->fileIndex != (uint16_t)trackOrTuid) continue;
        }

        if (countOnly || nextOnly) {
            if (countOnly) {
                v->numLoopsLeft = loopCount;
                v->refillLoopsLeft = loopCount;
            }
            if (nextOnly) {
                v->nextTrack = nextTrack;
            }
        } else {
            v->numLoopsLeft = loopCount;
            v->refillLoopsLeft = loopCount;
            v->startIndexA = (pointA < v->numSamples) ? pointA : 0;
            v->endIndexB = pointB;
            v->fadeInSamples = ((uint32_t)fadeInMs * AUDIO_FREQ) / 1000;
            v->fadeOutSamples = ((uint32_t)fadeOutMs * AUDIO_FREQ) / 1000;
            v->alertMarginSamples = ((uint32_t)alertMarginMs * AUDIO_FREQ) / 1000;
            v->nextTrack = nextTrack;

            if (flags & WAV_LOOP_PLAY_THROUGH_END) {
                v->playbackFlags |= WAV_FLAG_PLAY_THROUGH_END;
            } else {
                v->playbackFlags &= (uint16_t)(~WAV_FLAG_PLAY_THROUGH_END);
            }

            if (flags & WAV_LOOP_ALERT_EACH_LOOP) {
                v->playbackFlags |= WAV_FLAG_ALERT_EACH_LOOP;
            } else {
                v->playbackFlags &= (uint16_t)(~WAV_FLAG_ALERT_EACH_LOOP);
            }
            v->playbackFlags &= (uint16_t)(~WAV_FLAG_ALERT_SENT);

            // Seek to start at A if WAV_LOOP_START_AT_TOP is clear and starting playback
            if ((flags & WAV_LOOP_START_PLAYBACK) && !(flags & WAV_LOOP_START_AT_TOP) && v->startIndexA > 0) {
                uint32_t bytesPerSample = (v->playbackFlags & WAV_FLAG_STEREO) ? 4 : 2;
                v->curSample = v->startIndexA;
                v->readSample = v->startIndexA;
                v->bufReadPos = 0;
                if (!v->isFlashSource) {
                    f_lseek(&v->fil, v->dataStartOffset + (v->startIndexA * bytesPerSample));
                }
                FillVoiceHalf(v, 0);
                FillVoiceHalf(v, 1);
            }

            if (flags & WAV_LOOP_START_PLAYBACK) {
                uint8_t wasPaused = (v->playbackFlags & WAV_FLAG_PAUSED) ? 1 : 0;
                v->playbackFlags &= (uint16_t)(~WAV_FLAG_PAUSED);
                v->playbackFlags &= (uint16_t)(~WAV_FLAG_WAIT_FOR_BAR);
                if (wasPaused) {
                    PushTrackReport(v->fileIndex, i, 0x01);
                    PushEvent(WAV_EVT_STARTED, v->fileIndex, v->voiceUniqueIndex, v->soundClass, 0, 0);
                    if (v->soundClass == WAV_CLASS_MUSIC) {
                        PushEvent(WAV_EVT_MUSIC_CHANGED, v->fileIndex, v->voiceUniqueIndex, WAV_CLASS_MUSIC, 0, 0);
                    }
                }
            }
        }

        if (useTuid) break;
    }
}

void WAVFile_StopClass(uint8_t soundClass, uint16_t fadeTimeMs) {
    if (soundClass >= WAV_NUM_CLASSES) return;

    if (soundClass == WAV_CLASS_CALLOUT) {
        memset(CalloutQueue, 0, sizeof(CalloutQueue));
    }

    for (uint8_t i = 0; i < MAX_VOICES; i++) {
        WAVFileVoice *v = &Voices[i];
        if (v->voiceInUse != WAV_STATE_ACTIVE || v->soundClass != soundClass) continue;

        if (fadeTimeMs == 0 || (v->playbackFlags & WAV_FLAG_PAUSED)) {
            v->voiceInUse = WAV_STATE_STOPPING;
            PushTrackReport(v->fileIndex, i, 0x00);
        } else {
            int32_t targetQ16 = (WAV_GAIN_MIN_DB * 65536);
            v->targetGainQ16 = targetQ16;
            v->fadeStepPerMsQ16 = (targetQ16 - v->currentGainQ16) / (int32_t)fadeTimeMs;
            if (v->fadeStepPerMsQ16 == 0) v->fadeStepPerMsQ16 = -1;
            v->playbackFlags |= WAV_FLAG_STOP_AFTER_FADE;
        }
    }
}

void WAVFile_SetEventFlags(uint8_t mask, uint8_t soundClass) {
    if (soundClass >= WAV_NUM_CLASSES) return;
    ClassEventMask[soundClass] = mask;
}

void WAVFile_RefillVoices(void) {
    // Clean up any voices marked for stopping and advance callout queue
    uint8_t calloutEnded = 0;
    for (uint8_t i = 0; i < MAX_VOICES; i++) {
        if (Voices[i].voiceInUse == WAV_STATE_STOPPING) {
            if (Voices[i].soundClass == WAV_CLASS_CALLOUT) calloutEnded = 1;
            if (!Voices[i].isFlashSource) {
                f_close(&Voices[i].fil);
            }
            Voices[i].voiceInUse = WAV_STATE_FREE;
        }
    }
    if (calloutEnded) {
        PopNextCallout();
    }

    // Refill one voice half per call to keep main loop responsive
    for (uint8_t attempt = 0; attempt < MAX_VOICES; attempt++) {
        uint8_t vIdx = (uint8_t)WAVCurrentRefillVoice;
        WAVCurrentRefillVoice = (WAVCurrentRefillVoice + 1) % MAX_VOICES;

        WAVFileVoice *v = &Voices[vIdx];
        if (v->voiceInUse != WAV_STATE_ACTIVE) continue;

        if (!v->halfReady[0]) {
            FillVoiceHalf(v, 0);
            return;
        }
        if (!v->halfReady[1]) {
            FillVoiceHalf(v, 1);
            return;
        }
    }
}

void WAVFile_UpdateFades(uint32_t elapsedMs) {
    if (elapsedMs == 0) return;

    // 1. Update per-voice fades
    for (uint8_t i = 0; i < MAX_VOICES; i++) {
        WAVFileVoice *v = &Voices[i];
        if (v->voiceInUse != WAV_STATE_ACTIVE || v->fadeStepPerMsQ16 == 0) continue;

        int32_t delta = v->fadeStepPerMsQ16 * (int32_t)elapsedMs;
        v->currentGainQ16 += delta;

        uint8_t reached = 0;
        if (v->fadeStepPerMsQ16 > 0 && v->currentGainQ16 >= v->targetGainQ16) reached = 1;
        if (v->fadeStepPerMsQ16 < 0 && v->currentGainQ16 <= v->targetGainQ16) reached = 1;

        if (reached) {
            v->currentGainQ16 = v->targetGainQ16;
            v->fadeStepPerMsQ16 = 0;
            if (v->playbackFlags & WAV_FLAG_STOP_AFTER_FADE) {
                v->voiceInUse = WAV_STATE_STOPPING;
                PushTrackReport(v->fileIndex, i, 0x00);
                PushEvent(WAV_EVT_TRACK_FINISHED, v->fileIndex, v->voiceUniqueIndex, v->soundClass, 0, 0);
            }
        }
    }

    // 2. Update per-class gain fades
    for (uint8_t c = 0; c < WAV_NUM_CLASSES; c++) {
        ClassState *cs = &Classes[c];
        if (cs->fadeStepPerMsQ16 != 0) {
            cs->currentGainQ16 += cs->fadeStepPerMsQ16 * (int32_t)elapsedMs;
            if ((cs->fadeStepPerMsQ16 > 0 && cs->currentGainQ16 >= cs->targetGainQ16) ||
                (cs->fadeStepPerMsQ16 < 0 && cs->currentGainQ16 <= cs->targetGainQ16)) {
                cs->currentGainQ16 = cs->targetGainQ16;
                cs->fadeStepPerMsQ16 = 0;
            }
        }
    }

    // 3. Update ducking rules and ducking ramps
    uint8_t classActive[WAV_NUM_CLASSES];
    for (uint8_t c = 0; c < WAV_NUM_CLASSES; c++) {
        classActive[c] = IsClassPlaying(c);
    }


    for (uint8_t ducked = 0; ducked < WAV_NUM_CLASSES; ducked++) {
        int32_t targetDuckQ16 = 0;
        int32_t releaseAttenQ16 = 0;
        uint16_t attackMs = 0;
        uint16_t releaseMs = 0;
        uint8_t shouldDuck = 0;

        for (uint8_t ducking = 0; ducking < WAV_NUM_CLASSES; ducking++) {
            DuckRule *r = &DuckRules[ducked][ducking];
            if (r->enabled && classActive[ducking]) {
                int32_t attenQ16 = (((int32_t)r->attenuationDb) * 65536);
                if (!shouldDuck || attenQ16 > targetDuckQ16) {
                    targetDuckQ16 = attenQ16;
                    attackMs = r->attackMs;
                }
                shouldDuck = 1;
            } else if (r->enabled && r->releaseMs > releaseMs) {
                releaseMs = r->releaseMs;
                releaseAttenQ16 = (((int32_t)r->attenuationDb) * 65536);
            }
        }

        ClassState *cs = &Classes[ducked];
        if (shouldDuck != cs->isDucked) {
            cs->isDucked = shouldDuck;
            PushEvent(WAV_EVT_DUCKING_CHANGE, 0, 0, ducked, 1, shouldDuck);
        }

        if (cs->currentDuckQ16 != targetDuckQ16) {
            if (targetDuckQ16 > cs->currentDuckQ16) {
                if (attackMs == 0) {
                    cs->currentDuckQ16 = targetDuckQ16;
                } else {
                    int32_t step = (targetDuckQ16 / (int32_t)attackMs) * (int32_t)elapsedMs;
                    if (step == 0) step = 1;
                    cs->currentDuckQ16 += step;
                    if (cs->currentDuckQ16 > targetDuckQ16) cs->currentDuckQ16 = targetDuckQ16;
                }
            } else {
                if (releaseMs == 0 || releaseAttenQ16 == 0) {
                    cs->currentDuckQ16 = targetDuckQ16;
                } else {
                    int32_t step = (releaseAttenQ16 / (int32_t)releaseMs) * (int32_t)elapsedMs;
                    if (step == 0) step = 1;
                    cs->currentDuckQ16 -= step;
                    if (cs->currentDuckQ16 < targetDuckQ16) cs->currentDuckQ16 = targetDuckQ16;
                }
            }
        }
    }

}

void WAVFile_ProcessEvents(void) {
    while (EventQueueTail != EventQueueHead) {
        QueuedEvent e = EventQueue[EventQueueTail];
        EventQueueTail = (uint8_t)((EventQueueTail + 1) % WAV_EVENT_QUEUE_SIZE);
        if (e.isReport) {
            if (TrackReportCb != NULL) {
                TrackReportCb(e.trackNumber, e.voiceIndex, e.eventData);
            }
        } else {
            if (EventCb != NULL) {
                EventCb(e.eventId, e.trackNumber, e.tuid, e.soundClass, e.hasData, e.eventData);
            }
        }
    }
}

uint8_t WAVFile_GetNextSample(int32_t *leftChannel, int32_t *rightChannel) {
    int32_t retLeft = 0;
    int32_t retRight = 0;

    for (uint8_t voiceNum = 0; voiceNum < MAX_VOICES; voiceNum++) {
        WAVFileVoice *v = &Voices[voiceNum];
        if (v->voiceInUse != WAV_STATE_ACTIVE) continue;
        if (v->playbackFlags & (WAV_FLAG_PAUSED | WAV_FLAG_WAIT_FOR_BAR)) continue;

        uint8_t curHalf = (v->bufReadPos >= VOICE_BUF_HALF) ? 1 : 0;
        if (!v->halfReady[curHalf]) continue;

        uint8_t isStereo = (v->playbackFlags & WAV_FLAG_STEREO) ? 1 : 0;
        uint16_t bytesPerSample = isStereo ? 4 : 2;

        int16_t *samples = (int16_t *)(v->buffer + v->bufReadPos);
        int32_t leftSample = samples[0];
        int32_t rightSample = isStereo ? samples[1] : samples[0];

        // Combine voice gain, class gain, ducking attenuation, and system volume
        ClassState *cs = &Classes[v->soundClass];
        int32_t totalGainQ16 = v->currentGainQ16 + cs->currentGainQ16 - cs->currentDuckQ16 + SystemVolumeQ16;
        uint32_t linearQ14 = GainQ16ToLinearQ14(totalGainQ16);

        // Apply optional loop fade-in / fade-out envelope
        uint32_t endPoint = v->numSamples;
        if (v->endIndexB != 0xFFFFFFFF && v->endIndexB > 0 && v->endIndexB < v->numSamples) {
            endPoint = v->endIndexB;
        }
        if (v->fadeInSamples > 0 && v->curSample >= v->startIndexA &&
            (v->curSample - v->startIndexA) < v->fadeInSamples) {
            uint32_t pos = v->curSample - v->startIndexA;
            linearQ14 = (linearQ14 * pos) / v->fadeInSamples;
        }
        if (v->fadeOutSamples > 0 && v->curSample < endPoint &&
            (endPoint - v->curSample) < v->fadeOutSamples) {
            uint32_t rem = endPoint - v->curSample;
            linearQ14 = (linearQ14 * rem) / v->fadeOutSamples;
        }

        retLeft += (leftSample * (int32_t)linearQ14) >> 14;
        retRight += (rightSample * (int32_t)linearQ14) >> 14;

        // Advance ping-pong buffer read pointer
        uint8_t prevHalf = curHalf;
        v->bufReadPos = (uint16_t)((v->bufReadPos + bytesPerSample) & VOICE_BUF_SIZE_MASK);
        uint8_t nextHalf = (v->bufReadPos >= VOICE_BUF_HALF) ? 1 : 0;
        if (nextHalf != prevHalf) {
            v->halfReady[prevHalf] = 0;
        }

        v->curSample++;

        // Check loop alert margin before reaching B
        if (v->alertMarginSamples > 0 && !(v->playbackFlags & WAV_FLAG_ALERT_SENT)) {
            uint32_t alertPos = (endPoint > v->alertMarginSamples) ? (endPoint - v->alertMarginSamples) : 0;
            if (v->curSample >= alertPos) {
                uint8_t shouldAlert = (v->playbackFlags & WAV_FLAG_ALERT_EACH_LOOP) || (v->numLoopsLeft <= 1);
                if (shouldAlert) {
                    PushEvent(WAV_EVT_LOOPING, v->fileIndex, v->voiceUniqueIndex, v->soundClass, 1, v->numLoopsLeft);
                }
                v->playbackFlags |= WAV_FLAG_ALERT_SENT;
            }
        }

        // Check end of loop / end of file
        uint32_t stopSample = (v->numLoopsLeft > 0) ? endPoint :
            ((v->playbackFlags & WAV_FLAG_PLAY_THROUGH_END) ? v->numSamples : endPoint);

        if (v->curSample >= stopSample) {
            if (v->numLoopsLeft > 0) {
                if (v->numLoopsLeft != 0xFF) {
                    v->numLoopsLeft--;
                }
                v->curSample = (v->startIndexA < v->numSamples) ? v->startIndexA : 0;
                v->playbackFlags &= (uint16_t)(~WAV_FLAG_ALERT_SENT);
                if (v->soundClass == WAV_CLASS_MUSIC) {
                    TriggerMusicBarWaiters();
                }
            } else {
                v->voiceInUse = WAV_STATE_STOPPING;
                PushTrackReport(v->fileIndex, voiceNum, 0x00);
                PushEvent(WAV_EVT_TRACK_FINISHED, v->fileIndex, v->voiceUniqueIndex, v->soundClass, 0, 0);
                if (v->nextTrack != 0) {
                    TriggerNextTrack(v->nextTrack);
                }
            }
        }
    }

    *leftChannel = retLeft;
    *rightChannel = retRight;
    return 1;
}


void WAVFile_MixBlock(int32_t *mixBuffer, uint32_t stereoFrames) {
    memset(mixBuffer, 0, stereoFrames * 2 * sizeof(int32_t));

    for (uint8_t voiceNum = 0; voiceNum < MAX_VOICES; voiceNum++) {
        WAVFileVoice *v = &Voices[voiceNum];
        if (v->voiceInUse != WAV_STATE_ACTIVE) continue;
        if (v->playbackFlags & (WAV_FLAG_PAUSED | WAV_FLAG_WAIT_FOR_BAR)) continue;

        uint8_t isStereo = (v->playbackFlags & WAV_FLAG_STEREO) ? 1 : 0;
        uint8_t shiftBytes = isStereo ? 2 : 1;

        // Compute linear Q14 gain ONCE per voice per DMA block
        ClassState *cs = &Classes[v->soundClass];
        int32_t totalGainQ16 = v->currentGainQ16 + cs->currentGainQ16 - cs->currentDuckQ16 + SystemVolumeQ16;
        int32_t baseGainQ14 = (int32_t)GainQ16ToLinearQ14(totalGainQ16);

        uint32_t endPoint = v->numSamples;
        if (v->endIndexB != 0xFFFFFFFF && v->endIndexB > 0 && v->endIndexB < v->numSamples) {
            endPoint = v->endIndexB;
        }

        uint32_t framesLeft = stereoFrames;
        int32_t *mixPtr = mixBuffer;

        while (framesLeft > 0) {
            uint8_t curHalf = (v->bufReadPos >= VOICE_BUF_HALF) ? 1 : 0;
            if (!v->halfReady[curHalf]) break;

            uint32_t halfEndByte = curHalf ? VOICE_BUF_SIZE : VOICE_BUF_HALF;
            uint32_t framesInHalf = (halfEndByte - v->bufReadPos) >> shiftBytes;

            uint32_t stopSample = (v->numLoopsLeft > 0) ? endPoint :
                ((v->playbackFlags & WAV_FLAG_PLAY_THROUGH_END) ? v->numSamples : endPoint);
            uint32_t framesToStop = (v->curSample < stopSample) ? (stopSample - v->curSample) : 0;

            uint32_t chunk = framesLeft;
            if (chunk > framesInHalf) chunk = framesInHalf;
            if (chunk > framesToStop) chunk = framesToStop;

            if (chunk > 0) {
                int16_t *src = (int16_t *)(v->buffer + v->bufReadPos);

                uint8_t inFadeIn = (v->fadeInSamples > 0 &&
                                    v->curSample < (v->startIndexA + v->fadeInSamples) &&
                                    (v->curSample + chunk) > v->startIndexA);
                uint32_t fadeOutStart = (endPoint > v->fadeOutSamples) ? (endPoint - v->fadeOutSamples) : 0;
                uint8_t inFadeOut = (v->fadeOutSamples > 0 &&
                                     v->curSample < endPoint &&
                                     (v->curSample + chunk) > fadeOutStart);

                if (!inFadeIn && !inFadeOut) {
                    // Fast path: no per-sample envelope math
                    if (isStereo) {
                        for (uint32_t i = 0; i < chunk; i++) {
                            mixPtr[0] += ((int32_t)src[0] * baseGainQ14) >> 14;
                            mixPtr[1] += ((int32_t)src[1] * baseGainQ14) >> 14;
                            src += 2;
                            mixPtr += 2;
                        }
                    } else {
                        for (uint32_t i = 0; i < chunk; i++) {
                            int32_t s = ((int32_t)(*src++) * baseGainQ14) >> 14;
                            mixPtr[0] += s;
                            mixPtr[1] += s;
                            mixPtr += 2;
                        }
                    }
                } else {
                    // Envelope path: only runs during loop fade-in/fade-out windows
                    uint32_t sampleIdx = v->curSample;
                    for (uint32_t i = 0; i < chunk; i++, sampleIdx++) {
                        int32_t g = baseGainQ14;
                        if (v->fadeInSamples > 0 && sampleIdx >= v->startIndexA &&
                            (sampleIdx - v->startIndexA) < v->fadeInSamples) {
                            g = (int32_t)(((uint32_t)g * (sampleIdx - v->startIndexA)) / v->fadeInSamples);
                        }
                        if (v->fadeOutSamples > 0 && sampleIdx < endPoint &&
                            (endPoint - sampleIdx) < v->fadeOutSamples) {
                            g = (int32_t)(((uint32_t)g * (endPoint - sampleIdx)) / v->fadeOutSamples);
                        }
                        if (isStereo) {
                            mixPtr[0] += ((int32_t)src[0] * g) >> 14;
                            mixPtr[1] += ((int32_t)src[1] * g) >> 14;
                            src += 2;
                        } else {
                            int32_t s = ((int32_t)(*src++) * g) >> 14;
                            mixPtr[0] += s;
                            mixPtr[1] += s;
                        }
                        mixPtr += 2;
                    }
                }

                v->curSample += chunk;
                v->bufReadPos = (uint16_t)((v->bufReadPos + (chunk << shiftBytes)) & VOICE_BUF_SIZE_MASK);
                framesLeft -= chunk;

                if (chunk == framesInHalf) {
                    v->halfReady[curHalf] = 0;
                }
            }

            // Check loop alert margin once per chunk
            if (v->alertMarginSamples > 0 && !(v->playbackFlags & WAV_FLAG_ALERT_SENT)) {
                uint32_t alertPos = (endPoint > v->alertMarginSamples) ? (endPoint - v->alertMarginSamples) : 0;
                if (v->curSample >= alertPos) {
                    uint8_t shouldAlert = (v->playbackFlags & WAV_FLAG_ALERT_EACH_LOOP) || (v->numLoopsLeft <= 1);
                    if (shouldAlert) {
                        PushEvent(WAV_EVT_LOOPING, v->fileIndex, v->voiceUniqueIndex, v->soundClass, 1, v->numLoopsLeft);
                    }
                    v->playbackFlags |= WAV_FLAG_ALERT_SENT;
                }
            }

            // Check end of loop / end of file
            if (v->curSample >= stopSample) {
                if (v->numLoopsLeft > 0) {
                    if (v->numLoopsLeft != 0xFF) {
                        v->numLoopsLeft--;
                    }
                    v->curSample = (v->startIndexA < v->numSamples) ? v->startIndexA : 0;
                    v->playbackFlags &= (uint16_t)(~WAV_FLAG_ALERT_SENT);
                    if (v->soundClass == WAV_CLASS_MUSIC) {
                        TriggerMusicBarWaiters();
                    }
                } else {
                    v->voiceInUse = WAV_STATE_STOPPING;
                    PushTrackReport(v->fileIndex, voiceNum, 0x00);
                    PushEvent(WAV_EVT_TRACK_FINISHED, v->fileIndex, v->voiceUniqueIndex, v->soundClass, 0, 0);
                    if (v->nextTrack != 0) {
                        TriggerNextTrack(v->nextTrack);
                    }
                    break;
                }
            }
        }
    }
}


WAVFileVoice *WAVFile_GetVoiceSlot(uint8_t slotNum) {
    if (slotNum < MAX_VOICES) return &Voices[slotNum];
    return NULL;
}
