#include "WFPSerial.h"
#include "WAVFilePlayer.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------
// Platform-Specific Includes & UART Hardware Abstraction
// ---------------------------------------------------------
#if defined(ARDUINO_ARCH_STM32)
// Arduino STM32duino bridge (implemented in .ino/.cpp using HardwareSerial)
extern void WFPPlatform_UartInit(uint32_t baudRate);
extern bool WFPPlatform_UartIsWritable(void);
extern void WFPPlatform_UartWriteByte(uint8_t byte);
extern bool WFPPlatform_UartIsReadable(void);
extern uint8_t WFPPlatform_UartReadByte(void);

static void PlatformUartInit(uint32_t baudRate) {
    WFPPlatform_UartInit(baudRate);
}

static inline bool PlatformUartIsWritable(void) {
    return WFPPlatform_UartIsWritable();
}

static inline void PlatformUartWriteByte(uint8_t byte) {
    WFPPlatform_UartWriteByte(byte);
}

static inline bool PlatformUartIsReadable(void) {
    return WFPPlatform_UartIsReadable();
}

static inline uint8_t PlatformUartReadByte(void) {
    return WFPPlatform_UartReadByte();
}

#elif defined(WFP_PLATFORM_STM32)
#include <stm32f4xx_hal.h>

static UART_HandleTypeDef Huart1;

// Interrupt-driven RX ring buffer (STM32F4 USART has a 1-byte hardware RDR)
#define RX_RING_SIZE 256
static volatile uint8_t rxRingBuffer[RX_RING_SIZE];
static volatile uint16_t rxRingHead = 0;
static volatile uint16_t rxRingTail = 0;

void USART1_IRQHandler(void) {
    uint32_t sr = USART1->SR;
    if (sr & USART_SR_RXNE) {
        uint8_t byte = (uint8_t)(USART1->DR & 0xFF);
        uint16_t nextHead = (uint16_t)((rxRingHead + 1) % RX_RING_SIZE);
        if (nextHead != rxRingTail) {
            rxRingBuffer[rxRingHead] = byte;
            rxRingHead = nextHead;
        }
    }
    // Clear overrun error if set so RX does not stall
    if (sr & USART_SR_ORE) {
        volatile uint32_t tmp = USART1->DR;
        (void)tmp;
    }
}

static void PlatformUartInit(uint32_t baudRate) {
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();

    // PA9 = USART1_TX, PA10 = USART1_RX
    GPIO_InitTypeDef gpio;
    memset(&gpio, 0, sizeof(gpio));
    gpio.Pin = GPIO_PIN_9 | GPIO_PIN_10;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOA, &gpio);

    memset(&Huart1, 0, sizeof(Huart1));
    Huart1.Instance = USART1;
    Huart1.Init.BaudRate = baudRate;
    Huart1.Init.WordLength = UART_WORDLENGTH_8B;
    Huart1.Init.StopBits = UART_STOPBITS_1;
    Huart1.Init.Parity = UART_PARITY_NONE;
    Huart1.Init.Mode = UART_MODE_TX_RX;
    Huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    Huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    HAL_UART_Init(&Huart1);

    // Enable RXNE interrupt
    __HAL_UART_ENABLE_IT(&Huart1, UART_IT_RXNE);
    HAL_NVIC_SetPriority(USART1_IRQn, 2, 0);
    HAL_NVIC_EnableIRQ(USART1_IRQn);
}

static inline bool PlatformUartIsWritable(void) {
    return (USART1->SR & USART_SR_TXE) != 0;
}

static inline void PlatformUartWriteByte(uint8_t byte) {
    USART1->DR = byte;
}

static inline bool PlatformUartIsReadable(void) {
    return rxRingHead != rxRingTail;
}

static inline uint8_t PlatformUartReadByte(void) {
    uint8_t byte = rxRingBuffer[rxRingTail];
    rxRingTail = (uint16_t)((rxRingTail + 1) % RX_RING_SIZE);
    return byte;
}

#elif defined(WFP_PLATFORM_PICO)
#include "hardware/uart.h"
#include "pico/stdlib.h"

#define UART_ID     uart1
#define UART_TX_PIN 24
#define UART_RX_PIN 25

static void PlatformUartInit(uint32_t baudRate) {
    uart_init(UART_ID, baudRate);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
}

static inline bool PlatformUartIsWritable(void) {
    return uart_is_writable(UART_ID);
}

static inline void PlatformUartWriteByte(uint8_t byte) {
    uart_putc_raw(UART_ID, byte);
}

static inline bool PlatformUartIsReadable(void) {
    return uart_is_readable(UART_ID);
}

