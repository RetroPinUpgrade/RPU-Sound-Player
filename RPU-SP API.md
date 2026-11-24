# RPU-Sound Player (RPU-SP)

This document outlines the standard serial control protocol for the RPU (Retro Pin Upgrade) Sound Player. It serves as a definitive document of the command structure, packet formatting, and available functions.

## 1. Protocol Overview

The RPU-SP uses a bidirectional asynchronous serial port.

* **Default Baud Rate:** 57.6 kbps (can be changed via the `init` file).
* **Data Format:** 8 data bits, no parity, 1 stop bit (8N1).

### Packet Structure

All commands and responses to/from the RPU-SP follow a strict hex-formatted packet structure:

| Byte Index | Description | Example / Value | 
| ----- | ----- | ----- | 
| 0 | Start of Message 1 (SOM1) | `0xF0` | 
| 1 | Start of Message 2 (SOM2) | `0xAA` | 
| 2 | Packet Length | Total number of bytes (including SOM and EOM) | 
| 3 | Command Code | e.g., `0x03` for Track Control | 
| 4...n | Data Bytes | Command-specific parameters (LSB first) | 
| Last | End of Message (EOM) | `0x55` | 

*Note: Multi-byte parameters (like Track Numbers or Volume/Gain) are always transmitted in Little-Endian format (Least Significant Byte first).*


## 2. System & Global Commands - Overview

These are the command packets that the RPU-SP understands (unless marked with “unused”).

| Command Byte | Name | Description | 
| ----- | ----- | ----- | 
| 0x01 | Get Version | Requests version | 
| 0x02 | Get Sys Info | Requests system capabilities | 
| 0x03 | Track Control | Start/stop/loop, etc. | 
| 0x04 | Stop All | Stops playback | 
| 0x05 | Master Volume | Volume for whole system | 
| 0x07 | Get Status | Requests voice status | 
| 0x08 | Track Volume | Volume for track | 
| 0x09 | Amp Power | Turns on/off amplifier | 
| 0x0A | Track Fade | Fade for track | 
| 0x0B | Resume All Sync | (unused) | 
| 0x0C | Sample Rate Offset | (unused) | 
| 0x0D | Track Control Ex | Expanded version of above | 
| 0x0E | Set Reporting | Controls responses | 
| 0x0F | Set Trigger Bank | (unused) | 
| 0x60 | Play Ex | Play track with a class | 
| 0x61 | Class Gain | Set a gain for a class | 
| 0x62 | Duck Rule | Set duck rule between classes | 
| 0x63 | Loop Points | Override Track/TUID loop points | 
| 0x64 | Get Soundtrack Folders | Get number of alternate soundtracks |
| 0x65 | Stop Class | Stop every voice in a class | 
| 0x66 | Set Events Flags | Set which events receive a response | 
| 0x67 | Set Soundtrack Folder | Sets which alternate soundtrack to use |
| 0x70 | Synth Control | Controls for synthesizer card | 

Notes:

1. In the above commands, if they mention “Track,” the command will affect all current playing tracks that match that identifier. If the command mentions TUID (Track Unique ID), it will affect only one particular instance of a playing track.


## 3. Responses - Overview

The following are response packets to commands or events.

| Command Byte | Name | Description | 
| ----- | ----- | ----- | 
| 0x81 | Version String | Firmware version | 
| 0x82 | System Info | Track, voice capability | 
| 0x83 | Status | Returns all voices playing | 
| 0x84 | Track Report | Report if a track is playing/stopped | 
| 0x8A | Event | Alert of an event | 
| 0x8B | Soundtrack | Information about soundtracks |
| 0x8F | Synth Response | Responses from synthesizer card | 


## 4. Commands - Full Description

### Get Version (`0x01`)
 
The Get Version command is a simple request for a version string from the RPU-SP.
The RPU-SP will respond to this with a Version String (`0x81`).

The "WAV Trigger” firmware answers the same command with its own name, for example `"WAV Trigger v1.34   "`, so the reply tells a controller which player it is talking to (see Version String, response `0x81`, below).

#### Example Packet
 
| Byte Index | Description | Example / Value | 
| ----- | ----- | ----- | 
| 0 | Start of Message 1 (SOM1) | `0xF0` | 
| 1 | Start of Message 2 (SOM2) | `0xAA` | 
| 2 | Packet Length | `0x05` | 
| 3 | CMD: Get Version | `0x01` | 
| 4 | End of Message (EOM) | `0x55` | 

---   

### System Info (`0x02`)

The System Info command is a simple request for the basic capabilities of the RPU-SP.
The RPU-SP will respond to this with a System Info (`0x82`).

#### Example Packet

| Byte Index | Description | Example / Value | 
| ----- | ----- | ----- | 
| 0 | Start of Message 1 (SOM1) | `0xF0` | 
| 1 | Start of Message 2 (SOM2) | `0xAA` | 
| 2 | Packet Length | `0x05` | 
| 3 | CMD: System Info | `0x02` | 
| 4 | End of Message (EOM) | `0x55` | 

---

### Track Control (`0x03`)

The Track Control command is used to start, stop, or loop tracks.

#### Parameters
* **Control Code:** 8-bit integer representing the specific action to perform:
  * `0x00`: Play Solo (stops other tracks)
  * `0x01`: Play Polyphonic
  * `0x02`: Pause
  * `0x03`: Resume
  * `0x04`: Stop
  * `0x05`: Loop On
  * `0x06`: Loop Off
  * `0x07`: Load (pauses at first sample)
