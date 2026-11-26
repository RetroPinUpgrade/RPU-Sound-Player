# RPU Sound Player Instructions for Flashing

This document will talk about different ways to flash the RPU Sound Player firmware on a WAV Trigger board.

## Using the Arduino IDE and an FTDI  

  The [FTDI programmer](https://www.adafruit.com/product/284) is a USB to serial converter 

  ### Steps
  
  1. Install the [Arduino IDE](https://www.arduino.cc/en/software/)
  2. Open Arduino > Settings > Additional Boards Manager
  3. Add this URL on its own line: https://github.com/stm32duino/BoardManagerFiles/raw/main/
  4. Click Arduino > Tools > Board > Board Manager
  5. Search for STM32, and install version 2.7.1 (for mac, the toolchain is broken for others)
  6. In Terminal, you have to add the STM32 bin to your path:`echo "/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/Resources/bin" | sudo tee /etc/paths.d/stm32`
  7. In the Tools menu:  
		Board = "Generic STM32F4 Series”  
		Board Part Number = "Generic F405RGTx”  
		U(S)ART Support = "Enabled (generic 'Serial’)"  
		Upload Method = "STM32CubeProgrammer (Serial)”
  8. Connect FTDI to serial port of WAV Trigger board (on the 6-pin header — make sure to align GND to GND)
  9. Connect FTDI to USB port of computer
  10. Set Tools > Port in the Arduino IDE

## Using the Arduino IDE and an ST-Link V2

  The  [ST LINK V2](https://www.amazon.com/HiLetgo-Emulator-Downloader-Programmer-STM32F103C8T6/dp/B07SQV6VLZ/) is a USB to SWD programmer. 

  NOTE: I’VE NEVER SEEN THIS WORK!

  ### Steps

  1. Install the [Arduino IDE](https://www.arduino.cc/en/software/)
  2. Open Arduino > Settings > Additional Boards Manager
  3. Add this URL on its own line: https://github.com/stm32duino/BoardManagerFiles/raw/main/
  4. Click Arduino > Tools > Board > Board Manager
  5. Search for STM32, and install version 2.7.1 (for mac, the toolchain is broken for others)
  6. In Terminal, you have to add the STM32 bin to your path:`echo "/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/Resources/bin" | sudo tee /etc/paths.d/stm32`
  7. In the Tools menu:  
		Board = "Generic STM32F4 Series”  
		Board Part Number = "Generic F405RGTx”  
		U(S)ART Support = "Enabled (generic 'Serial’)"  
		Upload Method = "STM32CubeProgrammer (SWD)”
  8. Connect ST LINK V2 to WAV Trigger board
      ST GND -> GND (use the GND on pin 1 of the 6-pin header)
      ST 5V ->  5V (use the 5V on pin 3 of the 6-pin header)
      ST SWDIO -> PA13 (labelled T14 on the second trigger header)
      ST SWCLK -> PA14 (labelled T15 on the second trigger header)
  9. Connect ST LINK V2 to USB port of computer
  10. NOTE: I’VE NEVER SEEN THIS WORK!

    
    

