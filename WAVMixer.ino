// WAVMixer.ino
// STM32F405 Polyphonic RPU-SP Sound Player (14 Voices)
// - Architecture: Hybrid Cluster Map (SRAM2) + FatFs Fallback + 14-Voice Software Mixer
// - Serial Control: USART1 (PA9 TX / PA10 RX) at 57600 baud (8N1)

#include <Arduino.h>
#include <STM32SD.h>
#include <SPI.h>
#include <stm32f4xx_hal.h>
#include "WAVFilePlayer.h"
#include "WAVtoDAC.h"
#include "WFPSerial.h"

// Pinout
#define LED_PIN                 PC4
#define MUTE_PIN                PC0

// Internal flash storage for startup.wav (Sector 11)
#define STARTUP_WAV_FLASH_ADDR  0x080E0000
#define STARTUP_CRC_ADDR        (STARTUP_WAV_FLASH_ADDR)
#define STARTUP_LEN_ADDR        (STARTUP_WAV_FLASH_ADDR + 4)
#define STARTUP_DATA_ADDR       (STARTUP_WAV_FLASH_ADDR + 8)
#define STARTUP_WAV_MAX_SIZE    (128 * 1024 - 8)

I2S_HandleTypeDef Hi2s2;
DMA_HandleTypeDef HDMA_Spi2_TX;
volatile bool HalfTransfer = false;
volatile bool FullTransfer = false;

int16_t DMABuffer[DMA_BUF_SIZE];

static void OnAmpPowerChange(bool enable) {
    digitalWrite(MUTE_PIN, enable ? HIGH : LOW);
}

uint32_t CalculateFileCRC(const char *path) {
    FIL file;
    uint8_t buffer[2048];
    UINT br = 0;
    if (f_open(&file, path, FA_READ) != FR_OK) return 0;
    f_read(&file, buffer, 2048, &br);
    f_close(&file);

    __HAL_RCC_CRC_CLK_ENABLE();
    CRC_HandleTypeDef hcrc;
    hcrc.Instance = CRC;
    HAL_CRC_Init(&hcrc);
    uint32_t crc = HAL_CRC_Accumulate(&hcrc, (uint32_t *)buffer, br / 4);
    __HAL_RCC_CRC_CLK_DISABLE();
    return crc;
}

void UpdateFlashStartup(const char *path, uint32_t newCrc) {
    FIL file;
    if (f_open(&file, path, FA_READ) != FR_OK) return;

    uint32_t fileSize = f_size(&file);
    if (fileSize > STARTUP_WAV_MAX_SIZE) {
        f_close(&file);
        return;
    }

    HAL_FLASH_Unlock();

    FLASH_EraseInitTypeDef erase;
    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Sector = FLASH_SECTOR_11;
    erase.NbSectors = 1;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    uint32_t sectorError = 0;
    HAL_FLASHEx_Erase(&erase, &sectorError);

    HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, STARTUP_CRC_ADDR, newCrc);
    HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, STARTUP_LEN_ADDR, fileSize);

    uint8_t buf[512];
    UINT br = 0;
    uint32_t currentAddr = STARTUP_DATA_ADDR;

    while (f_read(&file, buf, 512, &br) == FR_OK && br > 0) {
        for (UINT i = 0; i < br; i += 4) {
            uint32_t word = 0;
            uint32_t copyLen = (br - i >= 4) ? 4 : (br - i);
            memcpy(&word, &buf[i], copyLen);
            HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, currentAddr + i, word);
        }
        currentAddr += br;
    }

    HAL_FLASH_Lock();
    f_close(&file);
}

// I2S / DMA Interrupts
extern "C" void HAL_I2S_TxHalfCpltCallback(I2S_HandleTypeDef *hi2s) {
    (void)hi2s;
    HalfTransfer = true;
}

extern "C" void HAL_I2S_TxCpltCallback(I2S_HandleTypeDef *hi2s) {
    (void)hi2s;
    FullTransfer = true;
}