* **Track Number:** 16-bit unsigned integer (Tracks 1 - 4096). Identifies the track to be controlled.

#### Example Packet (Play Track 1 Polyphonic)

| Byte Index | Description | Example / Value | 
| ----- | ----- | ----- | 
| 0 | Start of Message 1 (SOM1) | `0xF0` | 
| 1 | Start of Message 2 (SOM2) | `0xAA` | 
| 2 | Packet Length | `0x08` | 
| 3 | CMD: Track Control | `0x03` | 
| 4 | Control Code | `0x01` (Play Poly) | 
| 5 | Track Number LSB | `0x01` | 
| 6 | Track Number MSB | `0x00` | 
| 7 | End of Message (EOM) | `0x55` | 

---

### Stop All (`0x04`)

The Stop All command stops all playback immediately. It requires no additional data payload.

#### Example Packet

| Byte Index | Description | Example / Value | 
| ----- | ----- | ----- | 
| 0 | Start of Message 1 (SOM1) | `0xF0` | 
| 1 | Start of Message 2 (SOM2) | `0xAA` | 
| 2 | Packet Length | `0x05` | 
| 3 | CMD: Stop All | `0x04` | 
| 4 | End of Message (EOM) | `0x55` | 

---

### Master Volume (`0x05`)

The Master Volume command sets the final output gain for the whole system.

#### Parameters
* **Volume (Gain):** 16-bit signed integer. The range spans from -70dB to +10dB. Transmitted Little-Endian (LSB, then MSB).

#### Example Packet (Set Master Volume to 0dB)

| Byte Index | Description | Example / Value | 
| ----- | ----- | ----- | 
| 0 | Start of Message 1 (SOM1) | `0xF0` | 
| 1 | Start of Message 2 (SOM2) | `0xAA` | 
| 2 | Packet Length | `0x07` | 
| 3 | CMD: Master Volume | `0x05` | 
| 4 | Volume LSB | `0x00` | 
| 5 | Volume MSB | `0x00` | 
| 6 | End of Message (EOM) | `0x55` | 

---

### Get Status (`0x07`)

The Get Status command requests the status of all tracks currently playing in voices.

#### Example Packet

| Byte Index | Description | Example / Value | 
| ----- | ----- | ----- | 
| 0 | Start of Message 1 (SOM1) | `0xF0` | 
| 1 | Start of Message 2 (SOM2) | `0xAA` | 
| 2 | Packet Length | `0x05` | 
| 3 | CMD: Get Status | `0x07` | 
| 4 | End of Message (EOM) | `0x55` | 

---

### Track Volume (`0x08`)

The Track Volume command sets the gain for a specific playing track.

#### Parameters
* **Track Number:** 16-bit unsigned integer.
* **Volume (Gain):** 16-bit signed integer. The range spans from -70dB to +10dB.

#### Example Packet (Set Track 14 to 0dB)

| Byte Index | Description | Example / Value | 
| ----- | ----- | ----- | 
| 0 | Start of Message 1 (SOM1) | `0xF0` | 
| 1 | Start of Message 2 (SOM2) | `0xAA` | 
| 2 | Packet Length | `0x09` | 
| 3 | CMD: Track Volume | `0x08` | 
| 4 | Track Number LSB | `0x0E` | 
| 5 | Track Number MSB | `0x00` | 
| 6 | Volume LSB | `0x00` | 
| 7 | Volume MSB | `0x00` | 
| 8 | End of Message (EOM) | `0x55` | 

---

### Amp Power (`0x09`)

The Amp Power command turns the onboard audio amplifier on or off.

#### Parameters
* **Power State:** 8-bit boolean integer. `0x00` turns the amplifier off, `0x01` turns it on.

#### Example Packet (Turn Amp On)

| Byte Index | Description | Example / Value | 
| ----- | ----- | ----- | 
| 0 | Start of Message 1 (SOM1) | `0xF0` | 
| 1 | Start of Message 2 (SOM2) | `0xAA` | 
| 2 | Packet Length | `0x06` | 
| 3 | CMD: Amp Power | `0x09` | 
| 4 | Power State | `0x01` (On) | 
| 5 | End of Message (EOM) | `0x55` | 

---

### Track Fade (`0x0A`)

The Track Fade command initiates a smooth volume transition for a specific track over a defined period of time.

#### Parameters
* **Track Number:** 16-bit unsigned integer.
* **Target Volume:** 16-bit signed integer representing the final dB gain level.
* **Fade Time:** 16-bit unsigned integer representing the duration of the fade in milliseconds.
* **Stop Flag:** 8-bit boolean integer. If `0x01`, the track will automatically stop and release its voice when the fade reaches its target. If `0x00`, the track continues playing at the target volume.

#### Example Packet (Fade Track 1 to -50dB over 1000ms and stop)

| Byte Index | Description | Example / Value | 
| ----- | ----- | ----- | 
| 0 | Start of Message 1 (SOM1) | `0xF0` | 
| 1 | Start of Message 2 (SOM2) | `0xAA` | 
| 2 | Packet Length | `0x0C` | 
| 3 | CMD: Track Fade | `0x0A` | 
| 4 | Track Number LSB | `0x01` | 
| 5 | Track Number MSB | `0x00` | 
| 6 | Target Vol LSB | `0xCE` (-50) | 
| 7 | Target Vol MSB | `0xFF` | 
| 8 | Fade Time LSB | `0xE8` (1000) | 
| 9 | Fade Time MSB | `0x03` | 
| 10 | Stop Flag | `0x01` (Stop) | 
| 11 | End of Message (EOM) | `0x55` | 