static inline uint8_t PlatformUartReadByte(void) {
    return uart_getc(UART_ID);
}
#endif

#ifdef WFP_ENABLE_SYNTH
#include "SharedState.h"
#include "StorageFunctions.h"
#include "ServiceMenu.h"
#endif

// ---------------------------------------------------------
// Protocol Constants
// ---------------------------------------------------------
#define SOM1 0xF0
#define SOM2 0xAA
#define EOM  0x55

// System & Global Commands
#define CMD_GET_VERSION             0x01
#define CMD_GET_SYS_INFO            0x02
#define CMD_TRACK_CONTROL           0x03
#define CMD_STOP_ALL                0x04
#define CMD_MAIN_VOLUME             0x05
#define CMD_GET_STATUS              0x07
#define CMD_TRACK_VOLUME            0x08
#define CMD_AMP_POWER               0x09
#define CMD_TRACK_FADE              0x0A
#define CMD_RESUME_ALL_SYNC         0x0B
#define CMD_SAMPLERATE_OFFSET       0x0C
#define CMD_TRACK_CONTROL_EX        0x0D
#define CMD_SET_REPORTING           0x0E
#define CMD_SET_TRIGGER_BANK        0x0F

// Expanded & Class Commands
#define CMD_PLAY_EX                 0x60
#define CMD_CLASS_GAIN              0x61
#define CMD_DUCK_RULE               0x62
#define CMD_LOOP_POINTS             0x63
#define CMD_GET_SOUNDTRACK_FOLDERS  0x64
#define CMD_STOP_CLASS              0x65
#define CMD_SET_EVENTS_FLAGS        0x66
#define CMD_SET_SOUNDTRACK_FOLDER   0x67
#define CMD_GET_NUM_LOOPS           0x68
#define CMD_GET_LOOP_POINTS         0x69
#define CMD_SYNTH_CONTROL           0x70

// Responses
#define RSP_VERSION_STRING          0x81
#define RSP_SYSTEM_INFO             0x82
#define RSP_STATUS                  0x83
#define RSP_TRACK_REPORT            0x84
#define RSP_EVENT                   0x8A
#define RSP_SOUNDTRACK              0x8B
#define RSP_LOOP_REPORT             0x8C
#define RSP_SYNTH_RESPONSE          0x8F

// ---------------------------------------------------------
// TX Ring Buffer Implementation
// ---------------------------------------------------------
#define TX_BUFFER_SIZE 512
static uint8_t txRingBuffer[TX_BUFFER_SIZE];
static uint16_t txHead = 0;
static uint16_t txTail = 0;

static WFPAmpPowerCallback AmpPowerCb = NULL;

static void WFPSerial_QueueTxPacket(const uint8_t *data, uint8_t length) {
    for (uint8_t i = 0; i < length; i++) {
        uint16_t nextHead = (uint16_t)((txHead + 1) % TX_BUFFER_SIZE);
        if (nextHead != txTail) {
            txRingBuffer[txHead] = data[i];
            txHead = nextHead;
        } else {
            // Buffer full, drop remaining bytes to prevent blocking
            break;
        }
    }
}

// ---------------------------------------------------------
// Asynchronous Response Senders (0x84, 0x8A)
// ---------------------------------------------------------

void WFPSerial_SendTrackReport(uint16_t trackNumber, uint8_t voiceIndex, uint8_t voiceStatus) {
    if (trackNumber==0) return; // don't send an invalid track report
    uint16_t zeroBasedTrack = trackNumber - 1;
    uint8_t txBuf[9];
    txBuf[0] = SOM1;
    txBuf[1] = SOM2;
    txBuf[2] = 0x09;
    txBuf[3] = RSP_TRACK_REPORT;
    txBuf[4] = (uint8_t)(zeroBasedTrack & 0xFF);
    txBuf[5] = (uint8_t)((zeroBasedTrack >> 8) & 0xFF);
    txBuf[6] = voiceIndex;
    txBuf[7] = voiceStatus;
    txBuf[8] = EOM;
    WFPSerial_QueueTxPacket(txBuf, 9);
}