extern "C" void DMA1_Stream4_IRQHandler(void) {
    HAL_DMA_IRQHandler(&HDMA_Spi2_TX);
}

void Init_Audio_Hardware(void) {
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();
    __HAL_RCC_SPI2_CLK_ENABLE();

    // I2S Pins: PB12, PB13, PB15, PC6 (MCLK)
    GPIO_InitTypeDef GPIO_InitStruct;
    memset(&GPIO_InitStruct, 0, sizeof(GPIO_InitStruct));

    GPIO_InitStruct.Pin = GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_15;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF5_SPI2;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_6;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    // DMA Init (Stream 4 Channel 0)
    HDMA_Spi2_TX.Instance = DMA1_Stream4;
    HDMA_Spi2_TX.Init.Channel = DMA_CHANNEL_0;
    HDMA_Spi2_TX.Init.Direction = DMA_MEMORY_TO_PERIPH;
    HDMA_Spi2_TX.Init.PeriphInc = DMA_PINC_DISABLE;
    HDMA_Spi2_TX.Init.MemInc = DMA_MINC_ENABLE;
    HDMA_Spi2_TX.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    HDMA_Spi2_TX.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
    HDMA_Spi2_TX.Init.Mode = DMA_CIRCULAR;
    HDMA_Spi2_TX.Init.Priority = DMA_PRIORITY_VERY_HIGH;
    HAL_DMA_Init(&HDMA_Spi2_TX);
    __HAL_LINKDMA(&Hi2s2, hdmatx, HDMA_Spi2_TX);

    HAL_NVIC_SetPriority(DMA1_Stream4_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream4_IRQn);

    // I2S Init
    Hi2s2.Instance = SPI2;
    Hi2s2.Init.Mode = I2S_MODE_MASTER_TX;
    Hi2s2.Init.Standard = I2S_STANDARD_PHILIPS;
    Hi2s2.Init.DataFormat = I2S_DATAFORMAT_16B;
    Hi2s2.Init.MCLKOutput = I2S_MCLKOUTPUT_ENABLE;
    Hi2s2.Init.AudioFreq = I2S_AUDIOFREQ_44K;
    Hi2s2.Init.CPOL = I2S_CPOL_LOW;
    Hi2s2.Init.ClockSource = I2S_CLOCK_PLL;
    HAL_I2S_Init(&Hi2s2);
}

void SystemClock_Config(void) {
    RCC_OscInitTypeDef RCC_OscInitStruct;
    memset(&RCC_OscInitStruct, 0, sizeof(RCC_OscInitStruct));
    RCC_ClkInitTypeDef RCC_ClkInitStruct;
    memset(&RCC_ClkInitStruct, 0, sizeof(RCC_ClkInitStruct));
    RCC_PeriphCLKInitTypeDef PeriphClkInitStruct;
    memset(&PeriphClkInitStruct, 0, sizeof(PeriphClkInitStruct));

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState = RCC_HSE_ON;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLM = 8;
    RCC_OscInitStruct.PLL.PLLN = 336;
    RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
    RCC_OscInitStruct.PLL.PLLQ = 7;
    HAL_RCC_OscConfig(&RCC_OscInitStruct);

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                                  RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;
    HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5);

    PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_I2S;
    PeriphClkInitStruct.PLLI2S.PLLI2SN = 271;
    PeriphClkInitStruct.PLLI2S.PLLI2SR = 2;
    HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct);
}

void PlayStartup(void) {
    uint32_t storedCrc = *(uint32_t *)STARTUP_CRC_ADDR;
    uint32_t storedLen = *(uint32_t *)STARTUP_LEN_ADDR;

    if (SD.exists("startup.wav")) {
        uint32_t currentCrc = CalculateFileCRC("startup.wav");
        if (currentCrc != storedCrc) {
            UpdateFlashStartup("startup.wav", currentCrc);
            storedLen = *(uint32_t *)STARTUP_LEN_ADDR;
        }
    }

    if (storedLen == 0xFFFFFFFF || storedLen > STARTUP_WAV_MAX_SIZE || storedLen == 0) return;

    WAVFile_StartFlashVoice((const uint8_t *)STARTUP_DATA_ADDR, storedLen);
}