---

### Resume All Sync (`0x0B`)

*(Unused in RPU-SP)*

---

### Sample Rate Offset (`0x0C`)

*(Unused in RPU-SP)*

---

### Track Control Ex (`0x0D`)

The Track Control Ex command functions exactly like the standard Track Control command (`0x03`) but adds a lock flag. This allows the system to lock the playback of a specific track so it doesn’t get bumped by future requests.

#### Parameters
* **Control Code:** 8-bit integer representing the specific action to perform:
  * `0x00`: Play Solo (stops other tracks)
  * `0x01`: Play Polyphonic
  * `0x02`: Pause
  * `0x03`: Resume
  * `0x04`: Stop
  * `0x05`: Loop On
  * `0x06`: Loop Off
  * `0x07`: Load (pauses at first sample)
* **Track Number:** 16-bit unsigned integer.
* **Lock:** once this track takes a voice, don’t let it be bumped
  * `0x00`: Track can be bumped
  * `0x01`: Track is locked (cannot be bumped)

#### Example Packet (Stop Track 1)

| Byte Index | Description | Example / Value | 
| ----- | ----- | ----- | 
| 0 | Start of Message 1 (SOM1) | `0xF0` | 
| 1 | Start of Message 2 (SOM2) | `0xAA` | 
| 2 | Packet Length | `0x09` | 
| 3 | CMD: Track Control Ex | `0x0D` | 
| 4 | Control Code | `0x04` (Stop) | 
| 5 | Track Number LSB | `0x01` | 
| 6 | Track Number MSB | `0x00` | 
| 7 | Lock | `0x00` | 
| 8 | End of Message (EOM) | `0x55` | 

---

### Set Reporting (`0x0E`)

The Set Reporting command toggles system responses (like track status reporting) on or off.

#### Parameters
* **Reporting Flags:** 8-bit integer. Standard implementation uses `0x01` to enable automatic status reporting and `0x00` to disable it.

#### Example Packet (Enable Reporting)

| Byte Index | Description | Example / Value | 
| ----- | ----- | ----- | 
| 0 | Start of Message 1 (SOM1) | `0xF0` | 
| 1 | Start of Message 2 (SOM2) | `0xAA` | 
| 2 | Packet Length | `0x06` | 
| 3 | CMD: Set Reporting | `0x0E` | 
| 4 | Reporting Flags | `0x01` (Enable) | 
| 5 | End of Message (EOM) | `0x55` | 

---

### Set Trigger Bank (`0x0F`)

*(Unused in RPU-SP)*

---

### Play Ex (`0x60`)

The Play Ex command uses the class system to play a sample in a particular class of sounds. The class (SFX, Callout, Music) can have different behaviors and ducking rules.

#### Parameters
* **Track Number:** 16-bit unsigned integer.
* **Class:** sound class in which to play this track
  * `0x00`: Sound Effect (SFX) - in general, played concurrently with other SFX
  * `0x01`: Callout - in general, staged by priority
  * `0x02`: Music - in general, one at a time and locked and/or looped
* **Flags:** the flags dictate playback
  * `0x01`: loop - defaults to start > finish, infinitely
  * `0x02`: lock - playback will not be bumped
  * `0x04`: solo-in-class - kills other sounds in this class
  * `0x08`: wait for music bar - pauses until music A/B loop
  * `0x10`: crossfade - *used with solo
  * `0x20`: replace earlier - if this particular track is already playing, replace it
  * `0x40`: pause - if pause is set, the track will be assigned a voice and TUID, but will not start playing until it’s triggered with a `0x63` Loop Points command.
* **Gain:** 16-bit signed integer (to match other commands)
  * Overrides class gain
  * typical range = -70 to +10
* **Priority**
  * `0x00 - 0xFF`: higher priority takes precedence
  * For class=SFX, if all voices are in-use, the RPU-SP will loop through voices (in the SFX class) and look for a voice with a lower priority. That voice will be stopped and replaced with this sound.
  * For class=Callout, if a callout is being played of equal or higher priority, this callout will be inserted in the queue based on priority. If the playing callout is lower priority, it will be replaced with this callout and the queue will resume when this callout has finished.
  * For class=music, this parameter is unused. Music tracks are always replaced.

#### Example Packet (Track 1, Class 0, Flags 0x20, Gain -10, Priority 10)

| Byte Index | Description | Example / Value | 
| ----- | ----- | ----- | 
| 0 | Start of Message 1 (SOM1) | `0xF0` | 
| 1 | Start of Message 2 (SOM2) | `0xAA` | 
| 2 | Packet Length | `0x0C` | 
| 3 | CMD: Play Ex | `0x60` | 
| 4 | Track Number LSB | `0x01` | 
| 5 | Track Number MSB | `0x00` | 
| 6 | Class | `0x00` | 
| 7 | Flags | `0x20` | 
| 8 | Gain LSB | `0xF6` | 
| 9 | Gain MSB | `0xFF` | 
| 10 | Priority | `0x0A` | 
| 11 | End of Message (EOM) | `0x55` | 
  
---

### Class Gain (`0x61`)

The Class Gain command sets a new gain for a class and (optionally) a fade time in milliseconds to ramp into that new gain.

