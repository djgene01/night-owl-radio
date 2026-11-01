// Internet radio: MP3 over WiFi -> ESP32-S3 -> MAX98357A I2S amp, with a web UI.
//   - OLED rotates between a 7-segment clock, now-playing and music visualisers
//     (LED spectrum, mirrored spectrum, twin VU needles, oscilloscope)
//   - web UI: volume, mute, pause, reconnect, 3-band tone control (bass/mid/treble)
//   - display settings from the web UI: pick which OLED screens rotate, how long
//     each stays, show one now, jump to new songs, night dimming
//   - MP3 player: music from the microSD slot, chosen and controlled from the web
//     UI (track list with search, prev/play/next, seek, shuffle, repeat)
//   - tone control is our own DSP (speaker high-pass, shelves, peak, limiter),
//     run on the decoded samples before the library's volume stage
//   - time and date from NTP, South African time (SAST, UTC+2, no DST)
//   - Laptop source: the "Night Owl Link" Windows app serves the laptop's sound as a
//     live 320 kbps MP3 stream over WiFi and the radio plays it like a web stream
//     (API: /api/laptop?url=..&title=..&go=1, /api/laptop/ping, /api/laptop/stop)
//   - found on the LAN as nightowl-radio.local (mDNS)
//
// Tested with ESP32-audioI2S 4.0.1 + esp32 core 3.3.12, ESP32-S3-SIM7670G-4G.
// Build: Flash Size 16MB, Partition "16M Flash (3MB APP/9.9MB FATFS)",
//        PSRAM = QSPI PSRAM, USB CDC On Boot = Disabled.
//
// MAX98357A: BCLK GPIO39, LRC GPIO40, DIN GPIO42   (power it from 5V, not 3V3)
// OLED:      SDA GPIO10 (header pin 25), SCL GPIO9 (header pin 23)
// microSD:   on-board slot, SD_MMC 1-bit: CLK GPIO5, CMD GPIO4, D0 GPIO6.
//            FAT32 card; MP3 (also M4A/AAC/FLAC/WAV/OGG) in any folder, 3 levels deep.
// Encoder:   KY-040 style. CLK GPIO1 (header pin 20), DT GPIO33 (pin 16, printed IO33),
//            SW GPIO21 (pin 22), + to 3V3 (never 5V), GND to GND.
//            turn = volume, click = play/pause, double-click = next track,
//            long press = menu: bass > mid > treble > source (turn adjusts, click = next item)
//
// http://<ip printed on serial>/   No authentication: keep it on your LAN.

#include <WiFi.h>
#include <Wire.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Audio.h>
#include <FS.h>
#include <SD_MMC.h>
#include <driver/gpio.h>
#include <ESPmDNS.h>
#include <vector>
#include <algorithm>

#include "secrets.h"
#include "radio_types.h"

// ---- pins -------------------------------------------------------------------
static const int SDA_PIN  = 10;   // header pin 25
static const int SCL_PIN  = 9;    // header pin 23
static const int I2S_BCLK = 39;
static const int I2S_LRC  = 40;
static const int I2S_DIN  = 42;
static const int SD_CLK   = 5;    // on-board microSD slot (SD_MMC, 1-bit)
static const int SD_CMD   = 4;
static const int SD_D0    = 6;
static const int SD_CD    = 46;   // card-detect switch in the slot
static const int ENC_CLK  = 1;    // header pin 20
static const int ENC_DT   = 33;   // header pin 16: printed IO33 on this board revision
                                  // (Waveshare's pinout shows GPIO2 there); free with quad PSRAM
static const int ENC_SW   = 21;   // header pin 22, button pulls to GND
static const int  ENC_STEPS_PER_DETENT = 4;   // set to 2 if one click moves two volume steps
static const bool ENC_REVERSE = false;        // set to true if clockwise turns the volume down

static const char* STREAM_URL = "https://nightowlfm.adminsoftware.co.za/stream.mp3";
static const char* RADIO_NAME = "Night Owl FM";

// South Africa: SAST, UTC+2 all year (POSIX TZ sign is inverted)
static const char* TZ_INFO = "SAST-2";
static const char* NTP_1   = "za.pool.ntp.org";
static const char* NTP_2   = "pool.ntp.org";
static const char* NTP_3   = "time.google.com";

static const int NUM_BANDS = 15;

// ---- tone control design ----------------------------------------------------
// Tuned for small enclosed laptop-style speakers: "bass" sits where they can
// still reproduce it, and a fixed high-pass keeps sub-bass from wasting
// excursion (it only distorts on a 5 W box this size).
static const float SPEAKER_HPF_HZ = 90.0f;
static const float BASS_SHELF_HZ  = 180.0f;
static const float MID_PEAK_HZ    = 1200.0f;
static const float MID_Q          = 0.8f;
static const float TREBLE_SHELF_HZ = 5000.0f;
static const float LIMIT_CEILING  = 0.89f;    // -1 dBFS
static const float LIMIT_RELEASE_S = 0.15f;

// ---- display timing ---------------------------------------------------------
static const uint32_t FRAME_MS        = 40;     // 25 fps
static const uint32_t CLOCK_SHOW_MS   = 7000;
static const uint32_t NOW_MIN_MS      = 9000;   // plus until the title has scrolled once
static const uint32_t NOW_MAX_MS      = 25000;
static const uint32_t VIS_SHOW_MS     = 15000;
static const uint32_t OVERLAY_MS      = 1800;

// ---- state ------------------------------------------------------------------
Adafruit_SSD1306 display(128, 64, &Wire, -1);
Audio audio;
WebServer server(80);
Preferences prefs;

// strings are shared with the display task: write and copy them under uiMutex
SemaphoreHandle_t uiMutex;
String   station = "Night Owl FM";
String   title   = "";
String   statusMsg = "booting...";
volatile uint32_t titleSeq = 0;

uint32_t bitrate = 0;
volatile bool audioConnected = false;
volatile bool wifiUp  = false;
volatile bool paused  = false;
uint8_t  lastVol = 15;
volatile uint8_t targetVol = 15;   // survives connecttohost(), which resets library volume
uint32_t lastReconnect = 0;
uint32_t settingsChangedAt = 0;
bool     settingsDirty = false;

// SD card player. The track list (full paths, sorted, strings in PSRAM) is only
// touched from loop() and the web handlers, the same task as audio.loop().
enum Source : uint8_t { SRC_RADIO, SRC_SD, SRC_LAPTOP, SRC_COUNT };
enum Repeat : uint8_t { REP_OFF, REP_ALL, REP_ONE };
static const size_t MAX_TRACKS = 2000;
static const int HIST_N = 32;
volatile uint8_t source = SRC_RADIO;
bool     sdMounted = false;
std::vector<char*> tracks;
uint32_t tracksVer = 0;            // bumps on every rescan so the web UI refetches the list
int      curTrack = -1;
bool     shuffleOn = false;
uint8_t  repeatMode = REP_ALL;
bool     sdTrackEnded = false;     // set by evt_eof, acted on in loop()
String   sdArtist, sdTitle;        // from the file's ID3 tags, if it has them
int      history[HIST_N];          // what "previous" goes back to
int      histLen = 0;
uint8_t  bootSource = SRC_RADIO;   // restored from flash
String   bootTrack;

// Laptop source: where the companion app serves its WAV stream, and when it last
// checked in. It counts as available while it keeps pinging.
static const char*    LAPTOP_NAME = "Laptop";
static const uint32_t LAPTOP_TIMEOUT_MS = 15000;
String   laptopUrl;
String   laptopTitle;
uint32_t laptopSeen = 0;
uint32_t laptopRetryAt = 0;
bool     laptopGo = false;         // web request to switch to it; loop() does it so the reply is instant

// tone control: -12 .. +12 dB per band
volatile int eqBass = 0, eqMid = 0, eqTreble = 0;

// visualiser data, filled from the library's own analysis (after our EQ)
volatile uint8_t vuL = 0, vuR = 0;
volatile uint8_t bands[NUM_BANDS] = {0};

// oscilloscope: decimated mono copy of what the EQ outputs
static const int SCOPE_N = 256;
static int8_t scopeRing[SCOPE_N];
static volatile uint16_t scopeHead = 0;

// on-screen overlay after a volume or tone change
enum OverlayKind : uint8_t { OV_NONE, OV_VOLUME, OV_TONE, OV_SOURCE };
volatile uint8_t  overlayKind = OV_NONE;
volatile uint32_t overlayUntil = 0;

// what turning the encoder adjusts; anything but EM_VOL is the long-press menu
enum EncMode : uint8_t { EM_VOL, EM_BASS, EM_MID, EM_TREBLE, EM_SOURCE, EM_COUNT };
volatile uint8_t encMode = EM_VOL;

static void setStatus(const char* msg) {
  xSemaphoreTake(uiMutex, portMAX_DELAY);
  statusMsg = msg;
  xSemaphoreGive(uiMutex);
}

static void showOverlay(uint8_t kind) {
  overlayKind = kind;
  overlayUntil = millis() + OVERLAY_MS;
}

// ---- tone DSP -----------------------------------------------------------------
// RBJ audio-EQ-cookbook biquads, transposed direct form II, float.
enum { F_HPF, F_BASS, F_MID, F_TREBLE, F_COUNT };

static Biquad      dspCoef[F_COUNT];
static BiquadState dspState[2][F_COUNT];
static float       dspPreGain = 1.0f;
static float       dspLimGain = 1.0f;
static float       dspRelease = 0.0f;
static bool        dspToneActive = false;
static uint32_t    dspRate = 0;
static volatile bool dspDirty = true;

static Biquad normalise(float b0, float b1, float b2, float a0, float a1, float a2) {
  Biquad c;
  c.b0 = b0 / a0; c.b1 = b1 / a0; c.b2 = b2 / a0;
  c.a1 = a1 / a0; c.a2 = a2 / a0;
  return c;
}

