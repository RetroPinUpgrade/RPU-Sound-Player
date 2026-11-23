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
  8. Connect FTDI to serial port of WAV Trigger board
  9. Connect FTDI to USB port of computer
  10. Set Tools > Port in the Arduino IDE

## Using the Arduino IDE and an ST-Link V2