#### Parameters
* **Class:** sound class in which to play this track
  * `0x00`: Sound Effect (SFX)
  * `0x01`: Callout
  * `0x02`: Music
* **Gain:** 16-bit signed integer (to match other commands)
  * New class gain
  * typical range = -70 to +10
* **Fade Time:** 16-bit unsigned integer 
  * 0 for instantly setting gain of class
  * 1-65535 for number of milliseconds to fade into new gain

#### Example Packet (Class 0, Gain -10, Fade Time 1s)

| Byte Index | Description | Example / Value | 
| ----- | ----- | ----- | 
| 0 | Start of Message 1 (SOM1) | `0xF0` | 
| 1 | Start of Message 2 (SOM2) | `0xAA` | 
| 2 | Packet Length | `0x0A` | 
| 3 | CMD: Class Gain | `0x61` | 
| 4 | Class | `0x00` | 
| 5 | Gain LSB | `0xF6` | 
| 6 | Gain MSB | `0xFF` | 
| 7 | Fade Time LSB | `0xE8` | 
| 8 | Fade Time MSB | `0x03` | 
| 9 | End of Message (EOM) | `0x55` | 
  
---

### Duck Rule (`0x62`)

The Duck Rule command creates a behavior where playing a sample in one class will “duck” (typically, attenuate) the volume of another class. The most common application is to duck SFX and Music when a callout is played, although the implementation is flexible to allow other uses.

#### Parameters
* **Ducked Class:** sound class to duck (typically SFX and Music)
  * `0x00`: Sound Effect (SFX)
  * `0x01`: Callout
  * `0x02`: Music
* **Ducking Class:** sound class that will cause the ducking (typically Callout)
  * `0x00`: Sound Effect (SFX)
  * `0x01`: Callout
  * `0x02`: Music
* **Gain:** 8-bit signed integer 
  * Attenuation is assumed, so a positive value will **reduce** the gain of the Ducked Class
  * typical value = 20 (reduce gain of Ducked Class by 20dB while a sample is played in Ducking Class)
* **Attack Time:** 16-bit unsigned integer 
  * 0 for instantly setting new gain of Ducked Class
  * 1-65535 for number of milliseconds to fade into new gain
* **Release Time:** 16-bit unsigned integer 
  * 0 for instantly setting new gain of Ducked Class once Ducking Class has finished
  * 1-65535 for number of milliseconds to fade into new gain

#### Example Packet (Duck SFX under Callouts by 20dB, with 1 second fade in/out)

| Byte Index | Description | Example / Value | 
| ----- | ----- | ----- | 
| 0 | Start of Message 1 (SOM1) | `0xF0` | 
| 1 | Start of Message 2 (SOM2) | `0xAA` | 
| 2 | Packet Length | `0x0C` | 
| 3 | CMD: Duck Rule | `0x62` | 
| 4 | Ducked Class | `0x00` | 
| 5 | Ducking Class | `0x01` | 
| 6 | Gain | `0x14` | 
| 7 | Attack Time LSB | `0xE8` | 
| 8 | Attack Time MSB | `0x03` | 
| 9 | Release Time LSB | `0xE8` | 
| 10 | Release Time MSB | `0x03` | 
| 11 | End of Message (EOM) | `0x55` | 
  
---

### Loop Points (`0x63`)

The Loop Points command sets up looping for a given track (or TUID). If multiple voices are playing the same track and “Use TUID” is false, then they will all get the loop points applied (based on Track Number). If a track is currently looping, and the loop points are updated, they will be kept as pending until Loop Point B is reached. Using Loop Points to update an already-looping track allows the controller to jump to different loops in the same track without re-staging the track with Play Ex and needing to track the TUID of the track. If the playing track doesn’t have a Loop Point B, then new looping point will be adopted at the end of the track.

#### Parameters
* **Flags:** playback flags
  * `0x01`: Start Playback (this will start a track loaded with Play Ex)
  * `0x02`: Start at top (***1:*** start at sample 0, ***0:*** start at A)
  * `0x04`: Play through end (***1:*** after loops, play until end of file, ***0:*** end at B)
  * `0x08`: Alert each Loop (***1:*** send an alert each time B is approached, ***0:*** only alert on final B)
  * `0x10`: Use TUID (***0:*** use Track Number, ***1:*** use TUID)
  * `0x20`: Update Loop Count only (all other parameters except Track Number/TUID are ignored)
  * `0x40`: Update Next Track only (all other parameters except Track Number/TUID are ignored)
  * Note: flags 0x20 and 0x40 may be used in conjunction to end the loop and set the next track
  * `0x80`: Next Track is TUID (***0:*** Next Track is a 16-bit track number, ***1:*** Next Track is a 32-bit TUID)
* **Loop Count:** 8-bit unsigned integer
  * `0x00`: play through
  * `0x01` - `0xFE`: repeat Loop Count times
  * `0xFF`: repeat forever
* **A:** 32-bit unsigned integer
  * `0x00000000`: Loop point A is start of file
  * `0x00000001 - 0xFFFFFFFF`: Loop point number of ticks (each tick is 1/44100 of a second)
* **B:** 32-bit unsigned integer
  * `0xFFFFFFFF`: Loop point B is end of file
  * `0x00000001 - 0xFFFFFFFE`: Loop point number of ticks (each tick is 1/44100 of a second)
* **Fade In:** 16-bit unsigned integer
  * `0`: No fade in
  * `1-65534`: number of milliseconds to fade in after returning to A Loop point