void WFPSerial_SendEvent(uint8_t eventId, uint16_t trackNumber, uint32_t tuid,
                         uint8_t soundClass, uint8_t hasData, uint8_t eventData) {
    uint8_t packetLen = hasData ? 14 : 13;
    uint8_t txBuf[16];
    txBuf[0] = SOM1;
    txBuf[1] = SOM2;
    txBuf[2] = packetLen;
    txBuf[3] = RSP_EVENT;
    txBuf[4] = eventId;
    txBuf[5] = (uint8_t)(trackNumber & 0xFF);
    txBuf[6] = (uint8_t)((trackNumber >> 8) & 0xFF);
    txBuf[7] = (uint8_t)(tuid & 0xFF);
    txBuf[8] = (uint8_t)((tuid >> 8) & 0xFF);
    txBuf[9] = (uint8_t)((tuid >> 16) & 0xFF);
    txBuf[10] = (uint8_t)((tuid >> 24) & 0xFF);
    txBuf[11] = soundClass;
    if (hasData) {
        txBuf[12] = eventData;
        txBuf[13] = EOM;
    } else {
        txBuf[12] = EOM;
    }
    WFPSerial_QueueTxPacket(txBuf, packetLen);
}

// ---------------------------------------------------------
// Command Handlers
// ---------------------------------------------------------

void WFPSerial_OnGetVersion(void) {
    const char versionStr[18] = "RPU-SP FW   v1.00 ";
    uint8_t strLen = 18;
    uint8_t packetLen = (uint8_t)(5 + strLen);

    uint8_t txBuf[32];
    txBuf[0] = SOM1;
    txBuf[1] = SOM2;
    txBuf[2] = packetLen;
    txBuf[3] = RSP_VERSION_STRING;

    for (uint8_t i = 0; i < strLen; i++) {
        txBuf[4 + i] = (uint8_t)versionStr[i];
    }
    txBuf[4 + strLen] = EOM;

    WFPSerial_QueueTxPacket(txBuf, packetLen);
}

void WFPSerial_OnGetSysInfo(void) {
    uint8_t numVoices = WAVFile_GetMaxVoices();
    uint16_t numTracks = WAVFile_GetTrackCount();
    uint8_t packetLen = 8;

    uint8_t txBuf[8];
    txBuf[0] = SOM1;
    txBuf[1] = SOM2;
    txBuf[2] = packetLen;
    txBuf[3] = RSP_SYSTEM_INFO;
    txBuf[4] = numVoices;
    txBuf[5] = (uint8_t)(numTracks & 0xFF);
    txBuf[6] = (uint8_t)((numTracks >> 8) & 0xFF);
    txBuf[7] = EOM;

    WFPSerial_QueueTxPacket(txBuf, packetLen);
}

void WFPSerial_OnGetStatus(void) {
    uint16_t playingTracks[MAX_VOICES];
    uint8_t count = WAVFile_GetPlayingTracks(playingTracks, MAX_VOICES);
    uint8_t packetLen = (uint8_t)(5 + (count * 2));

    uint8_t txBuf[5 + (MAX_VOICES * 2)];
    txBuf[0] = SOM1;
    txBuf[1] = SOM2;
    txBuf[2] = packetLen;
    txBuf[3] = RSP_STATUS;

    for (uint8_t i = 0; i < count; i++) {
        txBuf[4 + (i * 2)] = (uint8_t)(playingTracks[i] & 0xFF);
        txBuf[5 + (i * 2)] = (uint8_t)((playingTracks[i] >> 8) & 0xFF);
    }
    txBuf[4 + (count * 2)] = EOM;

    WFPSerial_QueueTxPacket(txBuf, packetLen);
}

void WFPSerial_OnMainVolume(int16_t volume) {
    WAVFile_SetSystemVolume(volume);
}

void WFPSerial_OnAmpPower(bool enable) {
    if (AmpPowerCb != NULL) {
        AmpPowerCb(enable);
    }
}

void WFPSerial_OnSetReporting(bool enable) {
    WAVFile_SetReporting(enable ? 1 : 0);
}

void WFPSerial_OnTrackControl(uint8_t code, uint16_t track) {
    WAVFile_TrackControl(code, track, 0);
}

void WFPSerial_OnTrackControlEx(uint8_t code, uint16_t track, bool lock) {
    WAVFile_TrackControl(code, track, lock ? 1 : 0);
}

void WFPSerial_OnStopAll(void) {
    WAVFile_StopAllVoices();
}

void WFPSerial_OnResumeAllSync(void) {
    // Unused in RPU-SP
}

void WFPSerial_OnTrackVolume(uint16_t track, int16_t volume) {
    WAVFile_SetTrackVolume(track, volume);
}

void WFPSerial_OnTrackFade(uint16_t track, int16_t targetVolume, uint16_t fadeTimeMs, bool stopFlag) {
    WAVFile_FadeTrack(track, targetVolume, fadeTimeMs, stopFlag ? 1 : 0);
}

void WFPSerial_OnSamplerateOffset(uint16_t offset) {
    (void)offset;
    // Unused in RPU-SP
}