// USART1 bridge for WFPSerial.c (PA10 = RX, PA9 = TX)
Uart RPUSerial(PA_10, PA_9);

extern "C" void WFPPlatform_UartInit(uint32_t baudRate) {
    RPUSerial.begin(baudRate);
}

extern "C" bool WFPPlatform_UartIsWritable(void) {
    return RPUSerial.availableForWrite() > 0;
}

extern "C" void WFPPlatform_UartWriteByte(uint8_t byte) {
    RPUSerial.write(byte);
}

extern "C" bool WFPPlatform_UartIsReadable(void) {
    return RPUSerial.available() > 0;
}

extern "C" uint8_t WFPPlatform_UartReadByte(void) {
    return (uint8_t)RPUSerial.read();
}


bool StartupBongPending = false;

void setup(void) {
    pinMode(LED_PIN, OUTPUT);
    pinMode(MUTE_PIN, OUTPUT);
    digitalWrite(MUTE_PIN, HIGH);

    SystemClock_Config();
    WAVFile_Init();

    // Start serial early so incoming controller messages are buffered during boot
    WFPSerial_RegisterAmpPowerCallback(OnAmpPowerChange);
    WFPSerial_InitSerialComms();
    delay(200);

    // SD Card Init
    bool sdOk = false;
    unsigned long startMs = millis();
    while (!sdOk && (millis() - startMs) < 5000) {
        sdOk = SD.begin();
        if (!sdOk) {
            digitalWrite(LED_PIN, !digitalRead(LED_PIN));
            delay(50);
        }
    }

    // Check FatFs root directory access directly
    DIR dir;
    FRESULT fr = f_opendir(&dir, "/");
    if (fr == FR_OK) {
        f_closedir(&dir);
    }

    // Scan alternate soundtracks and build SRAM2 cluster map
    digitalWrite(LED_PIN, HIGH);
    WAVFile_ScanSoundtracks();
    WAVFile_BuildClusterMap();
    digitalWrite(LED_PIN, LOW);

    // Audio Engine Init
    Init_Audio_Hardware();
    HAL_I2S_Transmit_DMA(&Hi2s2, (uint16_t *)DMABuffer, DMA_BUF_SIZE);

    // Handle any messages that arrived during boot before playing the bong
    WFPSerial_Update();

    if (!WFPSerial_HasReceivedPacket()) {
        PlayStartup();
    } else {
        StartupBongPending = true;
    }
}

unsigned long lastBlinkTime = 0;
unsigned long lastFadeTime = 0;
bool ledState = false;

void loop(void) {
    unsigned long now = millis();

    // 1. Audio Processing (highest priority)
    if (HalfTransfer) {
        HalfTransfer = false;
        Mixer_Process(DMABuffer, DMA_BUF_SIZE, true, false);
    }
    if (FullTransfer) {
        FullTransfer = false;
        Mixer_Process(DMABuffer, DMA_BUF_SIZE, false, true);
    }

    // 2. Serial RX/TX and Deferred Event Dispatch
    WFPSerial_Update();
    WAVFile_ProcessEvents();

    // 3. Fade & Ducking Tick (1ms resolution)
    if (now != lastFadeTime) {
        WAVFile_UpdateFades((uint32_t)(now - lastFadeTime));
        lastFadeTime = now;
    }

    // 4. SD Card Voice Buffer Refill
    WAVFile_RefillVoices();

    // 5. Play deferred startup bong once all active tracks finish
    if (0 && StartupBongPending) {
        uint16_t playing[1];
        if (WAVFile_GetPlayingTracks(playing, 1) == 0) {
            StartupBongPending = false;
            PlayStartup();
        }
    }

    // 6. Heartbeat LED (1s)
    if (now - lastBlinkTime >= 1000) {
        lastBlinkTime = now;
        ledState = !ledState;
        digitalWrite(LED_PIN, ledState);
    }
}