* **Fade Out:** 16-bit unsigned integer
  * `0`: No fade out
  * `1-65534`: number of milliseconds to fade out before reaching B Loop point
* **Alert Margin:** 16-bit unsigned integer
  * `0`: No alert
  * `1-65535`: number of milliseconds to send alert before reaching B Loop point
* **Next Track Number / Next Track TUID:** Track Number (or TUID) to start when this track has finished
  * `0`: No following track
  * `1-65535`: Track Number - this must be staged with Play Ex and waiting to start
  * or
  * `0x00010000 - 0xFFFFFFFF`: TUID - from a track staged with Play Ex
* **Track Number / TUID:** either a 16-bit unsigned integer (Track Number) or 32-bit unsigned integer (TUID)

#### Example Packet (Start Track Number 200 from top of file, through A (00:01), to B (00:02), back to A, stop at B, alert 500ms before each time B is reached)

| Byte Index | Description | Example / Value | 
| ----- | ----- | ----- | 
| 0 | Start of Message 1 (SOM1) | `0xF0` | 
| 1 | Start of Message 2 (SOM2) | `0xAA` | 
| 2 | Packet Length | `0x19` | 
| 3 | CMD: Loop Points | `0x63` | 
| 4 | Flags | `0x0B` | 
| 5 | Loop Count | `0x02` | 
| 6 | A byte 0 | `0x44` | 
| 7 | A byte 1 | `0xAC` | 
| 8 | A byte 2 | `0x00` | 
| 9 | A byte 3 | `0x00` | 
| 10 | B byte 0 | `0x88` | 
| 11 | B byte 1 | `0x58` | 
| 12 | B byte 2 | `0x01` | 
| 13 | B byte 3 | `0x00` | 
| 14 | Fade In LSB | `0x00` | 
| 15 | Fade In MSB | `0x00` | 
| 16 | Fade Out LSB | `0x00` | 
| 17 | Fade Out MSB | `0x00` | 
| 18 | Alert Margin LSB | `0xF4` | 
| 19 | Alert Margin MSB | `0x01` | 
| 20 | Next Track LSB | `0x00` | 
| 21 | Next Track MSB | `0x00` | 
| 22 | Track Number LSB | `0xC8` | 
| 23 | Track Number MSB | `0x00` | 
| 24 | End of Message (EOM) | `0x55` | 


#### Example Packet (Start TUID 70000 from A (00:15) to B (02:00) forever, alert 500ms before finish)

| Byte Index | Description | Example / Value | 
| ----- | ----- | ----- | 
| 0 | Start of Message 1 (SOM1) | `0xF0` | 
| 1 | Start of Message 2 (SOM2) | `0xAA` | 
| 2 | Packet Length | `0x1B` | 
| 3 | CMD: Loop Points | `0x63` | 
| 4 | Flags | `0x11` | 
| 5 | Loop Count | `0xFF` | 
| 6 | A byte 0 | `0xFC` (661500 ticks = 15 s) | 
| 7 | A byte 1 | `0x17` | 
| 8 | A byte 2 | `0x0A` | 
| 9 | A byte 3 | `0x00` | 
| 10 | B byte 0 | `0xE0` (5292000 ticks = 120 s) | 
| 11 | B byte 1 | `0xBF` | 
| 12 | B byte 2 | `0x50` | 
| 13 | B byte 3 | `0x00` | 
| 14 | Fade In LSB | `0x00` | 
| 15 | Fade In MSB | `0x00` | 
| 16 | Fade Out LSB | `0x00` | 
| 17 | Fade Out MSB | `0x00` | 
| 18 | Alert Margin LSB | `0xF4` | 
| 19 | Alert Margin MSB | `0x01` | 
| 20 | Next Track LSB | `0x00` | 
| 21 | Next Track MSB | `0x00` | 
| 22 | TUID byte 0 | `0x70` | 
| 23 | TUID byte 1 | `0x11` | 
| 24 | TUID byte 2 | `0x01` | 
| 25 | TUID byte 3 | `0x00` | 
| 26 | End of Message (EOM) | `0x55` | 


---

### Get Soundtrack Folders (`0x64`)

The Get Soundtrack Folders command will search the SD card and return the number of folders (+1) that match the name “AltSoundtrackNNN” with NNN being a number between 1-255. Soundtrack 0 is assumed to be the root of the SD card, so this function will always return >= 1. The “AltSoundtrackNNN” names don’t have to be sequential, so a card might have AltSoundtrack001, AltSoundtrack3, and AltSoundtrack10. The function would return 4 in this case.

#### Parameters
* None
  
#### Example Packet (Request number of folders)

| Byte Index | Description | Example / Value | 
| ----- | ----- | ----- | 
| 0 | Start of Message 1 (SOM1) | `0xF0` | 
| 1 | Start of Message 2 (SOM2) | `0xAA` | 
| 2 | Packet Length | `0x05` | 
| 3 | CMD: Get Soundtrack Folders | `0x64` | 
| 4 | End of Message (EOM) | `0x55` | 
    
  
---

### Stop Class (`0x65`)

The Stop Class command will stop all the playing tracks in a given class. This command stops actively playing tracks and erases any queued tracks (queued with Play Ex and the 0x40 flag).

#### Parameters
* **Class:** sound class to stop
  * `0x00`: Sound Effect (SFX)
  * `0x01`: Callout
  * `0x02`: Music
* **Fade Time:** 16-bit unsigned integer 
  * 0 for instantly stopping the class tracks
  * 1-65535 for number of milliseconds to fade before stopping the class tracks