void WFPSerial_OnSetTriggerBank(uint8_t bank) {
    (void)bank;
    // Unused in RPU-SP
}

void WFPSerial_OnPlayEx(uint16_t trk, uint8_t clsId, uint8_t flags, int16_t gain, uint8_t prio) {
    WAVFile_PlayEx(trk, clsId, flags, gain, prio);
}

void WFPSerial_OnClassGain(uint8_t clsId, int16_t gain, uint16_t fadeTime) {
    WAVFile_SetClassGain(clsId, gain, fadeTime);
}

void WFPSerial_OnDuckRule(uint8_t duckedCls, uint8_t duckingCls, int8_t gain, uint16_t attack, uint16_t release) {
    WAVFile_SetDuckRule(duckedCls, duckingCls, gain, attack, release);
}


void WFPSerial_OnGetSoundtrackFolders(void) {
    uint8_t soundtrackCount = WAVFile_GetSoundtrackCount();
    uint8_t txBuf[6];

    txBuf[0] = SOM1;
    txBuf[1] = SOM2;
    txBuf[2] = 0x06;
    txBuf[3] = RSP_SOUNDTRACK;
    txBuf[4] = soundtrackCount;
    txBuf[5] = EOM;

    WFPSerial_QueueTxPacket(txBuf, 6);
}

void WFPSerial_OnLoopPoints(uint8_t flags, uint8_t loopCount, uint32_t a, uint32_t b,
                            uint16_t fadeIn, uint16_t fadeOut, uint8_t loopsPerPhrase,
                            uint16_t alertMargin, uint32_t nextTrackOrTuid, uint32_t trackOrTuid) {
    WAVFile_SetLoopPoints(flags, loopCount, a, b, fadeIn, fadeOut, loopsPerPhrase,
                          alertMargin, nextTrackOrTuid, trackOrTuid);
}

void WFPSerial_OnGetNumLoops(uint16_t trackNumber) {
    uint8_t txBuf[9];
    txBuf[0] = SOM1;
    txBuf[1] = SOM2;
    txBuf[2] = 0x09;
    txBuf[3] = RSP_LOOP_REPORT;
    txBuf[4] = (uint8_t)(trackNumber & 0xFF);
    txBuf[5] = (uint8_t)((trackNumber >> 8) & 0xFF);
    txBuf[6] = 0x00;
    txBuf[7] = WAVFile_GetTrackLoopCount(trackNumber);
    txBuf[8] = EOM;
    WFPSerial_QueueTxPacket(txBuf, 9);
}

void WFPSerial_OnGetLoopPoints(uint16_t trackNumber, uint8_t loopIndex) {
    WAVStoredLoop loop;
    if (!WAVFile_GetTrackLoop(trackNumber, loopIndex, &loop)) return;

    uint8_t txBuf[19];
    txBuf[0] = SOM1;
    txBuf[1] = SOM2;
    txBuf[2] = 0x13;
    txBuf[3] = RSP_LOOP_REPORT;
    txBuf[4] = (uint8_t)(trackNumber & 0xFF);
    txBuf[5] = (uint8_t)((trackNumber >> 8) & 0xFF);
    txBuf[6] = 0x01;
    txBuf[7] = loopIndex;
    txBuf[8] = loop.loopNumber;
    txBuf[9] = (uint8_t)(loop.pointA & 0xFF);
    txBuf[10] = (uint8_t)((loop.pointA >> 8) & 0xFF);
    txBuf[11] = (uint8_t)((loop.pointA >> 16) & 0xFF);
    txBuf[12] = (uint8_t)((loop.pointA >> 24) & 0xFF);
    txBuf[13] = (uint8_t)(loop.pointB & 0xFF);
    txBuf[14] = (uint8_t)((loop.pointB >> 8) & 0xFF);
    txBuf[15] = (uint8_t)((loop.pointB >> 16) & 0xFF);
    txBuf[16] = (uint8_t)((loop.pointB >> 24) & 0xFF);
    txBuf[17] = loop.loopsPerPhrase;
    txBuf[18] = EOM;
    WFPSerial_QueueTxPacket(txBuf, 19);
}

void WFPSerial_OnStopClass(uint8_t clsId, uint16_t fadeTime) {
    WAVFile_StopClass(clsId, fadeTime);
}

void WFPSerial_OnSetEventsFlags(uint8_t mask, uint8_t clsId) {
    WAVFile_SetEventFlags(mask, clsId);
}

void WFPSerial_OnSetSoundtrackFolder(uint8_t actionFlags, uint8_t folderNum) {
    WAVFile_SetSoundtrackFolder(actionFlags, folderNum);
}


