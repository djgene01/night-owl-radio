// EncoderProbe - shows which header pins the rotary encoder is really on.
// Every free header pin is read with its internal pull-up; any pin whose level
// changes is printed, with a running count. Turn the knob and press it while
// watching Serial @ 115200. Nothing is driven, so it is safe whatever is wired.
//
// Left out on purpose: 19/20 (USB to the 4G modem), 43/44 (serial port).

static const int PINS[] = {
  1, 2, 21, 3, 0, 41, 40, 39, 45, 42, 18, 17,          // right header
  4, 5, 6, 46, 15, 16, 7, 8, 9, 10, 11, 12, 13, 14      // left header
};
static const int NPINS = sizeof(PINS) / sizeof(PINS[0]);

static int level[NPINS];
static uint32_t changes[NPINS];

void setup() {
  Serial.begin(115200);
  delay(300);
  for (int i = 0; i < NPINS; i++) {
    pinMode(PINS[i], INPUT_PULLUP);
  }
  delay(20);
  Serial.println("\n=== EncoderProbe: turn and press the knob ===");
  Serial.print("idle levels:");
  for (int i = 0; i < NPINS; i++) {
    level[i] = digitalRead(PINS[i]);
    Serial.printf(" IO%d=%d", PINS[i], level[i]);
  }
  Serial.println();
}

void loop() {
  static uint32_t lastSummary = 0;
  for (int i = 0; i < NPINS; i++) {
    int v = digitalRead(PINS[i]);
    if (v != level[i]) {
      level[i] = v;
      changes[i]++;
      Serial.printf("%7lu ms  IO%-2d -> %d   (%lu changes)\n", millis(), PINS[i], v,
                    (unsigned long)changes[i]);
    }
  }
  if (millis() - lastSummary > 5000) {
    lastSummary = millis();
    Serial.print("summary, pins that moved:");
    bool any = false;
    for (int i = 0; i < NPINS; i++) {
      if (changes[i]) { Serial.printf(" IO%d x%lu", PINS[i], (unsigned long)changes[i]); any = true; }
    }
    Serial.println(any ? "" : " none yet");
  }
}