#### Example Packet (Class 0, Fade Time 1s)

| Byte Index | Description | Example / Value | 
| ----- | ----- | ----- | 
| 0 | Start of Message 1 (SOM1) | `0xF0` | 
| 1 | Start of Message 2 (SOM2) | `0xAA` | 
| 2 | Packet Length | `0x08` | 
| 3 | CMD: Stop Class | `0x65` | 
| 4 | Class | `0x00` | 
| 5 | Fade Time LSB | `0xE8` | 
| 6 | Fade Time MSB | `0x03` | 
| 7 | End of Message (EOM) | `0x55` | 
  
---

### Set Events Flags (`0x66`)

The Set Events Flags command tells the RPU-SP which events that the controller wants responses for. The messages can be specified by class.

#### Parameters
* **Mask:** 8-bit unsigned integer 
  * `0x01`: Finished - send a message when a sample finishes
  * `0x02`: Looped
  * `0x04`: Music Changed (only pertains to music class)
  * `0x08`: Ducking Change (on/off)
  * `0x10`: Started - sample moved from queued to playing
  * `0x20`: Staged and Paused - sample was given a voice slot and TUID
* **Class:** 8-bit unsigned integer
  * `0x00`: Sound Effect (SFX)
  * `0x01`: Callout
  * `0x02`: Music
   
#### Example Packet (Class 2 (Music), Alert when music loops or changes)

| Byte Index | Description | Example / Value | 
| ----- | ----- | ----- | 
| 0 | Start of Message 1 (SOM1) | `0xF0` | 
| 1 | Start of Message 2 (SOM2) | `0xAA` | 
| 2 | Packet Length | `0x07` | 
| 3 | CMD: Set Events Flags | `0x66` | 
| 4 | Mask | `0x06` | 
| 5 | Class | `0x02` | 
| 6 | End of Message (EOM) | `0x55` | 
  
---

### Set Soundtrack Folder (`0x67`)

The Set Soundtrack Folder tells the RPU-SP to use a folder on the SD card instead of looking for WAV files in the root. Soundtrack folders must be named “AltSoundtrackNNN”, with NNN being an integer between 1-255. Soundtrack 0 is the default and is assumed to be the root of the SD card. The soundtrack folder names do not need to be contiguous. If an SD card contains the folders AltSoundtrack005 and AltSoundtrack20, the number of folders will be calculated to be 3 (the number of folders + the root), and Set Soundtrack Folder of 1 would set folder AltSoundtrack005 as the active folder (ordered by number without leading zeroes, instead of strictly alpha). If a folder contains an “example.wav”, that can be played to give the operator an idea of the folder’s contents.

#### Parameters
* **Action:** 8-bit unsigned integer of flags
  * `0x01`: switch to new folder
  * `0x02`: pass through to root (if a sample doesn’t exist in folder, map to root folder)
  * `0x80`: play folder’s “example.wav” 
* **Folder Num:** 8-bit unsigned integer 
   
#### Example Packet (Switch to folder “AltSoundtrack2” assuming it’s the 2nd folder on the SD card, pass through missing tracks to root)

| Byte Index | Description | Example / Value | 
| ----- | ----- | ----- | 
| 0 | Start of Message 1 (SOM1) | `0xF0` | 
| 1 | Start of Message 2 (SOM2) | `0xAA` | 
| 2 | Packet Length | `0x07` | 
| 3 | CMD: Set Soundtrack Folder | `0x67` | 
| 4 | Action | `0x03` | 
| 5 | Folder Num | `0x02` | 
| 6 | End of Message (EOM) | `0x55` | 

  
---


### Synth Control (`0x70`)

If this RPU-SP can emulate a synthesizer (SB-300, Squawk & Talk, DCS, etc.), this command packet will encapsulate the control commands for that card.

#### Parameters
* **Synth Command:** 8-bit unsigned integer 
  * `0x01`: Get Volume (get a synth volume modifier - this is a left/right scale with 256 being zero change)
    * Adds 8-bit unsigned integer for volume ID
      * `0x01`: Synth Volume
      * `0x02`: DAC Volume
      * `0x03`: CVSD Volume
  * `0x02`: Set Volume (set a synth volume L/R modifier)
    * Adds 8-bit unsigned integer for volume ID
      * `0x01`: Synth Volume
      * `0x02`: DAC Volume
      * `0x03`: CVSD Volume
    * Adds 16-bit signed integer for left modifier (256=no change)
      * LSB: lower byte of left modifier
      * MSB: upper byte of left modifier
    * Adds 16-bit signed integer for right modifier (256=no change)
      * LSB: lower byte of right modifier
      * MSB: upper byte of right modifier
  * `0x03`: Get Num ROMs
  * `0x04`: Get ROM Name
    * Adds 8-bit unsigned integer for ROM Number
  * `0x05`: Get Current ROM
  * `0x06`: Flash ROM
    * Adds 8-bit unsigned integer for ROM number
  * `0x80`: Synth Available
    * Adds 16-bit unsigned integer for Synth Type:
      * `0x0201`: SB100 sound card emulation
      * `0x0202`: SB300 sound card emulation
      * `0x0301`: WMS Type 1 sound card emulation
      * `0x0302`: WMS Type 2 sound card emulation
      * `0x0A01`: A-12738 sound card emulation
      * `0x0A02`: DCS sound card emulation
  * `0x81`: Set Synth Status
    * Adds 16-bit unsigned integer for Synth Type (see above)
    * Adds 8-bit unsigned integer for Synth Status
      * `0x00`: de-activate synth
      * `0x01`: activate synth
  * `0x82`: Send Synth Command
    * Adds 8-bit unsigned integer for command
   
