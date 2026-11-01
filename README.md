# Night Owl Radio

A WiFi internet radio built on an ESP32-S3, styled as a modern take on the
1980s red-LED clock radio. It plays an internet station, music from a microSD
card, or whatever your Windows laptop is playing, through two small speakers.
A 0.96" OLED shows a 7-segment clock, now-playing and music visualisers, and a
single rotary knob runs it. Everything else is in a web page served by the radio.

```
firmware/RadioOLED     the radio (Arduino sketch)
firmware/EncoderProbe  test sketch: shows which header pins the knob is on
app/NightOwlLink       Windows app that streams the laptop's sound to the radio
enclosure              3D-printable case (STL)
```

## Features

- **Sources:** internet radio (MP3/AAC over HTTP/HTTPS), microSD card (MP3, M4A/AAC,
  FLAC, WAV, OGG) and **Laptop** (through the Night Owl Link app).
- **Tone control:** our own DSP on the decoded audio. A 90 Hz speaker-protection
  high-pass, bass shelf at 180 Hz, mid peak at 1.2 kHz and treble shelf at 5 kHz
  (±12 dB each), with automatic headroom and a peak limiter, so boosting a
  band does not make everything else quieter or clip.
- **OLED (128×64, SSD1306):** a 7-segment clock with date, a now-playing screen with
  a scrolling title, and four visualisers (LED spectrum, mirrored spectrum, twin VU
  needles, oscilloscope). Drawn at 25 fps on the second core so playback never stutters.
  Dims at night and shifts the clock slightly so it does not burn in.
- **Clock:** time and date from NTP (set to South African time; change `TZ_INFO`).
- **Web UI** at `http://nightowl-radio.local/`: volume, play/pause, tone, source,
  SD track list with search, shuffle and repeat, and which screens the OLED rotates.
- **Rotary knob:** turn for volume, click for play/pause, double-click for next
  track, long-press for a menu (bass › mid › treble › source).
- Volume, tone, source and display settings survive a power cut.

## Hardware

| Part | Notes |
|---|---|
| [Waveshare ESP32-S3-SIM7670G-4G](https://www.waveshare.com/esp32-s3-sim7670g-4g.htm) | 16 MB flash, PSRAM, microSD slot, 18650 holder |
| MAX98357A I2S amplifier | powered from 5 V |
| 2 × 8 Ω 5 W enclosed speakers | |
| 0.96" SSD1306 OLED, I2C | address 0x3C |
| KY-040 style rotary encoder with push button | |

### Wiring

| Signal | GPIO | Notes |
|---|---|---|
| Amp BCLK / LRC / DIN | 39 / 40 / 42 | |
| OLED SDA / SCL | 10 / 9 | |
| Encoder CLK | 1 | |
| Encoder DT | 33 | Printed **IO33** on the header. Waveshare's pinout shows GPIO2 at that position, so check your board's labels. Run `EncoderProbe` if the knob does nothing. |
| Encoder SW | 21 | |
| Encoder + | 3V3 | **not** 5 V |
| microSD | 4, 5, 6 (card detect 46) | on-board slot |

The pins are set at the top of `RadioOLED.ino`. If clockwise turns the volume
down, set `ENC_REVERSE = true`. If one click moves two steps, set
`ENC_STEPS_PER_DETENT = 2`.

## Building the firmware

1. Arduino IDE 2 or `arduino-cli`, with the **esp32 core 3.3.x** by Espressif.
2. Libraries: **ESP32-audioI2S 4.0.1** (schreibfaul1), **Adafruit SSD1306**, **Adafruit GFX**.
3. Copy `firmware/RadioOLED/secrets.example.h` to `secrets.h` and fill in your WiFi.
4. Board **ESP32S3 Dev Module** with: Flash Size **16MB**, Partition Scheme
   **16M Flash (3MB APP/9.9MB FATFS)**, PSRAM **QSPI PSRAM**, USB CDC On Boot **Disabled**.

```
arduino-cli compile -b "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=enabled,CDCOnBoot=default" firmware/RadioOLED
arduino-cli upload -p COM5 -b "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=enabled,CDCOnBoot=default" firmware/RadioOLED
```

The station is `STREAM_URL` near the top of the sketch.

## Night Owl Link (laptop audio)

The ESP32-S3 only has Bluetooth Low Energy, so it cannot be a regular Bluetooth
speaker. Night Owl Link does the job over WiFi instead:

1. It captures what Windows is playing (WASAPI loopback), whatever the app.
2. It encodes 320 kbps MP3 and serves it as a live stream on port 8090.
3. It tells the radio to play that stream. The radio plays it like an internet
   station, so tone control, visualisers and the knob all work on it.
4. It keeps the radio's buffer at about a quarter of a second by trimming its send
   rate, so the delay stays around half a second and does not creep up.
5. It sends the song title from Windows' media controls (Spotify, browsers, ...)
   to the OLED.

**Requirements:** Windows 10 2004 or later, .NET 10.

```
cd app/NightOwlLink
dotnet run -c Release
```

The app finds the radio as `nightowl-radio.local`; type its IP address if your
network does not resolve that. When Windows Firewall asks, allow it on
**private** networks, otherwise the radio cannot reach the laptop. The delay
(about 0.5 s) is fine for music, but too much for watching video.

### Radio API used by the app

| Call | Does |
|---|---|
| `GET /api/laptop?url=<stream>&title=<t>&go=1` | registers the stream and switches to it |
| `GET /api/laptop/ping?title=<t>` | keeps the Laptop source alive (it times out after 15 s), updates the title |
| `GET /api/laptop/stop` | app stopped; the radio goes back to its station |
| `GET /api/status` | everything, as JSON (`ibuf` = bytes waiting in the radio's buffer) |

## Enclosure

A low, wide case in the clock-radio style: 250 × 130 × 65 mm, with a ribbed
speaker grille, a smoked front lens for the OLED and a dial for the knob.
Designed to print **without supports**:

| File | Print |
|---|---|
| `ClockRadio_Housing.stl` | 1×, **upside-down** (top on the bed). Needs a bed of 256 mm or larger |
| `ClockRadio_BasePlate.stl` | 1×, flat |
| `ClockRadio_Dial.stl` | 1×, upside-down |
| `ClockRadio_SpeakerStrap_print2.stl` | 2× |
| `ClockRadio_Foot_TPU_print4.stl` | 4× in TPU, or use stick-on rubber feet |
| `ClockRadio_Lens.stl` | optional; laser-cut smoked red acrylic looks better |

Hardware: 4 × M3×10 screws for the base, 4 small self-tapping screws for the
speaker straps. The speaker cradles are sized for 100 × 45 × 21 mm speakers, so
check yours before printing. The encoder hole (7 mm) and dial bore (6.1 mm D-shaft)
are tight; ream them if your printer prints small.

## License

[MIT](LICENSE)