// ---------------------------------------------------------
// Optional Synth Subsystem Handlers (guarded by WFP_ENABLE_SYNTH)
// ---------------------------------------------------------
#ifdef WFP_ENABLE_SYNTH

void WFPSerial_OnSetSynthVolume(uint8_t volId, int16_t leftVol, int16_t rightVol) {
    if (volId == 1) {
        g_YMGainL = leftVol; g_YMGainR = rightVol;
    } else if (volId == 2) {
        g_DACGainL = leftVol; g_DACGainR = rightVol;
    } else if (volId == 3) {
        g_CVSDGainL = leftVol; g_CVSDGainR = rightVol;
    }
}

void WFPSerial_OnGetSynthVolume(uint8_t volId) {
    uint8_t txBuf[11];
    txBuf[0] = SOM1;
    txBuf[1] = SOM2;
    txBuf[2] = 11;
    txBuf[3] = RSP_SYNTH_RESPONSE;
    txBuf[4] = 0x01;
    txBuf[5] = volId;

    int16_t leftVol = 0;
    int16_t rightVol = 0;
    if (volId == 1) {
        leftVol = g_YMGainL; rightVol = g_YMGainR;
    } else if (volId == 2) {
        leftVol = g_DACGainL; rightVol = g_DACGainR;
    } else if (volId == 3) {
        leftVol = g_CVSDGainL; rightVol = g_CVSDGainR;
    }

    txBuf[6] = (uint8_t)(leftVol & 0xFF);
    txBuf[7] = (uint8_t)((leftVol >> 8) & 0xFF);
    txBuf[8] = (uint8_t)(rightVol & 0xFF);
    txBuf[9] = (uint8_t)((rightVol >> 8) & 0xFF);
    txBuf[10] = EOM;
    WFPSerial_QueueTxPacket(txBuf, 11);
}

void WFPSerial_OnGetNumROMs(void) {
    uint16_t numRoms = GetNumROMsInROMFolder();
    uint8_t txBuf[7];
    txBuf[0] = SOM1;
    txBuf[1] = SOM2;
    txBuf[2] = 7;
    txBuf[3] = RSP_SYNTH_RESPONSE;
    txBuf[4] = 0x03;
    txBuf[5] = (uint8_t)(numRoms & 0xFF);
    txBuf[6] = EOM;
    WFPSerial_QueueTxPacket(txBuf, 7);
}

void WFPSerial_OnGetROMName(uint8_t romNum) {
    char WAVFileFullPath[128];
    snprintf(WAVFileFullPath, sizeof(WAVFileFullPath), "0:/ALL_SND_ROMS/");
    char fileName[128];

    uint8_t txBuf[70];
    memset(txBuf, 0, sizeof(txBuf));
    txBuf[0] = SOM1;
    txBuf[1] = SOM2;
    txBuf[2] = 70;
    txBuf[3] = RSP_SYNTH_RESPONSE;
    txBuf[4] = 0x04;

    const char *src = GetROMFilenameFromSDCard(WAVFileFullPath, romNum, fileName, 64) ? fileName : "File Error";
    for (uint8_t i = 0; i < 64 && src[i] != 0; i++) {
        txBuf[5 + i] = (uint8_t)src[i];
    }
    txBuf[69] = EOM;
    WFPSerial_QueueTxPacket(txBuf, 70);
}

static uint8_t g_LastProgressSent = 0;

void WFPSerial_SendflashProgress(uint8_t progress) {
    if (progress != 0xFF && progress > 100) progress = 100;
    if (progress > (g_LastProgressSent + 10) || (progress == 100 && g_LastProgressSent < 100) || progress == 0xFF) {
        g_LastProgressSent = progress;
        uint8_t txBuf[7];
        txBuf[0] = SOM1;
        txBuf[1] = SOM2;
        txBuf[2] = 7;
        txBuf[3] = RSP_SYNTH_RESPONSE;
        txBuf[4] = 0x06;
        txBuf[5] = progress;
        txBuf[6] = EOM;
        WFPSerial_QueueTxPacket(txBuf, 7);

        while (txHead != txTail && PlatformUartIsWritable()) {
            PlatformUartWriteByte(txRingBuffer[txTail]);
            txTail = (uint16_t)((txTail + 1) % TX_BUFFER_SIZE);
        }
    }
}

void WFPSerial_OnFlashROM(uint8_t romNum) {
    g_LastProgressSent = 0;
    uint16_t numRoms = GetNumROMsInROMFolder();
    if (romNum < numRoms) {
        FlashROM(romNum, WFPSerial_SendflashProgress);
    } else {
        WFPSerial_SendflashProgress(0xFF);
    }
}