static Biquad designHighPass(float f, float fs) {
  float w = 2.0f * M_PI * f / fs, cw = cosf(w), alpha = sinf(w) / (2.0f * 0.7071f);
  return normalise((1 + cw) / 2, -(1 + cw), (1 + cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
}

static Biquad designLowShelf(float f, float db, float fs) {
  float A = powf(10.0f, db / 40.0f), w = 2.0f * M_PI * f / fs, cw = cosf(w);
  float beta = 2.0f * sqrtf(A) * sinf(w) / 2.0f * sqrtf(2.0f);   // shelf slope S = 1
  return normalise(A * ((A + 1) - (A - 1) * cw + beta), 2 * A * ((A - 1) - (A + 1) * cw),
                   A * ((A + 1) - (A - 1) * cw - beta), (A + 1) + (A - 1) * cw + beta,
                   -2 * ((A - 1) + (A + 1) * cw), (A + 1) + (A - 1) * cw - beta);
}

static Biquad designHighShelf(float f, float db, float fs) {
  float A = powf(10.0f, db / 40.0f), w = 2.0f * M_PI * f / fs, cw = cosf(w);
  float beta = 2.0f * sqrtf(A) * sinf(w) / 2.0f * sqrtf(2.0f);
  return normalise(A * ((A + 1) + (A - 1) * cw + beta), -2 * A * ((A - 1) + (A + 1) * cw),
                   A * ((A + 1) + (A - 1) * cw - beta), (A + 1) - (A - 1) * cw + beta,
                   2 * ((A - 1) - (A + 1) * cw), (A + 1) - (A - 1) * cw - beta);
}

static Biquad designPeak(float f, float q, float db, float fs) {
  float A = powf(10.0f, db / 40.0f), w = 2.0f * M_PI * f / fs, cw = cosf(w);
  float alpha = sinf(w) / (2.0f * q);
  return normalise(1 + alpha * A, -2 * cw, 1 - alpha * A, 1 + alpha / A, -2 * cw, 1 - alpha / A);
}

static float biquadMagDb(const Biquad& c, float w) {
  float c1 = cosf(w), s1 = sinf(w), c2 = cosf(2 * w), s2 = sinf(2 * w);
  float nr = c.b0 + c.b1 * c1 + c.b2 * c2, ni = -(c.b1 * s1 + c.b2 * s2);
  float dr = 1 + c.a1 * c1 + c.a2 * c2,    di = -(c.a1 * s1 + c.a2 * s2);
  return 10.0f * log10f((nr * nr + ni * ni) / (dr * dr + di * di));
}

static inline float biquadRun(const Biquad& c, BiquadState& s, float x) {
  float y = c.b0 * x + s.z1;
  s.z1 = c.b1 * x - c.a1 * y + s.z2;
  s.z2 = c.b2 * x - c.a2 * y;
  return y;
}

// Runs in the audio path (same task as audio.loop()), so it never races
// the web handlers that change the gains.
static void dspDesign(uint32_t fs) {
  float r = (float)fs;
  dspCoef[F_HPF]    = designHighPass(SPEAKER_HPF_HZ, r);
  dspCoef[F_BASS]   = designLowShelf(BASS_SHELF_HZ, (float)eqBass, r);
  dspCoef[F_MID]    = designPeak(MID_PEAK_HZ, MID_Q, (float)eqMid, r);
  dspCoef[F_TREBLE] = designHighShelf(TREBLE_SHELF_HZ, (float)eqTreble, r);

  // Find the real peak of the combined curve. The library's tone control
  // dropped the whole signal by the largest slider boost, so turning bass up
  // made everything else quieter. Here only half the peak is taken off and the
  // limiter catches what is left, so a boost actually sounds like a boost.
  float peakDb = 0.0f;
  for (int i = 0; i < 48; i++) {
    float f = 30.0f * powf(16000.0f / 30.0f, i / 47.0f);
    if (f >= r * 0.45f) break;
    float w = 2.0f * M_PI * f / r;
    float db = biquadMagDb(dspCoef[F_BASS], w) + biquadMagDb(dspCoef[F_MID], w) +
               biquadMagDb(dspCoef[F_TREBLE], w);
    if (db > peakDb) peakDb = db;
  }
  dspPreGain = powf(10.0f, -0.5f * peakDb / 20.0f);
  dspToneActive = eqBass || eqMid || eqTreble;
  dspLimGain = 1.0f;
  dspRelease = 1.0f - expf(-1.0f / (LIMIT_RELEASE_S * r));
  dspRate = fs;
  dspDirty = false;
  Serial.printf("tone: bass %+d mid %+d treble %+d @ %u Hz, peak %+.1f dB, pre-gain %.2f\n",
                eqBass, eqMid, eqTreble, (unsigned)fs, peakDb, dspPreGain);
}

// Library hook: decoded stereo samples (int32, full scale), before VU,
// spectrum and volume. Our output therefore drives the visualisers too.
void audio_process_raw_samples(int32_t* buf, int16_t words) {
  uint32_t fs = audio.getSampleRate();
  if (fs && (fs != dspRate || dspDirty)) dspDesign(fs);
  if (!dspRate) return;

  const float toF = 1.0f / 2147483648.0f;
  const float toI = 2147483520.0f;   // largest float below INT32_MAX
  static uint8_t decim = 0;

  for (int i = 0; i + 1 < words; i += 2) {
    float l = buf[i] * toF, r = buf[i + 1] * toF;
    l = biquadRun(dspCoef[F_HPF], dspState[0][F_HPF], l);
    r = biquadRun(dspCoef[F_HPF], dspState[1][F_HPF], r);

    if (dspToneActive) {
      l *= dspPreGain; r *= dspPreGain;
      for (int f = F_BASS; f <= F_TREBLE; f++) {
        l = biquadRun(dspCoef[f], dspState[0][f], l);
        r = biquadRun(dspCoef[f], dspState[1][f], r);
      }
      // stereo-linked peak limiter: instant attack, smooth release
      float pk = fmaxf(fabsf(l), fabsf(r));
      float want = pk > LIMIT_CEILING ? LIMIT_CEILING / pk : 1.0f;
      if (want < dspLimGain) dspLimGain = want;
      else                   dspLimGain += (want - dspLimGain) * dspRelease;
      l *= dspLimGain; r *= dspLimGain;
    }

    l = fminf(fmaxf(l, -1.0f), 1.0f);
    r = fminf(fmaxf(r, -1.0f), 1.0f);
    buf[i]     = (int32_t)(l * toI);
    buf[i + 1] = (int32_t)(r * toI);

    if (++decim >= 4) {
      decim = 0;
      scopeRing[scopeHead] = (int8_t)((l + r) * 63.0f);
      scopeHead = (scopeHead + 1) % SCOPE_N;
    }
  }
}

// ---- time ---------------------------------------------------------------------
static bool timeValid() { return time(nullptr) > 1700000000; }

static bool localNow(struct tm& t) {
  if (!timeValid()) return false;
  time_t now = time(nullptr);
  localtime_r(&now, &t);
  return true;
}

// ---- text helpers ---------------------------------------------------------------
// The built-in 5x7 font is ASCII: fold common UTF-8 (accents, smart quotes).
static String asciiFold(const String& in) {
  static const char LATIN1[] = "AAAAAAACEEEEIIII" "DNOOOOOxOUUUUYTs"
                               "aaaaaaaceeeeiiii" "dnooooo/ouuuuyty";   // U+00C0..U+00FF
  String out;
  out.reserve(in.length());
  size_t n = in.length();
  for (size_t i = 0; i < n;) {
    uint8_t c = (uint8_t)in[i];
    if (c < 0x80) { out += (c < 0x20) ? ' ' : (char)c; i++; continue; }
    if ((c == 0xC2 || c == 0xC3) && i + 1 < n) {
      uint8_t cp = (c == 0xC3 ? 0xC0 : 0x80) + ((uint8_t)in[i + 1] & 0x3F);
      if (cp >= 0xC0) out += LATIN1[cp - 0xC0];
      else if (cp == 0xA0) out += ' ';
      i += 2;
      continue;
    }
    if (c == 0xE2 && i + 2 < n && (uint8_t)in[i + 1] == 0x80) {
      uint8_t e = (uint8_t)in[i + 2];
      if (e == 0x98 || e == 0x99) out += '\'';
      else if (e == 0x9C || e == 0x9D) out += '"';
      else if (e == 0x93 || e == 0x94) out += '-';
      else if (e == 0xA6) out += "...";
    }
    i += (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : 2;
  }
  out.trim();
  return out;
}

static int textWidth(const String& s, uint8_t size) { return (int)s.length() * 6 * size; }

static void printCentered(const String& s, int y, uint8_t size) {
  display.setTextSize(size);
  display.setCursor((128 - textWidth(s, size)) / 2, y);
  display.print(s);
}

// Horizontal marquee: centred when it fits, otherwise scrolls with a pause at
// the start. `looped` turns true once the whole text has been shown.
struct Marquee {
  int pos = -30;
  bool looped = false;
  void reset() { pos = -30; looped = false; }
  void draw(const String& s, int y, uint8_t size, int speed) {
    int w = textWidth(s, size);
    display.setTextSize(size);
    if (w <= 128) {
      display.setCursor((128 - w) / 2, y);
      display.print(s);
      looped = true;
      return;
    }
    const int gap = 40;
    int x = pos > 0 ? -pos : 0;
    display.setCursor(x, y);           display.print(s);
    display.setCursor(x + w + gap, y); display.print(s);
    pos += speed;
    if (pos >= w + gap) { pos = -30; looped = true; }
  }
};

// ---- display: shared drawing ------------------------------------------------------
static float specLevel[NUM_BANDS];   // smoothed, 0..1
static float specPeak[NUM_BANDS];
static uint8_t specHold[NUM_BANDS];
static float specNorm = 48.0f;

// Auto-ranged spectrum: the library's band values depend on the stream's
// loudness, so scale by a slowly decaying running maximum.
static void updateSpectrum() {
  float frameMax = 0;
  for (int i = 0; i < NUM_BANDS; i++) frameMax = fmaxf(frameMax, (float)bands[i]);
  specNorm = fmaxf(48.0f, fmaxf(specNorm * 0.995f, frameMax));
  for (int i = 0; i < NUM_BANDS; i++) {
    float v = fminf(1.0f, bands[i] / specNorm);
    if (paused) v = 0;
    specLevel[i] += (v - specLevel[i]) * (v > specLevel[i] ? 0.6f : 0.25f);
    if (specLevel[i] >= specPeak[i]) { specPeak[i] = specLevel[i]; specHold[i] = 12; }
    else if (specHold[i]) specHold[i]--;
    else specPeak[i] = fmaxf(0.0f, specPeak[i] - 0.025f);
  }
}

static void drawMiniSpectrum(int yBottom, int maxH) {
  for (int i = 0; i < NUM_BANDS; i++) {
    int h = (int)(specLevel[i] * maxH + 0.5f);
    int x = 4 + i * 8;
    if (h > 0) display.fillRect(x, yBottom - h + 1, 6, h, SSD1306_WHITE);
    else       display.drawFastHLine(x, yBottom, 6, SSD1306_WHITE);
  }
}

static String clockText(bool withSeconds = false) {
  struct tm t;
  if (!localNow(t)) return withSeconds ? "--:--:--" : "--:--";
  char buf[12];
  strftime(buf, sizeof(buf), withSeconds ? "%H:%M:%S" : "%H:%M", &t);
  return buf;
}

// ---- display: 7-segment clock (a nod to the old red LED clock radios) --------------
static const uint8_t SEG_DIGITS[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

static void drawSevenSeg(int x, int y, uint8_t segs) {
  const int w = 22, h = 40, t = 4, half = (h - 3 * t) / 2;
  if (segs & 0x01) display.fillRect(x + t, y, w - 2 * t, t, SSD1306_WHITE);                  // a
  if (segs & 0x02) display.fillRect(x + w - t, y + t, t, half, SSD1306_WHITE);               // b
  if (segs & 0x04) display.fillRect(x + w - t, y + (h + t) / 2, t, half, SSD1306_WHITE);     // c
  if (segs & 0x08) display.fillRect(x + t, y + h - t, w - 2 * t, t, SSD1306_WHITE);          // d
  if (segs & 0x10) display.fillRect(x, y + (h + t) / 2, t, half, SSD1306_WHITE);             // e
  if (segs & 0x20) display.fillRect(x, y + t, t, half, SSD1306_WHITE);                       // f
  if (segs & 0x40) display.fillRect(x + t, y + (h - t) / 2, w - 2 * t, t, SSD1306_WHITE);    // g
}

static void drawClockScreen() {
  struct tm t;
  bool ok = localNow(t);
  // shift a pixel or two every few minutes so the digits do not burn in
  int minute = ok ? t.tm_min : 0;
  int dx = (minute % 5) - 2, dy = ((minute / 5) % 3) - 1;
  int x0 = 9 + dx, y0 = 3 + dy;

  uint8_t d[4] = {0x40, 0x40, 0x40, 0x40};   // dashes until NTP answers
  if (ok) {
    d[0] = SEG_DIGITS[t.tm_hour / 10]; d[1] = SEG_DIGITS[t.tm_hour % 10];
    d[2] = SEG_DIGITS[t.tm_min / 10];  d[3] = SEG_DIGITS[t.tm_min % 10];
  }
  drawSevenSeg(x0, y0, d[0]);
  drawSevenSeg(x0 + 26, y0, d[1]);
  drawSevenSeg(x0 + 62, y0, d[2]);
  drawSevenSeg(x0 + 88, y0, d[3]);
  if (!ok || t.tm_sec % 2 == 0) {
    display.fillRect(x0 + 54, y0 + 11, 4, 4, SSD1306_WHITE);
    display.fillRect(x0 + 54, y0 + 25, 4, 4, SSD1306_WHITE);
  }

  String line;
  if (ok) {
    char buf[24];
    strftime(buf, sizeof(buf), "%a %d %b %Y", &t);
    line = buf;
  } else {
    line = "syncing time...";
  }
  if (paused) line = "PAUSED";
  printCentered(line, 52 + dy, 1);
}

// ---- display: now playing ------------------------------------------------------------
static Marquee songMarquee, artistMarquee;

static void drawNowScreen(const UiSnapshot& s) {
  display.setTextSize(1);
  String st = s.station.length() > 15 ? s.station.substring(0, 15) : s.station;
  display.setCursor(0, 0);
  display.print(st);
  String ck = clockText();
  display.setCursor(128 - textWidth(ck, 1), 0);
  display.print(ck);
  for (int x = 0; x < 128; x += 2) display.drawPixel(x, 10, SSD1306_WHITE);

  if (s.song.length() == 0) {
    artistMarquee.draw(audioConnected ? "buffering..." : "connecting...", 15, 1, 1);
    songMarquee.draw(s.station, 27, 2, 2);
  } else {
    artistMarquee.draw(s.artist.length() ? s.artist : String("Now playing"), 15, 1, 1);
    songMarquee.draw(s.song, 27, 2, 2);
  }

  if (paused) {
    display.fillRect(46, 48, 4, 12, SSD1306_WHITE);
    display.fillRect(54, 48, 4, 12, SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(64, 51);
    display.print("PAUSED");
  } else {
    drawMiniSpectrum(63, 16);
  }
}

// ---- display: visualisers -------------------------------------------------------------
enum VisStyle { VIS_BARS, VIS_MIRROR, VIS_VU, VIS_SCOPE, VIS_COUNT };

static void drawVisBars() {
  for (int i = 0; i < NUM_BANDS; i++) {
    int x = 4 + i * 8;
    int h = (int)(specLevel[i] * 62);
    for (int y = 63; y > 63 - h; y -= 3) display.fillRect(x, y - 1, 7, 2, SSD1306_WHITE);   // LED ladder
    int p = 63 - (int)(specPeak[i] * 62) - 1;
    if (p < 63) display.drawFastHLine(x, p, 7, SSD1306_WHITE);
  }
}

static void drawVisMirror() {
  for (int x = 0; x < 128; x += 4) display.drawPixel(x, 32, SSD1306_WHITE);
  for (int i = 0; i < NUM_BANDS; i++) {
    int x = 4 + i * 8;
    int h = (int)(specLevel[i] * 30);
    if (h > 0) display.fillRect(x, 32 - h, 6, 2 * h + 1, SSD1306_WHITE);
    int p = (int)(specPeak[i] * 30);
    if (p > h) {
      display.drawFastHLine(x, 32 - p, 6, SSD1306_WHITE);
      display.drawFastHLine(x, 32 + p, 6, SSD1306_WHITE);
    }
  }
}

static float needle[2] = {0, 0};

static void drawVuMeter(int cx, float v, const char* label) {
  const int pivotY = 63, r = 40;
  const float span = 50.0f * M_PI / 180.0f;
  for (int k = 0; k <= 10; k++) {
    float a = -span + 2 * span * k / 10.0f;
    float sa = sinf(a), ca = cosf(a);
    int inner = (k % 2 == 0) ? r - 6 : r - 3;
    display.drawLine(cx + sa * inner, pivotY - ca * inner, cx + sa * r, pivotY - ca * r, SSD1306_WHITE);
    if (k >= 8) display.drawLine(cx + sa * inner + 1, pivotY - ca * inner, cx + sa * r + 1, pivotY - ca * r, SSD1306_WHITE);
  }
  float a = -span + 2 * span * v;
  display.drawLine(cx, pivotY, cx + sinf(a) * (r - 2), pivotY - cosf(a) * (r - 2), SSD1306_WHITE);
  display.fillCircle(cx, pivotY, 3, SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(cx - 28, 55);
  display.print(label);
}

static void drawVisVu() {
  float target[2] = {paused ? 0 : vuL / 255.0f, paused ? 0 : vuR / 255.0f};
  for (int i = 0; i < 2; i++) {
    needle[i] += (target[i] - needle[i]) * (target[i] > needle[i] ? 0.35f : 0.12f);
  }
  drawVuMeter(32, needle[0], "L");
  drawVuMeter(96, needle[1], "R");
  printCentered(clockText(), 0, 1);
}

static float scopeGain = 1.0f;

static void drawVisScope() {
  int8_t snap[SCOPE_N];
  uint16_t head = scopeHead;
  for (int i = 0; i < SCOPE_N; i++) snap[i] = scopeRing[(head + i) % SCOPE_N];

  // trigger on a rising zero crossing so the trace stands still
  int start = 0;
  for (int i = 1; i < SCOPE_N - 128; i++) {
    if (snap[i - 1] < 0 && snap[i] >= 0) { start = i; break; }
  }
  int peak = 4;
  for (int i = 0; i < 128; i++) peak = max(peak, abs((int)snap[start + i]));
  float want = 28.0f / peak;
  scopeGain += (want - scopeGain) * 0.1f;

  for (int x = 0; x < 128; x += 4) display.drawPixel(x, 32, SSD1306_WHITE);
  int prevY = 32;
  for (int x = 0; x < 128; x++) {
    int y = 32 - (int)(snap[start + x] * scopeGain);
    y = constrain(y, 0, 63);
    if (x) display.drawLine(x - 1, prevY, x, y, SSD1306_WHITE);
    prevY = y;
  }
}

// ---- display: overlays and status --------------------------------------------------------
static void drawOverlay() {
  if (overlayKind == OV_NONE || (int32_t)(millis() - overlayUntil) > 0) return;
  display.fillRect(0, 40, 128, 24, SSD1306_BLACK);
  display.drawRoundRect(0, 40, 128, 24, 3, SSD1306_WHITE);
  display.setTextSize(1);

  if (overlayKind == OV_VOLUME) {
    uint8_t v = targetVol;
    display.setCursor(5, 48);
    display.print(v ? "VOL" : "MUTE");
    for (int i = 0; i < 21; i++) {
      int x = 30 + i * 4;
      if (i < v) display.fillRect(x, 46, 3, 12, SSD1306_WHITE);
      else       display.drawFastHLine(x, 57, 3, SSD1306_WHITE);
    }
    return;
  }

  if (overlayKind == OV_SOURCE) {
    display.setCursor(5, 44);
    display.print("SOURCE  turn to pick");
    const char* names[SRC_COUNT] = {"RADIO", "SD", "LAPTOP"};
    const int xs[SRC_COUNT] = {7, 50, 80};
    for (int k = 0; k < SRC_COUNT; k++) {
      int x = xs[k];
      bool on = k == source;
      if (on) display.fillRoundRect(x - 3, 52, textWidth(names[k], 1) + 6, 10, 2, SSD1306_WHITE);
      display.setTextColor(on ? SSD1306_BLACK : SSD1306_WHITE);
      display.setCursor(x, 53);
      display.print(names[k]);
      display.setTextColor(SSD1306_WHITE);
    }
    return;
  }

  const char* labels[3] = {"B", "M", "T"};
  int vals[3] = {eqBass, eqMid, eqTreble};
  for (int k = 0; k < 3; k++) {
    int x = 4 + k * 42;
    bool sel = encMode == EM_BASS + k;     // the band the encoder is adjusting
    if (sel) display.fillRect(x - 2, 43, 9, 10, SSD1306_WHITE);
    display.setTextColor(sel ? SSD1306_BLACK : SSD1306_WHITE);
    display.setCursor(x, 44);
    display.print(labels[k]);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(x + 12, 44);
    display.printf("%+d", vals[k]);
    int cx = x + 18;
    display.drawFastHLine(x, 57, 36, SSD1306_WHITE);
    display.drawFastVLine(cx, 54, 7, SSD1306_WHITE);
    int len = vals[k] * 17 / 12;
    if (len > 0) display.fillRect(cx, 55, len, 5, SSD1306_WHITE);
    if (len < 0) display.fillRect(cx + len, 55, -len, 5, SSD1306_WHITE);
  }
}

static void drawStatusScreen(const UiSnapshot& s, uint32_t frame) {
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(s.station);
  for (int x = 0; x < 128; x += 2) display.drawPixel(x, 10, SSD1306_WHITE);
  printCentered(s.status, 24, 1);
  for (int i = 0; i < 3; i++) {   // three dots chasing
    int on = (frame / 6) % 3;
    if (i == on) display.fillCircle(56 + i * 8, 44, 2, SSD1306_WHITE);
    else         display.drawCircle(56 + i * 8, 44, 2, SSD1306_WHITE);
  }
  printCentered(clockText(), 56, 1);
}

// ---- display health ---------------------------------------------------------------------
static bool oledPresent() {
  Wire.beginTransmission(0x3C);
  return Wire.endTransmission() == 0;
}

static void i2cRecover() {
  pinMode(SDA_PIN, INPUT_PULLUP);
  pinMode(SCL_PIN, OUTPUT_OPEN_DRAIN);
  digitalWrite(SCL_PIN, HIGH);
  for (int i = 0; i < 9; i++) {
    digitalWrite(SCL_PIN, LOW);  delayMicroseconds(5);
    digitalWrite(SCL_PIN, HIGH); delayMicroseconds(5);
  }
  pinMode(SDA_PIN, OUTPUT_OPEN_DRAIN);
  digitalWrite(SDA_PIN, LOW);   delayMicroseconds(5);
  digitalWrite(SCL_PIN, HIGH);  delayMicroseconds(5);
  digitalWrite(SDA_PIN, HIGH);  delayMicroseconds(5);
  pinMode(SDA_PIN, INPUT_PULLUP);
  pinMode(SCL_PIN, INPUT_PULLUP);
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
}

static void oledWatchdog() {
  static uint32_t lastCheck = 0;
  static uint32_t lostCount = 0;
  if (millis() - lastCheck < 2000) return;
  lastCheck = millis();

  if (oledPresent()) return;

  lostCount++;
  Serial.printf("OLED not responding (check %u) - recovering\n", (unsigned)lostCount);
  i2cRecover();
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.dim(false);
  Serial.println(oledPresent() ? "OLED back after re-init"
                               : "OLED still silent - check 3V3/GND and the two data wires");
}

// ---- display task --------------------------------------------------------------------------
// The audio library decodes inside audio.loop() on the main task, and a full
// I2C frame takes ~25 ms. Drawing here, on the other core, keeps the OLED at
// 25 fps without starving playback.
// what the OLED can show; the web UI picks which of these take part in the rotation
enum DispItem : uint8_t { D_CLOCK, D_NOW, D_BARS, D_MIRROR, D_VU, D_SCOPE, D_COUNT };
static const uint8_t D_ALL = 0x3F;
static const uint8_t D_VISMASK = 0x3C;     // the four visualisers
volatile uint8_t dispMask = D_ALL;         // bit per DispItem
volatile uint8_t dispSecs = 0;             // seconds per screen, 0 = automatic timing
volatile bool    dispSongJump = true;      // a new song jumps to now-playing
volatile bool    dispNightDim = true;      // dim 22:00 - 06:00
volatile int8_t  dispShowNow = -1;         // web "show now" request
volatile uint8_t dispCurrent = D_CLOCK;    // what is on the OLED, reported to the web UI

static inline bool dispOn(uint8_t i) { return dispMask & (1 << i); }

// Rotation is clock -> now playing -> one visualiser -> clock ..., the visualisers
// taking turns for the third slot. Disabled screens are skipped; with only
// visualisers ticked it cycles through those. Paused, the (flat) visualisers are
// skipped unless nothing else is ticked. Returns cur if nothing else qualifies.
static uint8_t nextScreen(uint8_t cur, uint8_t& lastVis) {
  uint8_t m = dispMask;
  bool visOk = (m & D_VISMASK) && (!paused || !(m & ~D_VISMASK));
  uint8_t slot = cur >= D_BARS ? 2 : cur;
  for (int k = 1; k <= 3; k++) {
    uint8_t sl = (slot + k) % 3;
    if (sl == 0 && (m & (1 << D_CLOCK))) return D_CLOCK;
    if (sl == 1 && (m & (1 << D_NOW)))   return D_NOW;
    if (sl == 2 && visOk) {
      for (int j = 1; j <= 4; j++) {
        uint8_t v = D_BARS + (lastVis - D_BARS + j) % 4;
        if (m & (1 << v)) { lastVis = v; return v; }
      }
    }
  }
  return cur;
}

static bool screenDone(uint8_t cur, uint32_t onFor) {
  uint32_t secs = dispSecs;
  if (cur == D_NOW) {
    uint32_t mn = secs ? secs * 1000 : NOW_MIN_MS;
    uint32_t mx = secs ? mn + 10000 : NOW_MAX_MS;   // let a long title finish scrolling
    return (onFor > mn && songMarquee.looped) || onFor > mx;
  }
  if (secs) return onFor > secs * 1000;
  return onFor > (cur == D_CLOCK ? CLOCK_SHOW_MS : VIS_SHOW_MS);
}

static void takeSnapshot(UiSnapshot& s) {
  static uint32_t parsedSeq = UINT32_MAX;
  static String artist, song;
  xSemaphoreTake(uiMutex, portMAX_DELAY);
  s.station = station;
  s.status = statusMsg;
  s.titleSeq = titleSeq;
  if (parsedSeq != titleSeq) {
    // ICY titles are usually "Artist - Song"
    String t = asciiFold(title);
    int sep = t.indexOf(" - ");
    if (sep > 0) { artist = t.substring(0, sep); song = t.substring(sep + 3); artist.trim(); song.trim(); }
    else         { artist = ""; song = t; }
    parsedSeq = titleSeq;
  }
  xSemaphoreGive(uiMutex);
  s.station = asciiFold(s.station);
  s.artist = artist;
  s.song = song;
}

static void uiTask(void*) {
  uint8_t screen = D_CLOCK;
  uint32_t screenSince = millis();
  uint8_t lastVis = D_SCOPE;
  uint32_t seenTitle = 0;
  uint32_t frame = 0;
  int dimmed = -1;
  UiSnapshot s;

  auto go = [&](uint8_t to, uint32_t now) {
    screen = to;
    screenSince = now;
    if (to >= D_BARS) lastVis = to;
    if (to == D_NOW) { songMarquee.reset(); artistMarquee.reset(); }
  };

  for (;;) {
    uint32_t t0 = millis();
    oledWatchdog();
    takeSnapshot(s);
    updateSpectrum();

    // a new song jumps straight to now-playing (if that screen is in use)
    if (s.titleSeq != seenTitle) {
      seenTitle = s.titleSeq;
      if (dispSongJump && dispOn(D_NOW)) go(D_NOW, t0);
    }

    int8_t want = dispShowNow;
    if (want >= 0) {                       // "show now" from the web UI
      dispShowNow = -1;
      if (want < D_COUNT) go((uint8_t)want, t0);
    } else if (!dispOn(screen) || screenDone(screen, t0 - screenSince)) {
      uint8_t n = nextScreen(screen, lastVis);
      if (n != screen || !dispOn(screen)) go(n, t0);
      else screenSince = t0;               // the only screen ticked: stay on it
    }
    dispCurrent = screen;

    // dim from 22:00 to 06:00
    struct tm tm;
    int wantDim = (dispNightDim && localNow(tm) && (tm.tm_hour >= 22 || tm.tm_hour < 6)) ? 1 : 0;
    if (wantDim != dimmed) { display.dim(wantDim); dimmed = wantDim; }

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    bool down = (source == SRC_SD) ? !audioConnected : (!wifiUp || !audioConnected);
    if (down) {
      drawStatusScreen(s, frame);
    } else {
      switch (screen) {
        case D_CLOCK:  drawClockScreen(); break;
        case D_NOW:    drawNowScreen(s); break;
        case D_BARS:   drawVisBars(); break;
        case D_MIRROR: drawVisMirror(); break;
        case D_VU:     drawVisVu(); break;
        default:       drawVisScope(); break;
      }
    }
    drawOverlay();
    display.display();
    frame++;

    int32_t wait = (int32_t)FRAME_MS - (int32_t)(millis() - t0);
    vTaskDelay(pdMS_TO_TICKS(wait > 1 ? wait : 1));
  }
}

// ---- SD card player -----------------------------------------------------------------
static void markSettingsDirty();

static bool isAudioFile(const String& name) {
  String l = name;
  l.toLowerCase();
  return l.endsWith(".mp3") || l.endsWith(".m4a") || l.endsWith(".aac") || l.endsWith(".flac") ||
         l.endsWith(".wav") || l.endsWith(".ogg") || l.endsWith(".opus");
}

static void clearTracks() {
  for (char* p : tracks) free(p);
  tracks.clear();
}

static void addTrack(const String& path) {
  size_t n = path.length() + 1;
  char* s = (char*)ps_malloc(n);   // keep internal RAM free for WiFi and TLS
  if (!s) s = (char*)malloc(n);
  if (!s) return;
  memcpy(s, path.c_str(), n);
  tracks.push_back(s);
}

static void scanDir(const String& dir, int depth) {
  File d = SD_MMC.open(dir);
  if (!d || !d.isDirectory()) return;
  while (tracks.size() < MAX_TRACKS) {
    File f = d.openNextFile();
    if (!f) break;
    String path = f.path();
    String name = f.name();
    bool isDir = f.isDirectory();
    f.close();
    yield();
    if (name.startsWith(".") || name == "System Volume Information") continue;   // macOS ._ files etc.
    if (isDir) { if (depth < 3) scanDir(path, depth + 1); }
    else if (isAudioFile(name)) addTrack(path);
  }
  d.close();
}

// Some cards time out on their first command (sdmmc_card_init 0x107), so: pull-ups on the
// bus, a short settle delay, and retries at 20, 10 and 4 MHz before giving up.
static bool sdTryMount() {
  static const int freqs[] = {SDMMC_FREQ_DEFAULT, 10000, 4000};
  Serial.printf("[sd] card-detect pin (GPIO%d) reads %d\n", SD_CD, digitalRead(SD_CD));
  for (int a = 0; a < 3; a++) {
    SD_MMC.end();
    gpio_pullup_en((gpio_num_t)SD_CMD);
    gpio_pullup_en((gpio_num_t)SD_D0);
    delay(a == 0 ? 50 : 250);
    if (!SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0)) { Serial.println("[sd] setPins failed"); return false; }
    if (SD_MMC.begin("/sdcard", true, false, freqs[a], 8)) {
      uint8_t t = SD_MMC.cardType();
      Serial.printf("[sd] mounted at %d kHz, type %s\n", freqs[a],
                    t == CARD_SDHC ? "SDHC/SDXC" : t == CARD_SD ? "SD" : t == CARD_MMC ? "MMC" : "?");
      return true;
    }
    Serial.printf("[sd] mount attempt %d at %d kHz failed\n", a + 1, freqs[a]);
  }
  return false;
}

// (re)mounts the card and rebuilds the track list
static void mountSd() {
  SD_MMC.end();
  clearTracks();
  curTrack = -1;
  histLen = 0;
  sdMounted = sdTryMount();
  if (sdMounted) {
    uint32_t t0 = millis();
    scanDir("/", 0);
    std::sort(tracks.begin(), tracks.end(),
              [](const char* a, const char* b) { return strcasecmp(a, b) < 0; });
    Serial.printf("[sd] %u MB card, %u tracks (%u ms)\n", (unsigned)(SD_MMC.cardSize() >> 20),
                  (unsigned)tracks.size(), (unsigned)(millis() - t0));
  } else {
    Serial.println("[sd] no card, or it would not mount");
  }
  tracksVer++;
}

static int findTrack(const String& path) {
  if (!path.length()) return -1;
  for (size_t i = 0; i < tracks.size(); i++) {
    if (path == tracks[i]) return (int)i;
  }
  return -1;
}

// "/Music/Green_Day - Basket Case.mp3" -> "Green Day - Basket Case"
static String trackName(int i) {
  if (i < 0 || i >= (int)tracks.size()) return String();
  String n = tracks[i];
  n = n.substring(n.lastIndexOf('/') + 1);
  int dot = n.lastIndexOf('.');
  if (dot > 0) n = n.substring(0, dot);
  n.replace('_', ' ');
  return n;
}

// "Artist - Title" from the tags, else the file name; the OLED splits on " - "
static void publishSdTitle() {
  String t;
  if (sdTitle.length()) t = sdArtist.length() ? sdArtist + " - " + sdTitle : sdTitle;
  else                  t = trackName(curTrack);
  String st = "SD card";
  if (curTrack >= 0) st += " " + String(curTrack + 1) + "/" + String((unsigned)tracks.size());
  xSemaphoreTake(uiMutex, portMAX_DELAY);
  station = st;
  if (t != title) { title = t; titleSeq = titleSeq + 1; }
  xSemaphoreGive(uiMutex);
}

static void pushHistory(int i) {
  if (i < 0) return;
  if (histLen == HIST_N) {
    memmove(history, history + 1, sizeof(int) * (HIST_N - 1));
    histLen--;
  }
  history[histLen++] = i;
}

static void sdStopped(const char* why) {
  audioConnected = false;
  paused = false;
  setStatus(why);
}

// plays track i (wrapping); a file that will not open is skipped, up to 5 in a row
static bool playTrack(int i, bool remember) {
  int n = (int)tracks.size();
  if (n == 0) return false;
  if (remember && curTrack != i) pushHistory(curTrack);
  for (int attempt = 0; attempt < 5 && attempt < n; attempt++) {
    int k = ((i + attempt) % n + n) % n;
    curTrack = k;
    source = SRC_SD;
    paused = false;
    sdTrackEnded = false;
    sdArtist = "";
    sdTitle = "";
    publishSdTitle();
    Serial.printf("[sd] play %d/%d %s\n", k + 1, n, tracks[k]);
    if (audio.connecttoFS(SD_MMC, tracks[k])) {
      audio.setVolume(targetVol);   // like connecttohost(), it resets the volume
      dspDirty = true;
      audioConnected = true;
      markSettingsDirty();
      return true;
    }
    Serial.println("[sd] could not open it, skipping");
  }
  sdStopped("SD: files won't play");
  return false;
}

static void sdNext(bool automatic) {
  int n = (int)tracks.size();
  if (n == 0) return;
  if (automatic && repeatMode == REP_ONE) { playTrack(curTrack, false); return; }
  if (shuffleOn && n > 1) {
    int k;
    do { k = (int)random(n); } while (k == curTrack);
    playTrack(k, true);
    return;
  }
  if (automatic && repeatMode == REP_OFF && curTrack >= n - 1) {
    audio.stopSong();
    sdStopped("SD: end of list");
    return;
  }
  playTrack(curTrack + 1, true);
}

// more than 3 s in: restart the track; otherwise go back to what played before
static void sdPrev() {
  if (tracks.empty()) return;
  if (audioConnected && audio.getAudioCurrentTime() > 3) { playTrack(curTrack, false); return; }
  if (histLen > 0) { playTrack(history[--histLen], false); return; }
  playTrack(curTrack - 1, false);
}

static void switchToSd() {
  if (source == SRC_SD && audioConnected) return;
  if (!tracks.empty()) { playTrack(curTrack >= 0 ? curTrack : 0, false); return; }
  audio.stopSong();
  source = SRC_SD;
  curTrack = -1;
  publishSdTitle();
  sdStopped(sdMounted ? "SD: no music found" : "SD: no card");
  markSettingsDirty();
}

static void switchToRadio() {
  if (source == SRC_RADIO) return;
  audio.stopSong();
  source = SRC_RADIO;
  audioConnected = false;          // loop() starts the stream once WiFi is up
  paused = false;
  xSemaphoreTake(uiMutex, portMAX_DELAY);
  station = RADIO_NAME;
  title = "";
  titleSeq = titleSeq + 1;
  xSemaphoreGive(uiMutex);
  setStatus(wifiUp ? "starting stream" : "WiFi: joining...");
  markSettingsDirty();
}

// ---- Laptop source (Night Owl Link app) -------------------------------------------
static bool laptopAvailable() {
  return laptopUrl.length() && millis() - laptopSeen < LAPTOP_TIMEOUT_MS;
}

static void publishLaptopTitle() {
  xSemaphoreTake(uiMutex, portMAX_DELAY);
  station = LAPTOP_NAME;
  title = laptopTitle;
  titleSeq = titleSeq + 1;
  xSemaphoreGive(uiMutex);
}

// plays the app's live MP3 stream; the library treats it like any web stream,
// so tone control, visualisers and volume all apply
static void startLaptop() {
  if (!laptopAvailable()) return;
  if (source != SRC_LAPTOP || audioConnected) audio.stopSong();
  source = SRC_LAPTOP;
  paused = false;
  publishLaptopTitle();
  setStatus("connecting to laptop");
  Serial.printf("[laptop] play %s\n", laptopUrl.c_str());
  audioConnected = audio.connecttohost(laptopUrl.c_str());
  if (audioConnected) {
    audio.setVolume(targetVol);    // connecttohost() resets the volume
    dspDirty = true;
  }
  laptopRetryAt = millis() + 3000;
}

static void switchToLaptop() {
  if (source == SRC_LAPTOP && audioConnected) return;
  startLaptop();
}

// the order the knob's Source menu steps through; Laptop only while the app is running
static void stepSource(int dir) {
  uint8_t s = source;
  for (int k = 0; k < SRC_COUNT; k++) {
    s = (s + SRC_COUNT + (dir > 0 ? 1 : -1)) % SRC_COUNT;
    if (s != SRC_LAPTOP || laptopAvailable()) break;
  }
  if (s == SRC_RADIO)       switchToRadio();
  else if (s == SRC_SD)     switchToSd();
  else                      switchToLaptop();
}

// after swapping the card or copying new music; keeps the current track if it is still there
static void rescanSd() {
  String keep = (curTrack >= 0 && curTrack < (int)tracks.size()) ? String(tracks[curTrack]) : String();
  bool wasPlaying = source == SRC_SD && audioConnected;
  if (source == SRC_SD) { audio.stopSong(); audioConnected = false; }
  mountSd();
  curTrack = findTrack(keep);
  if (source != SRC_SD) return;
  if (wasPlaying && !tracks.empty()) { playTrack(curTrack >= 0 ? curTrack : 0, false); return; }
  publishSdTitle();
  if (tracks.empty()) sdStopped(sdMounted ? "SD: no music found" : "SD: no card");
  else                sdStopped("SD: stopped");
}

// ---- audio events (library 4.x API) -----------------------------------------
static void onAudioEvent(Audio::msg_t m) {
  switch (m.e) {
    case Audio::evt_name:
      if (m.msg) {
        xSemaphoreTake(uiMutex, portMAX_DELAY);
        station = m.msg;
        xSemaphoreGive(uiMutex);
        Serial.printf("station: %s\n", m.msg);
      }
      break;
    case Audio::evt_streamtitle:
      if (m.msg) {
        xSemaphoreTake(uiMutex, portMAX_DELAY);
        title = m.msg;
        titleSeq = titleSeq + 1;
        xSemaphoreGive(uiMutex);
        Serial.printf("now playing: %s\n", m.msg);
      }
      break;
    case Audio::evt_bitrate:
      bitrate = (uint32_t)m.arg1;
      break;
    case Audio::evt_vu:
      if (m.vec1.size() >= 2) {
        vuL = (uint8_t)(m.vec1[0] > 255 ? 255 : m.vec1[0]);
        vuR = (uint8_t)(m.vec1[1] > 255 ? 255 : m.vec1[1]);
      }
      break;
    case Audio::evt_spectrum:
      if (m.vec1.size() >= (size_t)NUM_BANDS) {
        for (int i = 0; i < NUM_BANDS; i++) {
          bands[i] = (uint8_t)(m.vec1[i] > 255 ? 255 : m.vec1[i]);
        }
      }
      break;
    case Audio::evt_info:
      Serial.printf("[audio] %s\n", m.msg ? m.msg : "");
      break;
    case Audio::evt_id3data:
      if (m.msg && source == SRC_SD) {
        String t = m.msg;
        if (t.startsWith("Title: "))       sdTitle = t.substring(7);
        else if (t.startsWith("Artist: ")) sdArtist = t.substring(8);
        else break;
        sdTitle.trim();
        sdArtist.trim();
        publishSdTitle();
      }
      break;
    case Audio::evt_eof:
      if (source == SRC_SD) {        // a file finished: loop() moves on to the next one
        Serial.printf("[sd] finished %s\n", m.msg ? m.msg : "");
        sdTrackEnded = true;
        break;
      }
      Serial.println("[audio] stream ended");
      audioConnected = false;
      setStatus("stream ended");
      break;
    default:
      break;
  }
}

// ---- web UI: page -----------------------------------------------------------
static String jsonEscape(const String& in) {
  String out;
  for (int i = 0; i < (int)in.length(); i++) {
    char c = in[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c == '\n') out += "\\n";
    else if ((uint8_t)c < 0x20) out += ' ';
    else out += c;
  }
  return out;
}

static const char PAGE_HTML[] PROGMEM = R"=====(
<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="theme-color" content="#0b0f14">
<title>Night Owl FM</title>
<style>
 *{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
 body{margin:0;padding:18px;display:flex;justify-content:center;
      font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;color:#e8ecf1;
      background:#080b10;overflow-x:hidden}
 body:before{content:"";position:fixed;inset:-30%;z-index:-2;
   background:
     radial-gradient(closest-side,rgba(74,222,128,.20),transparent 70%) 15% 5%/60% 55% no-repeat,
     radial-gradient(closest-side,rgba(56,189,248,.16),transparent 70%) 85% 25%/55% 50% no-repeat,
     radial-gradient(closest-side,rgba(168,85,247,.14),transparent 70%) 50% 105%/70% 55% no-repeat;
   animation:drift 24s ease-in-out infinite alternate;filter:blur(10px)}
 @keyframes drift{0%{transform:translate3d(-2%,-1%,0) scale(1)}
                  100%{transform:translate3d(3%,2%,0) scale(1.08)}}
 .card{width:100%;max-width:440px;padding:18px;border-radius:22px;
   background:linear-gradient(180deg,rgba(23,29,38,.92),rgba(14,18,24,.96));
   border:1px solid rgba(255,255,255,.08);
   box-shadow:0 24px 60px -18px rgba(0,0,0,.85),inset 0 1px 0 rgba(255,255,255,.06);
   animation:rise .5s cubic-bezier(.2,.8,.2,1) both}
 @keyframes rise{from{opacity:0;transform:translateY(14px)}to{opacity:1;transform:none}}
 .top{display:flex;align-items:center;gap:10px}
 h1{font-size:20px;margin:0;letter-spacing:-.2px;font-weight:700;flex:1}
 .clock{font-size:13px;color:#93a0b1;font-variant-numeric:tabular-nums}
 .dot{width:9px;height:9px;border-radius:50%;background:#4ade80;flex:0 0 auto;
   animation:pulse 2s infinite}
 .dot.off{background:#f87171;animation:none}
 .dot.pause{background:#fbbf24;animation:none}
 @keyframes pulse{0%{box-shadow:0 0 0 0 rgba(74,222,128,.55)}
   70%{box-shadow:0 0 0 9px rgba(74,222,128,0)}100%{box-shadow:0 0 0 0 rgba(74,222,128,0)}}
 .sub{color:#93a0b1;font-size:12.5px;margin:3px 0 14px 19px}
 .now{overflow:hidden;border-radius:16px;padding:13px 15px;min-height:60px;
   background:linear-gradient(180deg,rgba(255,255,255,.055),rgba(255,255,255,.015));
   border:1px solid rgba(255,255,255,.07)}
 .lbl{font-size:10.5px;letter-spacing:.14em;text-transform:uppercase;color:#7d8b9c;margin-bottom:6px}
 .track{font-size:16.5px;line-height:1.3;font-weight:600;white-space:nowrap;overflow:hidden}
 .track.scroll{animation:marq 12s linear infinite alternate}
 @keyframes marq{from{transform:translateX(0)}to{transform:translateX(calc(-100% + 330px))}}
 .vis{display:flex;align-items:flex-end;gap:3px;height:64px;margin:14px 0 6px}
 .vis i{flex:1;min-width:0;border-radius:3px 3px 1px 1px;height:4%;
   background:linear-gradient(180deg,#7ef0a8,#22d3ee 55%,#0ea5e9);
   transition:height .18s cubic-bezier(.3,.9,.3,1);
   box-shadow:0 0 10px -2px rgba(56,189,248,.55)}
 .vuwrap{display:flex;gap:8px;align-items:center;margin:6px 0}
 .vuwrap span{font-size:10px;color:#788698;width:10px}
 .vu{flex:1;height:5px;border-radius:99px;background:rgba(255,255,255,.08);overflow:hidden}
 .vu b{display:block;height:100%;width:0;border-radius:99px;
   background:linear-gradient(90deg,#4ade80,#fbbf24 78%,#f87171);transition:width .16s linear}
 .lbl2{font-size:10.5px;letter-spacing:.13em;text-transform:uppercase;color:#7d8b9c;
   margin:16px 0 9px}
 .slide{display:flex;align-items:center;gap:12px;margin-bottom:10px}
 .slide .nm{font-size:12px;color:#9aa7b6;width:46px}
 .slide output{font-size:12px;color:#cbd5e1;width:52px;text-align:right;
   font-variant-numeric:tabular-nums;font-weight:600}
 input[type=range]{-webkit-appearance:none;appearance:none;flex:1;height:26px;background:transparent}
 input[type=range]::-webkit-slider-runnable-track{height:6px;border-radius:99px;
   background:linear-gradient(90deg,rgba(74,222,128,.85),rgba(56,189,248,.85))}
 input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:22px;height:22px;
   margin-top:-8px;border-radius:50%;background:#fff;border:2px solid #22d3ee;
   box-shadow:0 0 0 4px rgba(34,211,238,.18),0 2px 8px rgba(0,0,0,.6);cursor:pointer;
   transition:box-shadow .18s,transform .12s}
 input[type=range]:active::-webkit-slider-thumb{transform:scale(1.12);
   box-shadow:0 0 0 8px rgba(34,211,238,.22),0 2px 10px rgba(0,0,0,.6)}
 input[type=range]::-moz-range-track{height:6px;border-radius:99px;background:rgba(56,189,248,.55)}
 input[type=range]::-moz-range-thumb{width:20px;height:20px;border-radius:50%;background:#fff;
   border:2px solid #22d3ee;box-shadow:0 0 0 4px rgba(34,211,238,.18)}
 svg.eq{width:100%;height:54px;display:block;margin:0 0 12px}
 svg.eq path.line{fill:none;stroke:#4ade80;stroke-width:2.2;stroke-linejoin:round;
   filter:drop-shadow(0 0 6px rgba(74,222,128,.65))}
 svg.eq path.fill{fill:rgba(74,222,128,.16)}
 svg.eq line.base{stroke:rgba(255,255,255,.13);stroke-dasharray:3 4}
 .row{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:14px}
 button{background:rgba(255,255,255,.06);color:#eef3f8;border:1px solid rgba(255,255,255,.1);
   border-radius:14px;padding:15px 8px;font-size:15px;font-weight:650;cursor:pointer;
   transition:transform .12s,background .18s,border-color .18s,box-shadow .18s}
 button:hover{background:rgba(255,255,255,.1);border-color:rgba(255,255,255,.18)}
 button:active{transform:scale(.965);background:rgba(255,255,255,.15)}
 button.on{border-color:rgba(74,222,128,.55);background:rgba(74,222,128,.14);
   box-shadow:0 0 22px -6px rgba(74,222,128,.65)}
 button.ghost{background:transparent;border-style:dashed;color:#9aa7b6;font-size:13.5px;
   padding:11px}
 .meta{display:flex;justify-content:space-between;color:#7a8798;font-size:11.5px;margin-top:13px;
   font-variant-numeric:tabular-nums}
 .toast{position:fixed;left:50%;bottom:26px;transform:translate(-50%,22px);opacity:0;
   background:rgba(20,26,34,.96);border:1px solid rgba(255,255,255,.14);color:#e8ecf1;
   padding:10px 18px;border-radius:99px;font-size:13px;pointer-events:none;
   transition:opacity .22s,transform .22s;box-shadow:0 12px 30px -10px #000;z-index:9}
 .toast.show{opacity:1;transform:translate(-50%,0)}
 body:not(.sd) .sdonly,body.sd .radioonly{display:none!important}
 .seg{display:grid;grid-template-columns:1fr 1fr 1fr;gap:4px;padding:4px;margin:0 0 14px;border-radius:14px;
   background:rgba(0,0,0,.25);border:1px solid rgba(255,255,255,.07)}
 .seg button{padding:9px 6px;font-size:13.5px;border-radius:10px;border-color:transparent;background:transparent;
   color:#9aa7b6}
 .seg button.on{color:#eef3f8}
 .prog{display:flex;align-items:center;gap:10px;margin-top:12px;font-size:11.5px;color:#93a0b1;
   font-variant-numeric:tabular-nums}
 .prog span{width:38px}.prog span:last-child{text-align:right}
 .bar{flex:1;height:22px;display:flex;align-items:center;cursor:pointer;touch-action:none}
 .bar i{display:block;width:100%;height:6px;border-radius:99px;background:rgba(255,255,255,.09);overflow:hidden}
 .bar b{display:block;height:100%;width:0;border-radius:99px;background:linear-gradient(90deg,#4ade80,#22d3ee)}
 .xport{display:grid;grid-template-columns:1fr 1.35fr 1fr;gap:10px;margin-top:8px}
 .xport button{padding:13px 0;display:flex;align-items:center;justify-content:center}
 .xport svg{width:22px;height:22px;fill:currentColor}
 .xport #play svg{width:26px;height:26px}
 .opts{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:10px}
 .opts button{padding:9px 6px;font-size:12.5px;font-weight:600}
 .libhead{display:flex;align-items:center;justify-content:space-between}
 .libhead button{padding:5px 11px;font-size:11.5px;border-radius:99px;letter-spacing:0;text-transform:none}
 .search{width:100%;padding:10px 12px;border-radius:12px;border:1px solid rgba(255,255,255,.1);
   background:rgba(0,0,0,.25);color:#e8ecf1;font-size:14px;margin-bottom:8px;outline:none}
 .search:focus{border-color:rgba(34,211,238,.5)}
 .list{position:relative;max-height:340px;overflow-y:auto;border-radius:12px;
   border:1px solid rgba(255,255,255,.07);background:rgba(0,0,0,.18)}
 .list div{padding:9px 12px;border-bottom:1px solid rgba(255,255,255,.045);cursor:pointer;font-size:13.5px;
   white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
 .list div:last-child{border-bottom:0}
 .list div:hover{background:rgba(255,255,255,.05)}
 .list div.cur{background:rgba(74,222,128,.13);color:#86efac;font-weight:600}
 .list div.cur:before{content:"\25B6\FE0E  ";font-size:10px}
 .list small{display:block;color:#6b7889;font-size:11px;font-weight:400;overflow:hidden;text-overflow:ellipsis}
 .list .empty{color:#7d8b9c;text-align:center;padding:18px;cursor:default;white-space:normal}
 .scr{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-bottom:12px}
 .tile{position:relative;border-radius:13px;padding:13px 6px 9px;cursor:pointer;text-align:center;
   font-size:12px;font-weight:600;color:#6f7c8d;background:rgba(255,255,255,.035);
   border:1px solid rgba(255,255,255,.08);user-select:none;
   transition:background .18s,border-color .18s,color .18s,box-shadow .18s}
 .tile:hover{border-color:rgba(255,255,255,.18)}
 .tile.on{color:#e8ecf1;border-color:rgba(74,222,128,.5);background:rgba(74,222,128,.09)}
 .tile svg{width:46px;height:26px;display:block;margin:0 auto 6px;stroke:currentColor;fill:none;
   stroke-width:1.7;stroke-linecap:round;stroke-linejoin:round;opacity:.55;transition:opacity .18s}
 .tile.on svg{opacity:1;stroke:#7ef0a8}
 .tile .ck{position:absolute;top:6px;left:7px;width:14px;height:14px;border-radius:4px;
   border:1.5px solid rgba(255,255,255,.25)}
 .tile.on .ck{background:#4ade80;border-color:#4ade80}
 .tile.on .ck:after{content:"";position:absolute;left:3.5px;top:0.5px;width:4px;height:8px;
   border:solid #08210f;border-width:0 2px 2px 0;transform:rotate(45deg)}
 .tile .go{position:absolute;top:4px;right:4px;width:24px;height:22px;padding:0;border-radius:7px;
   font-size:9px;line-height:20px;color:#9aa7b6}
 .tile.live{box-shadow:0 0 0 1px rgba(34,211,238,.75),0 0 18px -4px rgba(34,211,238,.7)}
 .tile .lv{display:none;font-size:9.5px;letter-spacing:.12em;color:#22d3ee;margin-top:3px}
 .tile.live .lv{display:block}
 .hint{font-size:11.5px;color:#6b7889;margin:8px 2px 0;line-height:1.45}
 @media (prefers-reduced-motion:reduce){*{animation:none!important}}
</style></head><body><div class="card">
 <div class="top"><span class="dot" id="dot"></span><h1 id="station">Night Owl FM</h1>
   <span class="clock" id="clock">--:--</span></div>
 <div class="sub" id="state">connecting&hellip;</div>
 <div class="seg"><button id="srcRadio" class="on">Radio</button><button id="srcSd">SD card</button><button id="srcLap" title="Start Night Owl Link on your laptop">Laptop</button></div>

 <div class="now"><div class="lbl">Now playing</div>
   <div class="track" id="title">&mdash;</div></div>

 <div class="sdonly">
   <div class="prog"><span id="pos">0:00</span>
     <div class="bar" id="bar"><i><b id="barf"></b></i></div><span id="dur">0:00</span></div>
   <div class="xport">
     <button id="prev" title="Previous"><svg viewBox="0 0 24 24"><path d="M6 5h2v14H6zM20 5v14L9.5 12z"/></svg></button>
     <button id="play" title="Play / pause"><svg viewBox="0 0 24 24"><path id="playico" d="M7 4v16l13-8z"/></svg></button>
     <button id="next" title="Next"><svg viewBox="0 0 24 24"><path d="M16 5h2v14h-2zM4 5v14l10.5-7z"/></svg></button>
   </div>
   <div class="opts"><button id="shuf">Shuffle</button><button id="rep">Repeat all</button></div>
 </div>

 <div class="vis" id="vis"></div>
 <div class="vuwrap"><span>L</span><div class="vu"><b id="vul"></b></div></div>
 <div class="vuwrap"><span>R</span><div class="vu"><b id="vur"></b></div></div>

 <div class="lbl2">Volume</div>
 <div class="slide">
   <button id="dn" style="width:46px;padding:8px 0">&minus;</button>
   <input id="vol" type="range" min="0" max="21" step="1" value="15">
   <button id="up" style="width:46px;padding:8px 0">+</button>
   <output id="volnum">15</output>
 </div>

 <div class="lbl2">Tone &middot; 180 Hz / 1.2 kHz / 5 kHz</div>
 <svg class="eq" viewBox="0 0 300 54" preserveAspectRatio="none">
   <line class="base" x1="0" y1="27" x2="300" y2="27"/>
   <path class="fill" id="eqfill" d=""></path>
   <path class="line" id="eqline" d=""></path>
 </svg>
 <div class="slide"><span class="nm">Bass</span>
   <input id="bass" type="range" min="-12" max="12" step="1" value="0">
   <output id="bassv">0 dB</output></div>
 <div class="slide"><span class="nm">Mid</span>
   <input id="mid" type="range" min="-12" max="12" step="1" value="0">
   <output id="midv">0 dB</output></div>
 <div class="slide"><span class="nm">Treble</span>
   <input id="treble" type="range" min="-12" max="12" step="1" value="0">
   <output id="treblev">0 dB</output></div>

 <div class="row">
   <button id="pause" class="radioonly">Pause</button>
   <button id="mute">Mute</button>
   <button id="flat" class="ghost">Flat EQ</button>
   <button id="reconnect" class="ghost radioonly">Reconnect</button>
 </div>

 <div class="lbl2">Display</div>
 <div class="scr" id="scr"></div>
 <div class="slide"><span class="nm">Each</span>
   <input id="dsec" type="range" min="0" max="11" step="1" value="0">
   <output id="dsecv">auto</output></div>
 <div class="opts"><button id="djump">Jump to new song</button><button id="ddim">Dim at night</button></div>
 <div class="hint" id="dhint">Tick the screens to rotate through. Tick just one to keep it on.
   &#9654; shows a screen straight away.</div>

 <div class="sdonly">
   <div class="lbl2 libhead"><span>Library &middot; <span id="ntr">0</span> tracks</span>
     <button id="rescan" class="ghost">Rescan card</button></div>
   <input class="search" id="q" type="search" placeholder="Search songs, artists, folders" autocomplete="off">
   <div class="list" id="list"><div class="empty">loading&hellip;</div></div>
 </div>

 <div class="meta"><span><b id="br">--</b> kbps</span><span id="ip">--</span></div>
</div>
<div class="toast" id="toast"></div>
<script>
function $(i){return document.getElementById(i);}
var dragging=false, editingNow=false, BANDS=15, bars=[], toneT;
var tracks=[], tver=-1, curIdx=-2, sdOk=false, isSd=false, lastDur=0, seekUntil=0;
var PLAY='M7 4v16l13-8z', PAUSE='M6 4h4v16H6zM14 4h4v16h-4z';

(function(){ var v=$('vis'); for(var i=0;i<BANDS;i++){ var b=document.createElement('i');
  v.appendChild(b); bars.push(b); } })();

var toastT;
function toast(m){ var t=$('toast'); t.textContent=m; t.classList.add('show');
  clearTimeout(toastT); toastT=setTimeout(function(){t.classList.remove('show');},1400); }

function fmtDb(v){ return (v>0?'+':'')+v+' dB'; }

// EQ response: low shelf 180 Hz + peaking 1.2 kHz + high shelf 5 kHz
function drawEq(b,m,t){
  var W=300,H=54,mid=H/2-3,pts=[];
  for(var i=0;i<=48;i++){
    var x=i/48, hz=20*Math.pow(1000,x);
    var ls=1/(1+Math.pow(hz/180,2));
    var hs=1/(1+Math.pow(5000/Math.max(hz,1),2));
    var pk=Math.exp(-Math.pow(Math.log(hz/1200)/Math.log(2)/1.2,2));
    var db=b*ls+m*pk+t*hs;
    pts.push((x*W).toFixed(1)+' '+(mid-db*(mid/12)).toFixed(1));
  }
  var d='M'+pts.join(' L');
  $('eqline').setAttribute('d',d);
  $('eqfill').setAttribute('d',d+' L300 54 L0 54 Z');
}

function fmtT(t){ t=Math.max(0,t|0); return Math.floor(t/60)+':'+('0'+t%60).slice(-2); }
function esc(t){ return t.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;'); }
// "/Rock/Green_Day - Basket Case.mp3" -> ["Green Day - Basket Case", "Rock"]
function splitPath(p){ var i=p.lastIndexOf('/'), f=p.slice(i+1), d=i>0?p.slice(1,i):'', dot=f.lastIndexOf('.');
  if(dot>0) f=f.slice(0,dot); return [f.replace(/_/g,' '), d]; }

function loadTracks(){
  fetch('/api/tracks').then(function(r){return r.json();}).then(function(d){
    tracks=d.tracks||[]; drawList(); scrollToCur(); }).catch(function(){ tver=-1; });
}
function drawList(){
  var q=$('q').value.trim().toLowerCase(), h=[];
  for(var i=0;i<tracks.length;i++){
    if(q && tracks[i].toLowerCase().indexOf(q)<0) continue;
    var p=splitPath(tracks[i]);
    h.push('<div data-i="'+i+'"'+(i===curIdx?' class="cur"':'')+'>'+esc(p[0])+
           (p[1]?'<small>'+esc(p[1])+'</small>':'')+'</div>');
  }
  $('list').innerHTML=h.length?h.join(''):'<div class="empty">'+(tracks.length?'No matches':
    (sdOk?'No music on the card. Copy MP3 files to it, then press Rescan card.':
          'No SD card found. Insert a FAT32 card, then press Rescan card.'))+'</div>';
}
function scrollToCur(){
  var L=$('list'), c=L.querySelector('.cur');
  if(c && (c.offsetTop<L.scrollTop || c.offsetTop+c.offsetHeight>L.scrollTop+L.clientHeight))
    L.scrollTop=c.offsetTop-L.clientHeight/2+c.offsetHeight/2;
}
function setProgress(pos,dur){
  $('pos').textContent=fmtT(pos);
  $('dur').textContent=dur?fmtT(dur):'--:--';
  $('barf').style.width=(dur?Math.min(100,100*pos/dur):0)+'%';
}

// ---- OLED display settings ----
var SCR=[
  ['Clock','<path d="M6 6h7v14H6zM17 6h7v14h-7zM31 6h7v14h-7z"/><circle cx="27.5" cy="10" r=".6"/><circle cx="27.5" cy="16" r=".6"/>'],
  ['Now playing','<path d="M4 6h22M4 13h38M4 20h30"/>'],
  ['Spectrum','<path d="M6 24v-8M12 24v-14M18 24v-10M24 24v-18M30 24v-12M36 24v-6M42 24v-9"/>'],
  ['Mirror','<path d="M6 17v-8M12 21v-16M18 19v-12M24 23v-20M30 18v-10M36 16v-6M42 20v-14"/>'],
  ['VU meters','<path d="M3 21a10 10 0 0 1 18 0M25 21a10 10 0 0 1 18 0M12 21l-4-8M34 21l3-8"/>'],
  ['Scope','<path d="M2 13c4-12 6-12 10 0s6 12 10 0 6-12 10 0 6 12 10 0"/>']];
var DSECS=[0,3,5,8,10,15,20,30,45,60,90,120], dmask=63, dEdit=false, dj=true, dd=true;
(function(){ var h=[];
  for(var i=0;i<SCR.length;i++) h.push('<div class="tile" data-t="'+i+'"><span class="ck"></span>'+
    '<button class="go ghost" data-go="'+i+'" title="Show now">&#9654;</button>'+
    '<svg viewBox="0 0 46 26">'+SCR[i][1]+'</svg>'+SCR[i][0]+'<div class="lv">ON SCREEN</div></div>');
  $('scr').innerHTML=h.join(''); })();
function bits(m){ var n=0; while(m){ n+=m&1; m>>=1; } return n; }
function secLabel(v){ return bits(dmask)===1?'fixed':(v?v+' s':'auto'); }
function markTiles(cur){
  var t=$('scr').children;
  for(var i=0;i<t.length;i++){ t[i].classList.toggle('on',!!(dmask&(1<<i)));
    if(cur!==undefined) t[i].classList.toggle('live',i===cur); }
}
$('scr').onclick=function(e){
  var g=e.target.closest('[data-go]');
  if(g){ var k=+g.getAttribute('data-go'); post('/api/show?i='+k,SCR[k][0]+' on screen'); return; }
  var t=e.target.closest('[data-t]'); if(!t) return;
  var m=dmask^(1<<+t.getAttribute('data-t'));
  if(!m){ toast('keep at least one screen'); return; }
  dmask=m; markTiles(); $('dsecv').textContent=secLabel(DSECS[+$('dsec').value]);
  post('/api/display?m='+m);
};
$('dsec').oninput=function(){ dEdit=true; $('dsecv').textContent=secLabel(DSECS[+this.value]); };
$('dsec').onchange=function(){ var v=DSECS[+this.value];
  post('/api/display?s='+v, v?'each screen '+v+' s':'automatic timing').then(function(){ dEdit=false; }); };
$('djump').onclick=function(){ post('/api/display?j='+(dj?0:1)); };
$('ddim').onclick=function(){ post('/api/display?d='+(dd?0:1)); };

function render(s){
  if(s.dm!==undefined){
    dmask=s.dm; dj=!!s.dj; dd=!!s.dd; markTiles(s.dc);
    if(!dEdit){ var k=DSECS.indexOf(s.ds); $('dsec').value=k<0?0:k; $('dsecv').textContent=secLabel(s.ds); }
    $('djump').classList.toggle('on',dj); $('ddim').classList.toggle('on',dd);
  }
  isSd=s.src==='sd'; sdOk=!!s.sd;
  document.body.classList.toggle('sd',isSd);
  var isLap=s.src==='laptop'; lapOk=!!s.lap;
  $('srcRadio').classList.toggle('on',!isSd&&!isLap);
  $('srcSd').classList.toggle('on',isSd);
  $('srcLap').classList.toggle('on',isLap);
  $('srcLap').style.opacity=lapOk?'':'.45';
  if(s.tver!==tver){ tver=s.tver; loadTracks(); }
  $('ntr').textContent=s.ntr;
  if(s.trk!==curIdx){ curIdx=s.trk; drawList(); scrollToCur(); }
  lastDur=s.dur||0;
  if(Date.now()>seekUntil) setProgress(s.pos||0,lastDur);
  var going=s.active&&!s.paused;
  $('playico').setAttribute('d',going?PAUSE:PLAY);
  $('play').classList.toggle('on',going);
  $('shuf').classList.toggle('on',!!s.shuf);
  $('rep').textContent=['Repeat off','Repeat all','Repeat one'][s.rep]||'Repeat';
  $('rep').classList.toggle('on',s.rep>0);

  $('station').textContent=s.station;
  $('clock').textContent=s.time||'--:--';
  var t=$('title'), txt=s.title||'—';
  if(t.textContent!==txt) t.textContent=txt;
  t.classList.toggle('scroll', t.scrollWidth>t.clientWidth+4);
  if(isSd){
    $('state').textContent = s.paused?'paused':(s.active?'playing from SD card':
      (!s.sd?'no SD card':(s.ntr?'stopped':'no music on card')));
    $('dot').className='dot'+(s.paused?' pause':(s.active?'':' off'));
  } else {
    $('state').textContent = s.paused?'paused':(s.playing?'streaming':'buffering');
    $('dot').className='dot'+(s.paused?' pause':(s.playing?'':' off'));
  }
  $('br').textContent=s.kbps?s.kbps:'--';
  $('ip').textContent=s.ip;
  if(!dragging) $('vol').value=s.vol;
  $('volnum').textContent=s.vol;
  $('pause').textContent=s.paused?'Resume':'Pause';
  $('pause').classList.toggle('on',!!s.paused);
  $('mute').textContent=(s.vol===0)?'Unmute':'Mute';
  $('mute').classList.toggle('on',s.vol===0);

  if(!editingNow){
    $('bass').value=s.bass; $('mid').value=s.mid; $('treble').value=s.treble;
    $('bassv').textContent=fmtDb(s.bass);
    $('midv').textContent=fmtDb(s.mid);
    $('treblev').textContent=fmtDb(s.treble);
    drawEq(s.bass,s.mid,s.treble);
  }

  var v=s.vu||[0,0];
  $('vul').style.width=Math.round(100*v[0]/255)+'%';
  $('vur').style.width=Math.round(100*v[1]/255)+'%';

  var bd=s.bands||[], maxv=48;
  for(var i=0;i<bd.length;i++){ if(bd[i]>maxv) maxv=bd[i]; }
  for(var k=0;k<bars.length;k++){
    var h=bd[k]?Math.round(100*bd[k]/maxv):2;
    bars[k].style.height=Math.max(4,Math.min(100,h))+'%';
  }
}

function post(u,msg){ return fetch(u).then(function(r){return r.json();}).then(function(s){
  render(s); if(msg) toast(msg); }).catch(function(){ toast('not reachable'); }); }
function poll(){ fetch('/api/status').then(function(r){return r.json();})
  .then(render).catch(function(){}); }

$('up').onclick=function(){ post('/api/volup'); };
$('dn').onclick=function(){ post('/api/voldn'); };
$('pause').onclick=function(){ post('/api/toggle'); };
$('mute').onclick=function(){ post('/api/mute'); };
$('reconnect').onclick=function(){ post('/api/reconnect','reconnecting'); };
$('flat').onclick=function(){ post('/api/tone?b=0&m=0&t=0','tone reset'); };

var lapOk=false;
$('srcRadio').onclick=function(){ post('/api/source?s=radio','radio'); };
$('srcSd').onclick=function(){ if(!isSd) post('/api/source?s=sd','SD card'); };
$('srcLap').onclick=function(){
  if(!lapOk){ toast('start Night Owl Link on the laptop first'); return; }
  post('/api/source?s=laptop','laptop'); };
$('play').onclick=function(){ post('/api/toggle'); };
$('prev').onclick=function(){ post('/api/prev'); };
$('next').onclick=function(){ post('/api/next'); };
$('shuf').onclick=function(){ post('/api/shuffle'); };
$('rep').onclick=function(){ post('/api/repeat'); };
$('rescan').onclick=function(){ toast('reading card\u2026'); post('/api/rescan','library updated'); };
$('q').oninput=function(){ drawList(); };
$('list').onclick=function(e){ var d=e.target.closest('[data-i]');
  if(d) post('/api/play?i='+d.getAttribute('data-i')); };
$('bar').onclick=function(e){ if(!lastDur) return;
  var r=this.getBoundingClientRect(), f=Math.min(1,Math.max(0,(e.clientX-r.left)/r.width)), t=Math.round(f*lastDur);
  setProgress(t,lastDur); seekUntil=Date.now()+1500; post('/api/seek?t='+t); };
$('vol').oninput=function(e){ dragging=true; $('volnum').textContent=e.target.value; };
$('vol').onchange=function(e){ dragging=false; post('/api/vol?v='+e.target.value); };

// tone is applied live while dragging (throttled), so you hear what you set
function sendTone(msg){
  return post('/api/tone?b='+$('bass').value+'&m='+$('mid').value+'&t='+$('treble').value,msg);
}
function bindEq(id,out,name){
  var el=$(id);
  el.oninput=function(){ editingNow=true; $(out).textContent=fmtDb(+el.value);
    drawEq(+$('bass').value,+$('mid').value,+$('treble').value);
    clearTimeout(toneT); toneT=setTimeout(function(){ sendTone(); },120); };
  el.onchange=function(){ clearTimeout(toneT);
    sendTone(name+' '+fmtDb(+el.value)).then(function(){ editingNow=false; }); };
}
bindEq('bass','bassv','bass');
bindEq('mid','midv','mid');
bindEq('treble','treblev','treble');

document.addEventListener('keydown',function(e){
  if(e.target.tagName==='INPUT') return;
  if(e.code==='Space'){ e.preventDefault(); post('/api/toggle'); }
  else if(isSd && e.key==='ArrowRight'){ post('/api/next'); }
  else if(isSd && e.key==='ArrowLeft'){ post('/api/prev'); }
  else if(e.key==='ArrowUp'){ post('/api/volup'); }
  else if(e.key==='ArrowDown'){ post('/api/voldn'); }
});

drawEq(0,0,0);
setInterval(poll,400); poll();
</script></body></html>
)=====";

// ---- web UI: json + handlers ------------------------------------------------
static String statusJson() {
  xSemaphoreTake(uiMutex, portMAX_DELAY);
  String st = station, ti = title;
  xSemaphoreGive(uiMutex);

  String j = "{";
  j += "\"station\":\"" + jsonEscape(st) + "\",";
  j += "\"title\":\""   + jsonEscape(ti) + "\",";
  j += "\"time\":\""    + clockText() + "\",";
  j += "\"vol\":"      + String(audio.getVolume()) + ",";
  j += "\"kbps\":"     + String((bitrate ? bitrate : audio.getBitRate()) / 1000) + ",";
  j += "\"playing\":"  + String(audio.isRunning() ? "true" : "false") + ",";
  j += "\"paused\":"   + String(paused ? "true" : "false") + ",";
  j += "\"bass\":"     + String(eqBass) + ",";
  j += "\"mid\":"      + String(eqMid) + ",";
  j += "\"treble\":"   + String(eqTreble) + ",";
  j += "\"ip\":\""     + WiFi.localIP().toString() + "\",";
  bool sdOn = source == SRC_SD && audioConnected;
  j += "\"src\":\""    + String(source == SRC_SD ? "sd" : source == SRC_LAPTOP ? "laptop" : "radio") + "\",";
  j += "\"lap\":"      + String(laptopAvailable() ? "true" : "false") + ",";
  j += "\"ibuf\":"     + String(source == SRC_LAPTOP && audioConnected ? audio.inBufferFilled() : 0) + ",";
  j += "\"active\":"   + String(audioConnected ? "true" : "false") + ",";
  j += "\"sd\":"       + String(sdMounted ? "true" : "false") + ",";
  j += "\"ntr\":"      + String((unsigned)tracks.size()) + ",";
  j += "\"trk\":"      + String(curTrack) + ",";
  j += "\"tver\":"     + String(tracksVer) + ",";
  j += "\"pos\":"      + String(sdOn ? audio.getAudioCurrentTime() : 0) + ",";
  j += "\"dur\":"      + String(sdOn ? audio.getAudioFileDuration() : 0) + ",";
  j += "\"shuf\":"     + String(shuffleOn ? "true" : "false") + ",";
  j += "\"rep\":"      + String(repeatMode) + ",";
  j += "\"dm\":"       + String(dispMask) + ",";
  j += "\"ds\":"       + String(dispSecs) + ",";
  j += "\"dj\":"       + String(dispSongJump ? "true" : "false") + ",";
  j += "\"dd\":"       + String(dispNightDim ? "true" : "false") + ",";
  j += "\"dc\":"       + String(dispCurrent) + ",";
  j += "\"vu\":["      + String((int)vuL) + "," + String((int)vuR) + "],";
  j += "\"bands\":[";
  for (int i = 0; i < NUM_BANDS; i++) {
    if (i) j += ",";
    j += String((int)bands[i]);
  }
  j += "]}";
  return j;
}

static void sendJson() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", statusJson());
}

// the whole library, streamed in chunks so a big card needs no big buffer
static void sendTrackList() {
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  String chunk = "{\"ver\":" + String(tracksVer) + ",\"sd\":" + String(sdMounted ? "true" : "false") +
                 ",\"tracks\":[";
  for (size_t i = 0; i < tracks.size(); i++) {
    if (i) chunk += ',';
    chunk += '"';
    chunk += jsonEscape(String(tracks[i]));
    chunk += '"';
    if (chunk.length() > 1400) { server.sendContent(chunk); chunk = ""; }
  }
  chunk += "]}";
  server.sendContent(chunk);
  server.sendContent("");          // ends the chunked reply
}

static void markSettingsDirty() {
  settingsDirty = true;
  settingsChangedAt = millis();
}

static void setVol(int v) {
  if (v < 0) v = 0;
  if (v > 21) v = 21;
  audio.setVolume((uint8_t)v);
  if (v > 0) { lastVol = (uint8_t)v; }
  targetVol = (uint8_t)v;
  showOverlay(OV_VOLUME);
  markSettingsDirty();
  Serial.printf("volume -> %d\n", v);
}

static int clampDb(int v) {
  if (v < -12) return -12;
  if (v >  12) return  12;
  return v;
}

static void setTone(int b, int m, int t) {
  b = clampDb(b); m = clampDb(m); t = clampDb(t);
  if (b == eqBass && m == eqMid && t == eqTreble) return;
  eqBass = b; eqMid = m; eqTreble = t;
  dspDirty = true;                 // the audio path redesigns its filters
  showOverlay(OV_TONE);
  markSettingsDirty();
}

static void startStream() {
  source = SRC_RADIO;
  audio.connecttohost(STREAM_URL);
  // connecttohost() re-initialises I2S and drops volume to the default
  audio.setVolume(targetVol);
  dspDirty = true;
  audioConnected = true;
  paused = false;
  xSemaphoreTake(uiMutex, portMAX_DELAY);
  station = RADIO_NAME;            // the stream's own name replaces it when it arrives
  title = "";
  titleSeq = titleSeq + 1;
  xSemaphoreGive(uiMutex);
}

// play/pause for the web button and the encoder; on a stopped SD card it starts the list
static void togglePlay(const char* from) {
  if (source == SRC_SD && !audioConnected) {
    if (!tracks.empty()) playTrack(curTrack >= 0 ? curTrack : 0, false);
    return;
  }
  audio.pauseResume();
  paused = !paused;
  Serial.printf("[%s] %s\n", from, paused ? "paused" : "resumed");
}

static void setupWeb() {
  server.on("/", []() {
    server.send_P(200, "text/html; charset=utf-8", PAGE_HTML);
  });
  server.on("/api/status", []() { sendJson(); });
  server.on("/api/volup",   []() { setVol(audio.getVolume() + 1); sendJson(); });
  server.on("/api/voldn",   []() { setVol(audio.getVolume() - 1); sendJson(); });
  server.on("/api/vol",     []() {
    if (server.hasArg("v")) setVol(server.arg("v").toInt());
    sendJson();
  });
  server.on("/api/mute",    []() {
    if (audio.getVolume() > 0) { setVol(0); }
    else                       { setVol(lastVol ? lastVol : 15); }
    sendJson();
  });
  server.on("/api/toggle",  []() { togglePlay("web"); sendJson(); });
  server.on("/api/tone",    []() {
    setTone(server.hasArg("b") ? server.arg("b").toInt() : eqBass,
            server.hasArg("m") ? server.arg("m").toInt() : eqMid,
            server.hasArg("t") ? server.arg("t").toInt() : eqTreble);
    sendJson();
  });
  server.on("/api/reconnect", []() {
    Serial.println("[web] reconnect");
    startStream();
    sendJson();
  });
  // ---- SD card player ----
  server.on("/api/tracks",  []() { sendTrackList(); });
  server.on("/api/source",  []() {
    String s = server.arg("s");
    if (s == "sd")          switchToSd();
    else if (s == "laptop") laptopGo = true;
    else                    switchToRadio();
    sendJson();
  });
  // ---- Laptop source: called by the Night Owl Link app ----
  server.on("/api/laptop",  []() {
    if (server.hasArg("url")) laptopUrl = server.arg("url");
    laptopSeen = millis();
    if (server.hasArg("title")) laptopTitle = asciiFold(server.arg("title"));
    // connecting to the laptop can take a few seconds (closing the old stream first),
    // so it happens in loop() and this reply goes back at once
    if (server.arg("go") == "1") laptopGo = true;
    else if (source == SRC_LAPTOP) publishLaptopTitle();
    sendJson();
  });
  server.on("/api/laptop/ping", []() {
    laptopSeen = millis();
    if (server.hasArg("title")) {
      String t = asciiFold(server.arg("title"));
      if (t != laptopTitle) {
        laptopTitle = t;
        if (source == SRC_LAPTOP) publishLaptopTitle();
      }
    }
    sendJson();
  });
  server.on("/api/laptop/stop", []() {
    Serial.println("[laptop] app stopped streaming");
    laptopUrl = "";
    if (source == SRC_LAPTOP) switchToRadio();
    sendJson();
  });
  server.on("/api/play",    []() {
    int i = server.hasArg("i") ? server.arg("i").toInt() : -1;
    if (i >= 0 && i < (int)tracks.size()) playTrack(i, true);
    sendJson();
  });
  server.on("/api/next",    []() { if (source == SRC_SD) sdNext(false); sendJson(); });
  server.on("/api/prev",    []() { if (source == SRC_SD) sdPrev(); sendJson(); });
  server.on("/api/seek",    []() {
    if (source == SRC_SD && audioConnected && server.hasArg("t")) {
      int t = server.arg("t").toInt();
      if (t >= 0) audio.setAudioPlayTime((uint16_t)t);
    }
    sendJson();
  });
  server.on("/api/shuffle", []() { shuffleOn = !shuffleOn; markSettingsDirty(); sendJson(); });
  server.on("/api/repeat",  []() { repeatMode = (repeatMode + 1) % 3; markSettingsDirty(); sendJson(); });
  server.on("/api/rescan",  []() { rescanSd(); sendJson(); });
  // ---- OLED display ----
  server.on("/api/display", []() {
    if (server.hasArg("m")) {
      uint8_t m = (uint8_t)server.arg("m").toInt() & D_ALL;
      dispMask = m ? m : (1 << D_CLOCK);   // never leave the screen with nothing to show
    }
    if (server.hasArg("s")) dispSecs = (uint8_t)constrain(server.arg("s").toInt(), 0, 120);
    if (server.hasArg("j")) dispSongJump = server.arg("j").toInt() != 0;
    if (server.hasArg("d")) dispNightDim = server.arg("d").toInt() != 0;
    markSettingsDirty();
    sendJson();
  });
  server.on("/api/show",    []() {
    int i = server.hasArg("i") ? server.arg("i").toInt() : -1;
    if (i >= 0 && i < D_COUNT) dispShowNow = (int8_t)i;
    sendJson();
  });
  server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
  server.begin();
  Serial.printf("web UI: http://%s/\n", WiFi.localIP().toString().c_str());
}

// ---- settings: volume and tone survive a reboot ----------------------------------
static void loadSettings() {
  prefs.begin("radio", true);
  targetVol = prefs.getUChar("vol", 15);
  eqBass    = clampDb(prefs.getChar("bass", 0));
  eqMid     = clampDb(prefs.getChar("mid", 0));
  eqTreble  = clampDb(prefs.getChar("treble", 0));
  bootSource = prefs.getUChar("src", SRC_RADIO);
  if (bootSource >= SRC_LAPTOP) bootSource = SRC_RADIO;   // the laptop app may not be running
  bootTrack  = prefs.getString("trk", "");
  shuffleOn  = prefs.getBool("shuf", false);
  repeatMode = prefs.getUChar("rep", REP_ALL);
  dispMask     = prefs.getUChar("dmask", D_ALL) & D_ALL;
  dispSecs     = prefs.getUChar("dsecs", 0);
  dispSongJump = prefs.getBool("djump", true);
  dispNightDim = prefs.getBool("ddim", true);
  if (!dispMask) dispMask = D_ALL;
  if (dispSecs > 120) dispSecs = 0;
  prefs.end();
  if (repeatMode > REP_ONE) repeatMode = REP_ALL;
  if (targetVol > 21) targetVol = 15;
  lastVol = targetVol ? targetVol : 15;
}

// writes 3 s after the last change, so dragging a slider does not wear the flash
static void saveSettingsIfDue() {
  if (!settingsDirty || millis() - settingsChangedAt < 3000) return;
  settingsDirty = false;
  prefs.begin("radio", false);
  prefs.putUChar("vol", targetVol);
  prefs.putChar("bass", (int8_t)eqBass);
  prefs.putChar("mid", (int8_t)eqMid);
  prefs.putChar("treble", (int8_t)eqTreble);
  prefs.putUChar("src", source == SRC_LAPTOP ? SRC_RADIO : source);
  prefs.putString("trk", (curTrack >= 0 && curTrack < (int)tracks.size()) ? tracks[curTrack] : "");
  prefs.putBool("shuf", shuffleOn);
  prefs.putUChar("rep", repeatMode);
  prefs.putUChar("dmask", dispMask);
  prefs.putUChar("dsecs", dispSecs);
  prefs.putBool("djump", dispSongJump);
  prefs.putBool("ddim", dispNightDim);
  prefs.end();
  Serial.println("settings saved");
}

// ---- rotary encoder -----------------------------------------------------------------
// Rotation is decoded in an interrupt from every CLK/DT edge (a quadrature state
// table rejects contact bounce); the button is polled and debounced in loop().
// Everything it changes goes through the same functions as the web UI.
static const int8_t QDEC[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
static volatile int32_t encEdges = 0;
static volatile uint8_t encPrev = 0;
static portMUX_TYPE encMux = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR encIsr() {
  uint8_t s = (gpio_get_level((gpio_num_t)ENC_CLK) << 1) | gpio_get_level((gpio_num_t)ENC_DT);
  portENTER_CRITICAL_ISR(&encMux);
  encEdges += QDEC[(encPrev << 2) | s];
  encPrev = s;
  portEXIT_CRITICAL_ISR(&encMux);
}

static void setupEncoder() {
  pinMode(ENC_CLK, INPUT_PULLUP);   // the module has its own 10k pull-ups too
  pinMode(ENC_DT, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);
  encPrev = (digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT);
  attachInterrupt(ENC_CLK, encIsr, CHANGE);
  attachInterrupt(ENC_DT, encIsr, CHANGE);
}

static void encTurn(int detents) {
  switch (encMode) {
    case EM_VOL:    setVol((int)targetVol + detents); break;
    case EM_BASS:   setTone(eqBass + detents, eqMid, eqTreble); break;
    case EM_MID:    setTone(eqBass, eqMid + detents, eqTreble); break;
    case EM_TREBLE: setTone(eqBass, eqMid, eqTreble + detents); break;
    case EM_SOURCE: stepSource(detents); break;
  }
}

static void encClick() {
  if (encMode == EM_VOL) { togglePlay("knob"); return; }
  encMode = (encMode + 1) % EM_COUNT;        // next menu item; after source it closes
  Serial.printf("[knob] menu item %d\n", encMode);
}

static void encDoubleClick() {
  if (encMode != EM_VOL) return;
  if (source == SRC_SD) sdNext(false);
}

static void encLongPress() {
  encMode = (encMode == EM_VOL) ? EM_BASS : EM_VOL;   // long press opens or closes the menu
  Serial.printf("[knob] menu %s\n", encMode == EM_VOL ? "closed" : "opened");
}

static void handleEncoder() {
  static int32_t acc = 0;
  static uint32_t lastUse = 0;
  uint32_t now = millis();

  // rotation
  portENTER_CRITICAL(&encMux);
  int32_t edges = encEdges;
  encEdges = 0;
  portEXIT_CRITICAL(&encMux);
  acc += edges;
  int detents = acc / ENC_STEPS_PER_DETENT;
  acc -= detents * ENC_STEPS_PER_DETENT;
  if (ENC_REVERSE) detents = -detents;
  if (detents) {
    if (encMode == EM_SOURCE && now - lastUse < 400) detents = 0;   // one source switch per flick
    else { lastUse = now; encTurn(detents); }
  }

  // button: debounce, then tell click / double-click / long press apart
  static bool raw = false, down = false, longDone = false;
  static uint32_t rawSince = 0, downSince = 0, lastRelease = 0;
  static uint8_t clicks = 0;
  bool r = digitalRead(ENC_SW) == LOW;
  if (r != raw) { raw = r; rawSince = now; }
  if (raw != down && now - rawSince > 25) {
    down = raw;
    lastUse = now;
    if (down) { downSince = now; longDone = false; }
    else if (!longDone) { clicks++; lastRelease = now; }
  }
  if (down && !longDone && now - downSince > 800) { longDone = true; clicks = 0; encLongPress(); }
  if (clicks && !down && now - lastRelease > 300) {   // no second click came
    if (clicks >= 2) encDoubleClick();
    else             encClick();
    clicks = 0;
  }

  // the menu closes itself when left alone, and keeps its overlay up while open
  if (encMode != EM_VOL) {
    if (now - lastUse > 6000) { encMode = EM_VOL; return; }
    overlayKind = encMode == EM_SOURCE ? OV_SOURCE : OV_TONE;
    overlayUntil = now + 300;
  }
}

// ---- setup / loop -----------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== internet radio (web controlled, tone control, NTP clock) ===");

  uiMutex = xSemaphoreCreateMutex();
  loadSettings();

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED init failed");
  }
  display.setTextWrap(false);
  // from here on only the display task touches the OLED and Wire
  xTaskCreatePinnedToCore(uiTask, "oled", 6144, nullptr, 1, nullptr, 0);

  Audio::audio_info_callback = onAudioEvent;
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DIN);
  audio.setVolume(targetVol);       // 0..21
  audio.settings.VU_LEVEL = true;   // enable evt_vu
  audio.settings.SPECTRUM = true;   // enable evt_spectrum (15 bands)
  audio.settings.IIR_FILTER = false;   // tone control is done in audio_process_raw_samples()

  setupEncoder();
  pinMode(SD_CD, INPUT_PULLUP);
  delay(200);                      // give the card time to power up
  mountSd();
  if (bootSource == SRC_SD) {      // it was playing the SD card when it went off: carry on
    int k = findTrack(bootTrack);
    if (!tracks.empty()) playTrack(k >= 0 ? k : 0, false);
    else                 switchToSd();
  }

  Serial.printf("WiFi: joining \"%s\"\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // keep the stream smooth
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  if (source == SRC_RADIO) setStatus("WiFi: joining...");
}

void loop() {
  static bool announced = false;
  static bool firstStream = true;

  wifiUp = WiFi.status() == WL_CONNECTED;

  if (wifiUp && !announced) {
    Serial.print("WiFi connected, IP ");
    Serial.println(WiFi.localIP());
    configTzTime(TZ_INFO, NTP_1, NTP_2, NTP_3);   // SNTP keeps itself in sync from here
    setupWeb();
    if (MDNS.begin("nightowl-radio")) {          // the laptop app finds it by this name
      MDNS.addService("http", "tcp", 80);
      Serial.println("mDNS: nightowl-radio.local");
    }
    announced = true;
  }

  if (source == SRC_RADIO && wifiUp && !audioConnected) {
    setStatus(firstStream ? "WiFi OK - starting" : "reconnecting stream");
    firstStream = false;
    startStream();
  }

  // card inserted or removed: re-read it (the card-detect switch changes level)
  static int lastCd = -1;
  static uint32_t cdChangedAt = 0;
  static uint32_t lastCdPoll = 0;
  if (millis() - lastCdPoll > 200) {
    lastCdPoll = millis();
    int cd = digitalRead(SD_CD);
    if (lastCd < 0) lastCd = cd;
    else if (cd != lastCd) { lastCd = cd; cdChangedAt = millis(); }
    if (cdChangedAt && millis() - cdChangedAt > 600) {   // let the contacts settle
      cdChangedAt = 0;
      Serial.printf("[sd] card %s\n", sdMounted ? "removed or changed" : "inserted");
      rescanSd();
    }
  }

  if (laptopGo) {
    laptopGo = false;
    if (laptopAvailable()) { laptopRetryAt = millis(); source == SRC_LAPTOP ? startLaptop() : switchToLaptop(); }
  }

  // Laptop stream dropped (app restarted, WiFi hiccup): retry while the app is
  // still checking in, otherwise go back to the radio
  if (source == SRC_LAPTOP && audioConnected && !paused && !audio.isRunning()) {
    audioConnected = false;
    laptopRetryAt = millis() + 1000;
    setStatus("laptop stream lost");
  }
  if (source == SRC_LAPTOP && !audioConnected && wifiUp && (int32_t)(millis() - laptopRetryAt) > 0) {
    if (laptopAvailable()) startLaptop();
    else { Serial.println("[laptop] app gone, back to radio"); switchToRadio(); }
  }

  if (source == SRC_SD && sdTrackEnded) {
    sdTrackEnded = false;
    sdNext(true);
  }

  if (!wifiUp) {
    if (millis() - lastReconnect > 10000) {
      lastReconnect = millis();
      Serial.printf("WiFi status %d, retrying\n", (int)WiFi.status());
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      if (source == SRC_RADIO) setStatus("WiFi: retrying...");
    }
  }

  server.handleClient();         // cheap, keeps audio.loop() frequent
  handleEncoder();
  audio.loop();
  saveSettingsIfDue();
}