#### Example Packet (Pan DAC halfway to the left)

| Byte Index | Description | Example / Value | 
| ----- | ----- | ----- | 
| 0 | Start of Message 1 (SOM1) | `0xF0` | 
| 1 | Start of Message 2 (SOM2) | `0xAA` | 
| 2 | Packet Length | `0x0B` | 
| 3 | CMD: Synth Control | `0x70` | 
| 4 | Synth Command - Set Volume | `0x02` | 
| 5 | Vol Type - DAC | `0x02` | 
| 6 | Left Vol LSB | `0x00` | 
| 7 | Left Vol MSB | `0x01` | 
| 8 | Right Vol LSB | `0x80` | 
| 9 | Right Vol MSB | `0x00` | 
| 10 | End of Message (EOM) | `0x55` | 
  
---


## 5. Responses - Full Description

### Version String (`0x81`)

The Version String is returned by the RPU-SP after receiving a Get Version (`0x01`) command. 

#### Parameters
* **Version Data:**
  * A variable-length array of ASCII characters representing the firmware version. 
  * The string always starts with `RPU-SP`. A controller that sees `RPU-SP` can use commands `0x60` and up; anything else (the WAV Trigger Firmware answers `"WAV Trigger v1.34   "`, 20 characters padded with spaces) gets only the WAV Trigger commands.

#### Example Packet (Version "RPU-SP FW   v1.00 ")
| Byte Index | Description | Example / Value | 
| ----- | ----- | ----- | 
| 0 | Start of Message 1 (SOM1) | `0xF0` | 
| 1 | Start of Message 2 (SOM2) | `0xAA` | 
| 2 | Packet Length | `0x17` | 
| 3 | RSP: Version String | `0x81` | 
| 4-21 | (version str) | "RPU-SP FW   v1.00 " |  
| 22 | End of Message (EOM) | `0x55` | 

---

### System Info (`0x82`)

The System Info response is returned by the RPU-SP after receiving a Get Sys Info (`0x02`) command. It reports the hardware/firmware capabilities.

#### Parameters
* **Number of Voices:** 8-bit unsigned integer representing the maximum polyphonic voices supported.
* **Number of Tracks:** 16-bit unsigned integer representing the total playable tracks found on the storage media.

#### Example Packet (24 Voices, 4096 Tracks)

| Byte Index | Description | Example / Value | 
| ----- | ----- | ----- | 
| 0 | Start of Message 1 (SOM1) | `0xF0` | 
| 1 | Start of Message 2 (SOM2) | `0xAA` | 
| 2 | Packet Length | `0x08` | 
| 3 | RSP: System Info | `0x82` | 
| 4 | Voices | `0x18` (24) | 
| 5 | Tracks LSB | `0x00` | 
| 6 | Tracks MSB | `0x10` (4096) | 
| 7 | End of Message (EOM) | `0x55` | 

---

### Status (`0x83`)

The Status response is returned by the RPU-SP after receiving a Get Status (`0x07`) command. It provides a list of all track numbers currently occupying a voice.

#### Parameters
* **Playing Tracks:**
  * A variable-length array of 16-bit unsigned integers representing the track numbers currently playing. Sent Little-Endian.
  * These are the real track numbers (track 2 = `0x0002`), as on the WAV Trigger.

#### Example Packet (Tracks 1 and 14 Playing)

| Byte Index | Description | Example / Value | 
| ----- | ----- | ----- | 
| 0 | Start of Message 1 (SOM1) | `0xF0` | 
| 1 | Start of Message 2 (SOM2) | `0xAA` | 
| 2 | Packet Length | `0x09` | 
| 3 | RSP: Status | `0x83` | 
| 4 | Track 1 LSB | `0x01` | 
| 5 | Track 1 MSB | `0x00` | 
| 6 | Track 14 LSB | `0x0E` | 
| 7 | Track 14 MSB | `0x00` | 
| 8 | End of Message (EOM) | `0x55` | 

---

### Track Report (`0x84`)

The Track Report response is transmitted automatically by the RPU-SP (if reporting is enabled via `0x0E`) whenever a track changes state.

#### Parameters
* **Track Number:** 16-bit unsigned integer, counted from 0 (the track number minus 1).
* **Voice Number:** 8-bit unsigned integer, the voice the track is playing on.
* **Voice Status:** 8-bit integer indicating the track's current state (e.g., `0x00` for Stopped/Finished, `0x01` for Playing).

#### Example Packet (Track 1 Stopped, on voice 0)

| Byte Index | Description | Example / Value | 
| ----- | ----- | ----- | 
| 0 | Start of Message 1 (SOM1) | `0xF0` | 
| 1 | Start of Message 2 (SOM2) | `0xAA` | 
| 2 | Packet Length | `0x09` | 
| 3 | RSP: Track Report | `0x84` | 
| 4 | Track Number LSB | `0x00` (track 1, counted from 0) | 
| 5 | Track Number MSB | `0x00` | 
| 6 | Voice Number | `0x00` | 
| 7 | Voice Status | `0x00` (Stopped) | 
| 8 | End of Message (EOM) | `0x55` | 

---

### Event (`0x8A`)

The Event response is sent when a specific system or game event occurs (configured via command `0x66`).