void WFPSerial_OnGetCurrentROMName(void) {
    char fileName[128];
    uint8_t txBuf[70];
    memset(txBuf, 0, sizeof(txBuf));
    txBuf[0] = SOM1;
    txBuf[1] = SOM2;
    txBuf[2] = 70;
    txBuf[3] = RSP_SYNTH_RESPONSE;
    txBuf[4] = 0x05;

    const char *src = GetROMName(fileName) ? fileName : "Unknown";
    for (uint8_t i = 0; i < 64 && src[i] != 0; i++) {
        txBuf[5 + i] = (uint8_t)src[i];
    }
    txBuf[69] = EOM;
    WFPSerial_QueueTxPacket(txBuf, 70);
}
#endif


static void SendSynthNotSupported(uint8_t synthCmd, uint16_t synthType) {
    uint8_t txBuf[9];
    txBuf[0] = SOM1;
    txBuf[1] = SOM2;
    txBuf[2] = 9;
    txBuf[3] = RSP_SYNTH_RESPONSE;
    txBuf[4] = synthCmd;
    txBuf[5] = (uint8_t)(synthType & 0xFF);
    txBuf[6] = (uint8_t)((synthType >> 8) & 0xFF);
    txBuf[7] = 0x00; // Synth not supported
    txBuf[8] = EOM;
    WFPSerial_QueueTxPacket(txBuf, 9);
}


// ---------------------------------------------------------
// Packet Dispatcher
// ---------------------------------------------------------
static bool PacketReceived = false;

bool WFPSerial_HasReceivedPacket(void) {
    return PacketReceived;
}

