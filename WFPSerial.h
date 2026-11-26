#ifndef WAV_FILE_PLAYER_SERIAL_H
#define WAV_FILE_PLAYER_SERIAL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Platform selection: define WFP_PLATFORM_STM32 or WFP_PLATFORM_PICO in build flags
// Defaults to STM32 if STM32F4xx/STM32F405xx is defined, otherwise Pico
#if !defined(WFP_PLATFORM_STM32) && !defined(WFP_PLATFORM_PICO)
#if defined(STM32F405xx) || defined(STM32F4xx) || defined(ARDUINO_ARCH_STM32)
#define WFP_PLATFORM_STM32
#else
#define WFP_PLATFORM_PICO
#endif
#endif

// Default UART configuration
#ifndef WFP_DEFAULT_BAUD_RATE
#define WFP_DEFAULT_BAUD_RATE       57600
#endif

// Optional amp power callback (e.g., to toggle MUTE_PIN in board-level code)
typedef void (*WFPAmpPowerCallback)(bool enable);

// Initialization and main-loop update
void WFPSerial_InitSerialComms(void);
void WFPSerial_InitSerialCommsWithBaud(uint32_t baudRate);
void WFPSerial_RegisterAmpPowerCallback(WFPAmpPowerCallback cb);
uint32_t WFPSerial_LoadBaudRateFromIni(void);
void WFPSerial_Update(void);
bool WFPSerial_HasReceivedPacket(void);

// Asynchronous response senders (can be registered with WAVFile_RegisterCallbacks)
void WFPSerial_SendTrackReport(uint16_t trackNumber, uint8_t voiceIndex, uint8_t voiceStatus);
void WFPSerial_SendEvent(uint8_t eventId, uint16_t trackNumber, uint32_t tuid,
                         uint8_t soundClass, uint8_t hasData, uint8_t eventData);

#ifdef __cplusplus
}
#endif

#endif