#### Parameters
* **Event ID:** 8-bit unsigned integer identifying the event type.
  * `0x01`: Track Finished - playback has finished
  * `0x02`: Looping Event - track will loop in Alert Margin milliseconds (based on Loop Points command)
  * `0x03`: Music Changed - music class track has changed
  * `0x04`: Ducking Change - returned class ducking has changed, adds 8-bit value for ducking status
  * `0x05`: Started - Track with given Number and TUID has started
  * `0x06`: Staged and Paused - Track staged in a voice slot with returned TUID
* **Track Number:** 16-bit unsigned integer
  * 0 - event not associated with a track
  * 1-65535 - Track Number associated with event
* **TUID:** 32-bit unsigned integer (aka Track Unique Identifier)
  * 0 - event not associated with track
  * 0x00010000 - 0xFFFFFFFF - TUID of track
* **Class:** 8-bit unsigned integer
  * `0x00`: Sound Effect (SFX)
  * `0x01`: Callout
  * `0x02`: Music
* **Event Data:** Variable-length payload depending on the specific event ID.
  * (for Event ID `0x01`), there is no additional data
  * (for Event ID `0x02`), **Loop Count:** 8-bit unsigned integer
  * (for Event ID `0x03`), there is no additional data
  * (for Event ID `0x04`), **Ducking Status:** 8-bit unsigned integer
    * `0x00`: Class is no longer ducked
    * `0x01`: Class is ducked
  * (for Event ID `0x05`), there is no additional data
  * (for Event ID `0x06`), there is no additional data

#### Example Packet (Track Number 4 (TUID=0x00011170) will loop, and has 3 loops left)

| Byte Index | Description | Example / Value | 
| ----- | ----- | ----- | 
| 0 | Start of Message 1 (SOM1) | `0xF0` | 
| 1 | Start of Message 2 (SOM2) | `0xAA` | 
| 2 | Packet Length | `0x0E` | 
| 3 | RSP: Event | `0x8A` | 
| 4 | Event ID | `0x02` | 
| 5 | Track Number LSB | `0x04` | 
| 6 | Track Number MSB | `0x00` | 
| 7 | TUID byte 0 | `0x70` | 
| 8 | TUID byte 1 | `0x11` | 
| 9 | TUID byte 2 | `0x01` | 
| 10 | TUID byte 3 | `0x00` | 
| 11 | Class | 0x02 |
| 12 | Event Data - Loop Count | 0x03 |
| 13 | End of Message (EOM) | `0x55` | 

---

### Soundtrack (`0x8B`)

The Soundtrack Response is used to report on the number of soundtracks available on the SD card.

#### Parameters
* **Soundtrack Count:** 8-bit unsigned integer

#### Example Packet (SD card has 2 AltSoundtrackNNN folders in addition to the root)

| Byte Index | Description | Example / Value | 
| ----- | ----- | ----- | 
| 0 | Start of Message 1 (SOM1) | `0xF0` | 
| 1 | Start of Message 2 (SOM2) | `0xAA` | 
| 2 | Packet Length | `0x06` | 
| 3 | RSP: Soundtrack | `0x8B` | 
| 4 | Soundtrack Count | `0x03` | 
| 5 | End of Message (EOM) | `0x55` | 

---

### Synth Response (`0x8F`)

The Synth Response carries telemetry or state information originating specifically from the synthesizer card.

#### Parameters
* **Synth Command:** This tells the controller what command is being responded to
* (for Synth Command `0x01`, Get Volume)
  * **Volume ID:** 8-bit integer
    * `0x01`: Synth Volume
    * `0x02`: DAC Volume
    * `0x03`: CVSD Volume
  * **Left Modifier:**
    * LSB: lower byte of Left modifier
    * MSB: upper byte of Left modifier
  * **Right Modifier:**
    * LSB: lower byte of Right modifier
    * MSB: upper byte of Right modifier
* (for Synth Command `0x03`, Get Num ROMs)
  * **Number of ROMs:** 8-bit unsigned integer
* (for Synth Command `0x04` or `0x05`, Get ROM Name)
  * **ROM Name:** 64-character buffer
* (for Synth Command `0x06`, Flash ROM)
  * **Percent Complete:** 8-bit unsigned integer
    * `0-100`: progress (sent in 10-point increments)
    * `0xFF`: Flash failed
* (for Synth Command `0x80`, Synth Available, or `0x81`, Set Synth Status)
  * **Synth Type:** 16-bit unsigned integer of the synth type
    * LSB: lower byte of Synth Type
    * MSB: upper byte of Synth Type
  * **Synth Capability:** 8-bit unsigned integer
    * `0x00`: Synth not supported
    * `0x01 - 0xFF`: Synth supported flags
      * `0x01`: Synth available
      * `0x80`: Synth currently on
* (for Synth Command `0x82`)
  * **Synth Response:** 8-bit unsigned integer
    * response byte from the synth

#### Example Packet (Response for Get Current ROM, “Hurricane")

| Byte Index | Description | Example / Value | 
| ----- | ----- | ----- | 
| 0 | Start of Message 1 (SOM1) | `0xF0` | 
| 1 | Start of Message 2 (SOM2) | `0xAA` | 
| 2 | Packet Length | `0x46` | 
| 3 | RSP: Synth Response | `0x8F` | 
| 4 | Synth Command | `0x05` | 
| 5-68 | ROM Name | “Hurricane" | 
| 69 | End of Message (EOM) | `0x55` |