static void WFPSerial_ProcessPacket(const uint8_t *buffer, uint8_t length) {
    if (length < 5) return;

    PacketReceived = true;
    uint8_t cmd = buffer[3];

    switch (cmd) {
        case CMD_GET_VERSION:
            WFPSerial_OnGetVersion();
            break;
        case CMD_GET_SYS_INFO:
            WFPSerial_OnGetSysInfo();
            break;
        case CMD_GET_STATUS:
            WFPSerial_OnGetStatus();
            break;
        case CMD_MAIN_VOLUME:
            if (length == 7) {
                int16_t vol = (int16_t)(buffer[4] | (buffer[5] << 8));
                WFPSerial_OnMainVolume(vol);
            }
            break;
        case CMD_AMP_POWER:
            if (length == 6) {
                WFPSerial_OnAmpPower(buffer[4] != 0);
            }
            break;
        case CMD_SET_REPORTING:
            if (length == 6) {
                WFPSerial_OnSetReporting(buffer[4] != 0);
            }
            break;
        case CMD_TRACK_CONTROL:
            if (length == 8) {
                uint8_t code = buffer[4];
                uint16_t trk = (uint16_t)(buffer[5] | (buffer[6] << 8));
                WFPSerial_OnTrackControl(code, trk);
            }
            break;
        case CMD_TRACK_CONTROL_EX:
            if (length == 9) {
                uint8_t code = buffer[4];
                uint16_t trk = (uint16_t)(buffer[5] | (buffer[6] << 8));
                bool lock = (buffer[7] != 0);
                WFPSerial_OnTrackControlEx(code, trk, lock);
            }
            break;
        case CMD_STOP_ALL:
            WFPSerial_OnStopAll();
            break;
        case CMD_RESUME_ALL_SYNC:
            WFPSerial_OnResumeAllSync();
            break;
        case CMD_TRACK_VOLUME:
            if (length == 9) {
                uint16_t trk = (uint16_t)(buffer[4] | (buffer[5] << 8));
                int16_t vol = (int16_t)(buffer[6] | (buffer[7] << 8));
                WFPSerial_OnTrackVolume(trk, vol);
            }
            break;
        case CMD_TRACK_FADE:
            if (length == 12) {
                uint16_t trk = (uint16_t)(buffer[4] | (buffer[5] << 8));
                int16_t vol = (int16_t)(buffer[6] | (buffer[7] << 8));
                uint16_t time = (uint16_t)(buffer[8] | (buffer[9] << 8));
                bool stopFlag = (buffer[10] != 0);
                WFPSerial_OnTrackFade(trk, vol, time, stopFlag);
            }
            break;
        case CMD_SAMPLERATE_OFFSET:
            if (length == 7) {
                uint16_t off = (uint16_t)(buffer[4] | (buffer[5] << 8));
                WFPSerial_OnSamplerateOffset(off);
            }
            break;
        case CMD_SET_TRIGGER_BANK:
            if (length == 6) {
                WFPSerial_OnSetTriggerBank(buffer[4]);
            }
            break;
        case CMD_PLAY_EX:
            if (length == 12) {
                uint16_t trk = (uint16_t)(buffer[4] | (buffer[5] << 8));
                uint8_t clsId = buffer[6];
                uint8_t flags = buffer[7];
                int16_t gain = (int16_t)(buffer[8] | (buffer[9] << 8));
                uint8_t prio = buffer[10];
                WFPSerial_OnPlayEx(trk, clsId, flags, gain, prio);
            }
            break;
        case CMD_CLASS_GAIN:
            if (length == 10) {
                uint8_t clsId = buffer[4];
                int16_t gain = (int16_t)(buffer[5] | (buffer[6] << 8));
                uint16_t fade = (uint16_t)(buffer[7] | (buffer[8] << 8));
                WFPSerial_OnClassGain(clsId, gain, fade);
            }
            break;
        case CMD_DUCK_RULE:
            if (length == 12) {
                uint8_t ducked = buffer[4];
                uint8_t ducking = buffer[5];
                int8_t gain = (int8_t)buffer[6];
                uint16_t atk = (uint16_t)(buffer[7] | (buffer[8] << 8));
                uint16_t rel = (uint16_t)(buffer[9] | (buffer[10] << 8));
                WFPSerial_OnDuckRule(ducked, ducking, gain, atk, rel);
            }
            break;

        case CMD_LOOP_POINTS: {
            if (length >= 26) {
                uint8_t flags = buffer[4];
                uint8_t nextIsTuid = (flags & WAV_LOOP_NEXT_IS_TUID) ? 1 : 0;
                uint8_t useTuid = (flags & WAV_LOOP_USE_TUID) ? 1 : 0;
                uint8_t expectedLen = (uint8_t)(26 + (nextIsTuid ? 2 : 0) + (useTuid ? 2 : 0));
                if (length >= expectedLen) {
                    uint8_t loops = buffer[5];
                    uint32_t a = (uint32_t)buffer[6] | ((uint32_t)buffer[7] << 8) |
                                 ((uint32_t)buffer[8] << 16) | ((uint32_t)buffer[9] << 24);
                    uint32_t b = (uint32_t)buffer[10] | ((uint32_t)buffer[11] << 8) |
                                 ((uint32_t)buffer[12] << 16) | ((uint32_t)buffer[13] << 24);
                    uint16_t fadein = (uint16_t)(buffer[14] | (buffer[15] << 8));
                    uint16_t fadeout = (uint16_t)(buffer[16] | (buffer[17] << 8));
                    uint8_t loopsPerPhrase = buffer[18];
                    uint16_t margin = (uint16_t)(buffer[19] | (buffer[20] << 8));

                    uint8_t idx = 21;
                    uint32_t nextTrk = (uint32_t)buffer[idx] | ((uint32_t)buffer[idx + 1] << 8);
                    idx += 2;
                    if (nextIsTuid) {
                        nextTrk |= ((uint32_t)buffer[idx] << 16) | ((uint32_t)buffer[idx + 1] << 24);
                        idx += 2;
                    }

                    uint32_t target = (uint32_t)buffer[idx] | ((uint32_t)buffer[idx + 1] << 8);
                    idx += 2;
                    if (useTuid) {
                        target |= ((uint32_t)buffer[idx] << 16) | ((uint32_t)buffer[idx + 1] << 24);
                    }

                    WFPSerial_OnLoopPoints(flags, loops, a, b, fadein, fadeout, loopsPerPhrase,
                                           margin, nextTrk, target);
                }
            }
            break;
        }
        case CMD_GET_NUM_LOOPS:
            if (length == 7) {
                uint16_t trk = (uint16_t)(buffer[4] | (buffer[5] << 8));
                WFPSerial_OnGetNumLoops(trk);
            }
            break;
        case CMD_GET_LOOP_POINTS:
            if (length == 8) {
                uint16_t trk = (uint16_t)(buffer[4] | (buffer[5] << 8));
                WFPSerial_OnGetLoopPoints(trk, buffer[6]);
            }
            break;

        case CMD_GET_SOUNDTRACK_FOLDERS:
            if (length == 5) WFPSerial_OnGetSoundtrackFolders();
            break;
        case CMD_STOP_CLASS:
            if (length == 8) {
                uint8_t clsId = buffer[4];
                uint16_t fade = (uint16_t)(buffer[5] | (buffer[6] << 8));
                WFPSerial_OnStopClass(clsId, fade);
            }
            break;
        case CMD_SET_EVENTS_FLAGS:
            if (length == 7) WFPSerial_OnSetEventsFlags(buffer[4], buffer[5]);
            break;
        case CMD_SET_SOUNDTRACK_FOLDER:
            if (length == 7) WFPSerial_OnSetSoundtrackFolder(buffer[4], buffer[5]);
            break;
        case CMD_SYNTH_CONTROL:
            if (length >= 6) {
                uint8_t synthCmd = buffer[4];
                if (synthCmd == 0x80 && length == 8) {
                    uint16_t synthType = (uint16_t)(buffer[5] | (buffer[6] << 8));
                    SendSynthNotSupported(0x80, synthType);
                } else if (synthCmd == 0x81 && length == 9) {
                    uint16_t synthType = (uint16_t)(buffer[5] | (buffer[6] << 8));
                    SendSynthNotSupported(0x81, synthType);
                }
#ifdef WFP_ENABLE_SYNTH
                else if (synthCmd == 0x01 && length == 7) {
                    WFPSerial_OnGetSynthVolume(buffer[5]);
                } else if (synthCmd == 0x02 && length == 11) {
                    uint8_t volId = buffer[5];
                    int16_t leftVol = (int16_t)(buffer[6] | (buffer[7] << 8));
                    int16_t rightVol = (int16_t)(buffer[8] | (buffer[9] << 8));
                    WFPSerial_OnSetSynthVolume(volId, leftVol, rightVol);
                } else if (synthCmd == 0x03 && length == 6) {
                    WFPSerial_OnGetNumROMs();
                } else if (synthCmd == 0x04 && length == 7) {
                    WFPSerial_OnGetROMName(buffer[5]);
                } else if (synthCmd == 0x05 && length == 6) {
                    WFPSerial_OnGetCurrentROMName();
                } else if (synthCmd == 0x06 && length == 7) {
                    WFPSerial_OnFlashROM(buffer[5]);
                }
#endif
            }
            break;
        default:
            break;
    }
}


// ---------------------------------------------------------
// Public API
// ---------------------------------------------------------

uint32_t WFPSerial_LoadBaudRateFromIni(void) {
    // Stub: parse SD card ini file here once format is finalized
    return WFP_DEFAULT_BAUD_RATE;
}

void WFPSerial_RegisterAmpPowerCallback(WFPAmpPowerCallback cb) {
    AmpPowerCb = cb;
}

void WFPSerial_InitSerialCommsWithBaud(uint32_t baudRate) {
    txHead = 0;
    txTail = 0;
    PlatformUartInit(baudRate);
    WAVFile_RegisterCallbacks(WFPSerial_SendTrackReport, WFPSerial_SendEvent);
}

void WFPSerial_InitSerialComms(void) {
    uint32_t baudRate = WFPSerial_LoadBaudRateFromIni();
    WFPSerial_InitSerialCommsWithBaud(baudRate);
}

void WFPSerial_Update(void) {
    // 1. Drain the software TX ring buffer into hardware UART
    while (txHead != txTail && PlatformUartIsWritable()) {
        PlatformUartWriteByte(txRingBuffer[txTail]);
        txTail = (uint16_t)((txTail + 1) % TX_BUFFER_SIZE);
    }

#if defined(WFP_PLATFORM_PICO)
    // Throttle APB bus reads on Pico to once every 1ms
    static uint32_t last_check_us = 0;
    uint32_t now = time_us_32();
    if (now - last_check_us < 1000) return;
    last_check_us = now;
#endif

    // 2. Process incoming RX bytes
    static uint8_t state = 0;
    static uint8_t rxCount = 0;
    static uint8_t rxLen = 0;
    static uint8_t rxBuffer[128];

    while (PlatformUartIsReadable()) {
        uint8_t dat = PlatformUartReadByte();

        if (state == 0 && dat == SOM1) {
            rxBuffer[0] = dat;
            state = 1;
        } else if (state == 1) {
            if (dat == SOM2) {
                rxBuffer[1] = dat;
                state = 2;
            } else if (dat == SOM1) {
                rxBuffer[0] = dat;
                state = 1;
            } else {
                state = 0;
            }
        } else if (state == 2) {
            rxLen = dat;
            if (rxLen >= 5 && rxLen <= sizeof(rxBuffer)) {
                rxBuffer[2] = rxLen;
                rxCount = 3;
                state = 3;
            } else {
                state = 0;
            }
        } else if (state == 3) {
            rxBuffer[rxCount++] = dat;
            if (rxCount == rxLen) {
                if (dat == EOM) {
                    WFPSerial_ProcessPacket(rxBuffer, rxLen);
                }
                state = 0;
            }
        }
    }
}
