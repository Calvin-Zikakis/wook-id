#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <FastLED.h>
#include "config.h"

// ---------------------------------------------------------------------------
// LED hardware — 7 strips of WS2812, 99 LEDs each
// ---------------------------------------------------------------------------
#define NUM_STRIPS          7
#define NUM_LEDS_PER_STRIP  99
#define LED_TYPE            WS2812
#define COLOR_ORDER         GRB

#define PIN_1   13  // Strip 1
#define PIN_2   12  // Strip 2
#define PIN_3   14  // Strip 3
#define PIN_4   27  // Strip 4
#define PIN_5   26  // Strip 5
#define PIN_6   25  // Strip 6
#define PIN_7   33  // Strip 7

// UART to ESP32-CAM (Serial2 default pins on ESP32)
#define CAM_UART_TX  17
#define CAM_UART_RX  16

// ---------------------------------------------------------------------------
// One distinct color per strip / "wook2woke type"
// ---------------------------------------------------------------------------
const CRGB PATH_COLORS[NUM_STRIPS] = {
  CRGB(255,  30,   0),   // 0 Level 3 wook — fiery red
  CRGB(  0,  80, 255),   // 1 Level 2 wook — electric blue
  CRGB(  0, 210,  50),   // 2 Level 1 wook — vivid green
  CRGB(180,   0, 255),   // 3 Normie — deep purple
  CRGB(255, 140,   0),   // 4 Level 1 woke — amber
  CRGB(  0, 220, 220),   // 5 Level 2 woke — cyan
  CRGB(255, 255,   0),   // 6 Level 3 woke  — yellow
};

CRGB leds[NUM_STRIPS][NUM_LEDS_PER_STRIP];

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------
enum State { IDLE, BUILD, COMPETE, DECIDING, REVEAL, UNIDENTIFIED };
State state = IDLE;
unsigned long stateStart = 0;

int   winnerPath = -1;
float pathIntensity[NUM_STRIPS];

int buildAnim = 0;
int buildFillDir = 0;          // 0=from start, 1=from end, 2=both ends
int competeAnim = 0;           // 0=random LEDs, 1=comets, 2=starfield, 3=fireflies
bool competeScoreReceived = false;

// Starfield state
uint8_t starBright[NUM_STRIPS][NUM_LEDS_PER_STRIP];

// Firefly state
#define NUM_FIREFLIES 12
struct Firefly {
  float   pos;
  float   vel;
  uint8_t bright;
  int8_t  brightDir;   // 1=fading in, -1=fading out
  uint8_t hue;
  uint8_t strip;
  uint8_t size;        // 1–3 LEDs
};
Firefly fireflies[NUM_FIREFLIES];
int buildStripOrder[NUM_STRIPS];
unsigned long buildLoopCount = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
void enterState(State s) {
  state      = s;
  stateStart = millis();
}

void fillStrip(int s, CRGB color, uint8_t bright) {
  CRGB c = color;
  c.nscale8(bright);
  for (int i = 0; i < NUM_LEDS_PER_STRIP; i++) leds[s][i] = c;
}

float triangleWave(unsigned long elapsed, float cycleDurationMs) {
  float phase = fmod((float)elapsed, cycleDurationMs) / cycleDurationMs;
  return (phase < 0.5f) ? phase * 2.0f : (1.0f - phase) * 2.0f;
}

void shuffleStrips() {
  for (int i = 0; i < NUM_STRIPS; i++) buildStripOrder[i] = i;
  for (int i = NUM_STRIPS - 1; i > 0; i--) {
    int j = random(i + 1);
    int tmp = buildStripOrder[i];
    buildStripOrder[i] = buildStripOrder[j];
    buildStripOrder[j] = tmp;
  }
}

void initStarfield() {
  memset(starBright, 0, sizeof(starBright));
}

void initFireflies() {
  for (int f = 0; f < NUM_FIREFLIES; f++) {
    fireflies[f].strip     = random(NUM_STRIPS);
    fireflies[f].pos       = random(NUM_LEDS_PER_STRIP);
    fireflies[f].vel       = (random8() > 127 ? 1.0f : -1.0f) * (0.05f + random8() / 800.0f);
    fireflies[f].bright    = 0;
    fireflies[f].brightDir = 1;
    fireflies[f].hue       = 40 + random8(40);   // yellow-green
    fireflies[f].size      = 1 + random(3);       // 1–3 LEDs
  }
}

// ---------------------------------------------------------------------------
// LED EFFECTS (unchanged from partner's working code)
// ---------------------------------------------------------------------------

// IDLE — all dark
void updateIdle() {
  FastLED.setBrightness(200);
  FastLED.clear();
}

// UNIDENTIFIED — all strips breathe red for 5 seconds
void updateUnidentified(unsigned long elapsed) {
  FastLED.setBrightness(255);
  uint8_t bright = scale8(sin8((uint8_t)(elapsed / 8)), 180) + 40;  // 40–220
  for (int s = 0; s < NUM_STRIPS; s++)
    fillStrip(s, CRGB::Red, bright);
}

// BUILD — all strips fill from both ends toward center, 6 seconds, accelerating
void updateBuild(unsigned long elapsed) {
  FastLED.setBrightness(200);
  float progress        = constrain(elapsed / 6000.0f, 0.0f, 1.0f);
  float cycleDurationMs = 1800.0f - progress * 1200.0f;  // 1.8s → 0.6s
  float fillFraction    = triangleWave(elapsed, cycleDurationMs);
  uint8_t brightness    = (uint8_t)(50 + progress * 205);

  FastLED.clear();
  int half = NUM_LEDS_PER_STRIP / 2;
  int fillCount = (int)(fillFraction * (half + 1));

  for (int s = 0; s < NUM_STRIPS; s++) {
    CRGB c = PATH_COLORS[s];
    c.nscale8(brightness);
    for (int i = 0; i < fillCount; i++) {
      leds[s][i] = c;
      leds[s][NUM_LEDS_PER_STRIP - 1 - i] = c;
    }
  }
}

// BUILD anim 1 — all strips fill, accelerating; direction chosen randomly per run
void updateBuildAllFill(unsigned long elapsed) {
  FastLED.setBrightness(200);
  FastLED.clear();
  float t = constrain((float)elapsed / 6000.0f, 0.0f, 1.0f);
  int totalLit = constrain((int)(t * t * NUM_LEDS_PER_STRIP), 0, NUM_LEDS_PER_STRIP);
  for (int s = 0; s < NUM_STRIPS; s++) {
    for (int i = 0; i < totalLit; i++) {
      if (buildFillDir == 0) {
        leds[s][i] = PATH_COLORS[s];                              // start → end
      } else if (buildFillDir == 1) {
        leds[s][NUM_LEDS_PER_STRIP - 1 - i] = PATH_COLORS[s];   // end → start
      } else {
        if (i % 2 == 0)
          leds[s][i / 2] = PATH_COLORS[s];                       // both ends → middle
        else
          leds[s][NUM_LEDS_PER_STRIP - 1 - (i / 2)] = PATH_COLORS[s];
      }
    }
  }
}

// BUILD anim 2 — one strip at a time, random order, full brightness then off
void updateBuildSequential(unsigned long elapsed) {
  FastLED.setBrightness(200);
  FastLED.clear();
  float slotMs = 6000.0f / NUM_STRIPS;
  int slotIdx  = constrain((int)(elapsed / slotMs), 0, NUM_STRIPS - 1);
  float slotElapsed = fmod((float)elapsed, slotMs);
  if (slotElapsed < slotMs * 0.8f) {
    int s = buildStripOrder[slotIdx];
    fillStrip(s, PATH_COLORS[s], 255);
  }
}

// BUILD anim 3 — brightness ramps 0→200 while LEDs blink random colors at increasing rates
void updateBuildRainbow(unsigned long elapsed) {
  FastLED.setBrightness(200);
  float progress  = constrain((float)elapsed / 6000.0f, 0.0f, 1.0f);
  uint8_t baseBright = (uint8_t)(progress * 200);

  for (int s = 0; s < NUM_STRIPS; s++) {
    for (int i = 0; i < NUM_LEDS_PER_STRIP; i++) {
      uint16_t seed = (uint16_t)(s * 99 + i);
      // Each LED has a unique period 100–1000ms, shrinking toward 50ms as progress increases
      uint32_t period = (uint32_t)((100 + (seed * 37) % 900) * (1.0f - progress * 0.9f));
      period = max(period, (uint32_t)50);
      // Toggle on/off by period; use deterministic hue per LED
      if ((elapsed / period) % 2 == 0) {
        leds[s][i] = CHSV((uint8_t)(seed * 53), 255, 255);
      } else {
        leds[s][i].setRGB(baseBright, baseBright, baseBright);
      }
    }
  }
}

// BUILD anim 4 — comet: staggered per strip, random color per pass
void updateBuildComet(unsigned long elapsed) {
  FastLED.setBrightness(200);
  FastLED.clear();

  const int   COMET_LEN = 5;
  const float PERIOD_MS = 2000.0f;
  const int   TRAVEL    = NUM_LEDS_PER_STRIP + COMET_LEN;
  const float OFFSET_MS = PERIOD_MS / NUM_STRIPS;           // evenly stagger strips

  for (int s = 0; s < NUM_STRIPS; s++) {
    float adjusted = elapsed + s * OFFSET_MS;
    int   passNum  = (int)(adjusted / PERIOD_MS);
    float t        = fmod(adjusted, PERIOD_MS) / PERIOD_MS;
    int   headPos  = (int)(t * TRAVEL) - COMET_LEN;

    // deterministic random hue — different per strip and per pass
    uint8_t hue = (uint8_t)((passNum * 73 + s * 41) * 37);

    for (int tail = 0; tail < COMET_LEN; tail++) {
      int idx = headPos - tail;
      if (idx >= 0 && idx < NUM_LEDS_PER_STRIP) {
        uint8_t bright = 255 - (tail * (255 / COMET_LEN));
        leds[s][idx] = CHSV(hue, 255, bright);
      }
    }
  }
}

// BUILD anim 5 — breathe: all strips fade in/out together, hue shifts across spectrum over 6s
void updateBuildBreathe(unsigned long elapsed) {
  FastLED.setBrightness(200);
  uint8_t hue    = (uint8_t)(elapsed * 255 / 6000);         // full spectrum over 6 seconds
  uint8_t bright = scale8(sin8((uint8_t)(elapsed / 8)), 180) + 40;  // 40–220, ~1.3s cycle
  for (int s = 0; s < NUM_STRIPS; s++) {
    for (int i = 0; i < NUM_LEDS_PER_STRIP; i++) {
      leds[s][i] = CHSV(hue, 255, bright);
    }
  }
}

// COMPETE — random LEDs across all strips slowly fade in and out in random colors
void updateCompete(unsigned long elapsed) {
  FastLED.setBrightness(200);
  for (int s = 0; s < NUM_STRIPS; s++) {
    for (int i = 0; i < NUM_LEDS_PER_STRIP; i++) {
      uint16_t seed   = (uint16_t)(s * 99 + i);
      float period    = 800.0f + (float)((seed * 37) % 3200);   // 0.8–4s per LED
      uint8_t phaseOffset = (uint8_t)((seed * 53) & 0xFF);
      uint8_t phase   = (uint8_t)((float)elapsed / period * 255.0f) + phaseOffset;
      uint8_t bright  = sin8(phase);
      if (bright < 50) bright = 0;   // spend time fully off
      uint8_t cycleNum = (uint8_t)((float)elapsed / period);
      uint8_t hue     = (uint8_t)(seed * 53 + cycleNum * 79);   // new color each cycle
      leds[s][i] = (bright > 0) ? CRGB(CHSV(hue, 255, bright)) : CRGB::Black;
    }
  }
}

// COMPETE anim 1 — comets bouncing back and forth on all strips, each strip its own color
void updateCompeteComet(unsigned long elapsed) {
  FastLED.setBrightness(200);
  FastLED.clear();

  const int   COMET_LEN = 5;
  const float PERIOD_MS = 1600.0f;                          // one back-and-forth every 1.6s
  const float OFFSET_MS = PERIOD_MS / NUM_STRIPS;

  for (int s = 0; s < NUM_STRIPS; s++) {
    float adjusted = elapsed + s * OFFSET_MS;
    int   passNum  = (int)(adjusted / PERIOD_MS);
    float t        = fmod(adjusted, PERIOD_MS) / PERIOD_MS;

    // Ping-pong: 0→1 first half, 1→0 second half
    float pos      = (t < 0.5f) ? t * 2.0f : (1.0f - t) * 2.0f;
    int   headPos  = (int)(pos * (NUM_LEDS_PER_STRIP - 1));
    int   dir      = (t < 0.5f) ? 1 : -1;

    uint8_t hue = (uint8_t)((passNum * 73 + s * 41) * 37);

    for (int tail = 0; tail < COMET_LEN; tail++) {
      int idx = headPos - dir * tail;
      if (idx >= 0 && idx < NUM_LEDS_PER_STRIP) {
        uint8_t bright = 255 - (tail * (255 / COMET_LEN));
        leds[s][idx] = CHSV(hue, 255, bright);
      }
    }
  }
}

// COMPETE anim 2 — starfield: sparse LEDs pop on and slowly fade out
void updateCompeteStarfield() {
  FastLED.setBrightness(200);
  // Fade all stars down each frame
  for (int s = 0; s < NUM_STRIPS; s++)
    for (int i = 0; i < NUM_LEDS_PER_STRIP; i++)
      if (starBright[s][i] > 3) starBright[s][i] -= 3; else starBright[s][i] = 0;

  // Randomly ignite new stars
  for (int n = 0; n < 4; n++) {
    if (random8() > 200) {
      int s = random(NUM_STRIPS);
      int i = random(NUM_LEDS_PER_STRIP);
      starBright[s][i] = 180 + random8(75);
    }
  }

  // Render: white with a faint blue tint
  for (int s = 0; s < NUM_STRIPS; s++) {
    for (int i = 0; i < NUM_LEDS_PER_STRIP; i++) {
      uint8_t b = starBright[s][i];
      leds[s][i] = (b > 0) ? CRGB(b, b, min(255, (int)b + 30)) : CRGB::Black;
    }
  }
}

// COMPETE anim 3 — fireflies: small glowing clusters drift slowly along strips
void updateCompeteFireflies() {
  FastLED.setBrightness(200);
  FastLED.clear();

  for (int f = 0; f < NUM_FIREFLIES; f++) {
    Firefly& ff = fireflies[f];

    // Drift
    ff.pos += ff.vel;
    if (ff.pos < 0)                      { ff.pos = 0;                      ff.vel = -ff.vel; }
    if (ff.pos >= NUM_LEDS_PER_STRIP - 1){ ff.pos = NUM_LEDS_PER_STRIP - 1; ff.vel = -ff.vel; }

    // Breathe in then out
    if (ff.brightDir == 1) {
      ff.bright = min(220, ff.bright + 2);
      if (ff.bright >= 220) ff.brightDir = -1;
    } else {
      if (ff.bright > 2) ff.bright -= 2;
      else {
        // Respawn at new random location
        ff.strip     = random(NUM_STRIPS);
        ff.pos       = random(NUM_LEDS_PER_STRIP);
        ff.vel       = (random8() > 127 ? 1.0f : -1.0f) * (0.05f + random8() / 800.0f);
        ff.bright    = 0;
        ff.brightDir = 1;
        ff.hue       = 40 + random8(40);
        ff.size      = 1 + random(3);
      }
    }

    // Render with tail fade
    int head = (int)ff.pos;
    for (int t = 0; t < ff.size; t++) {
      int idx = head + t;
      if (idx < NUM_LEDS_PER_STRIP) {
        uint8_t b = ff.bright / (t + 1);
        leds[ff.strip][idx] += CHSV(ff.hue, 200, b);
      }
    }
  }
}

// DECIDING — quick 0.5s fade to black before REVEAL
void updateDeciding(unsigned long elapsed) {
  FastLED.setBrightness(200);
  for (int s = 0; s < NUM_STRIPS; s++)
    for (int i = 0; i < NUM_LEDS_PER_STRIP; i++)
      leds[s][i].fadeToBlackBy(8);
}

// REVEAL — fill from both ends toward center, then breathe with sparkles
#define REVEAL_MS_PER_LED   125UL                                      // 8 LEDs/sec
#define REVEAL_FILL_MS      ((unsigned long)NUM_LEDS_PER_STRIP * REVEAL_MS_PER_LED)  // ~12.4s
#define REVEAL_TOTAL_MS     (REVEAL_FILL_MS + 8000UL)                 // fill + 8s breathe

void updateReveal(unsigned long elapsed) {
  FastLED.clear();
  FastLED.setBrightness(255);

  if (elapsed < REVEAL_FILL_MS) {
    // Phase 1: fill from both ends toward center, accelerating (ease-in)
    // sequence: index 0, 98, 1, 97, 2, 96 ...
    float t = constrain((float)elapsed / (float)REVEAL_FILL_MS, 0.0f, 1.0f);
    int totalLit = constrain((int)(t * t * NUM_LEDS_PER_STRIP), 0, NUM_LEDS_PER_STRIP);
    for (int i = 0; i < totalLit; i++) {
      if (i % 2 == 0)
        leds[winnerPath][i / 2] = PATH_COLORS[winnerPath];
      else
        leds[winnerPath][NUM_LEDS_PER_STRIP - 1 - (i / 2)] = PATH_COLORS[winnerPath];
    }
  } else {
    // Phase 2: full strip breathing
    uint8_t angle = (uint8_t)(elapsed / 3);
    uint8_t bright = scale8(sin8(angle), 80) + 160;       // 160–240
    fillStrip(winnerPath, PATH_COLORS[winnerPath], bright);
  }
}

// ---------------------------------------------------------------------------
// OTA web server
// ---------------------------------------------------------------------------
WebServer otaServer(80);

// ---------------------------------------------------------------------------
// Serial command buffer
// ---------------------------------------------------------------------------
String serialBuffer = "";

// Score received from camera (set when SCORE:X arrives)
volatile int  receivedScore = -1;
volatile bool scoreReady    = false;

// ---------------------------------------------------------------------------
// Pick a random build animation and enter BUILD state
// ---------------------------------------------------------------------------
void startBuild() {
  buildAnim = random(6);  // 0=pulse, 1=allFill, 2=sequential, 3=rainbow, 4=comet, 5=breathe
  if (buildAnim == 1) buildFillDir = random(3);  // 0=from start, 1=from end, 2=both ends
  if (buildAnim == 2) shuffleStrips();
  Serial.printf("[BUILD] Animation %d selected%s\n", buildAnim,
    buildAnim == 1 ? (buildFillDir == 0 ? " (fill from start)" : buildFillDir == 1 ? " (fill from end)" : " (fill both ends)") : "");
  enterState(BUILD);
}

// ---------------------------------------------------------------------------
// Process text commands from ESP32-CAM
// ---------------------------------------------------------------------------
void processCommand(String cmd) {
  cmd.trim();
  Serial.printf("[CMD] Received: '%s'\n", cmd.c_str());

  if (cmd == "PING") {
    Serial1.println("PONG");
    Serial.println("[CMD] Replied PONG");
  }
  else if (cmd == "START") {
    Serial.println("[CMD] Camera says START — beginning BUILD animation");
    startBuild();
  }
  else if (cmd.startsWith("SCORE:")) {
    int score = cmd.substring(6).toInt();
    Serial.printf("[CMD] Received score: %d\n", score);

    if (score >= 1 && score <= 7) {
      // Map score 1-7 to strip index 0-6
      // Map score 1-7 to physical strip index
      const int SCORE_TO_STRIP[8] = {0, 4, 6, 3, 1, 5, 2, 0};  // index 0 unused
      receivedScore = SCORE_TO_STRIP[score];
      scoreReady = true;
    } else {
      // Score 0 — not a crystal or person, breathe red then return to IDLE
      Serial.println("[CMD] Score 0 — not identified, entering UNIDENTIFIED");
      enterState(UNIDENTIFIED);
    }
  }
}

// ---------------------------------------------------------------------------
// Read serial data (non-blocking, line-oriented)
// ---------------------------------------------------------------------------
void readSerial() {
  while (Serial1.available()) {
    char c = Serial1.read();
    if (c == '\n') {
      if (serialBuffer.length() > 0) {
        processCommand(serialBuffer);
        serialBuffer = "";
      }
    } else if (c != '\r') {
      serialBuffer += c;
    }
  }
}

// ---------------------------------------------------------------------------
// SETUP
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(3000);  // USB CDC needs time to connect
  Serial.println("\n\n=== Lolin S3 LED Controller ===");

  Serial.printf("UART to CAM: TX=%d, RX=%d, baud=%d\n", CAM_UART_TX, CAM_UART_RX, UART_BAUD);
  Serial1.begin(UART_BAUD, SERIAL_8N1, CAM_UART_RX, CAM_UART_TX);

  Serial.println("Initializing 7 LED strips...");
  FastLED.addLeds<LED_TYPE, PIN_1, COLOR_ORDER>(leds[0], NUM_LEDS_PER_STRIP);
  FastLED.addLeds<LED_TYPE, PIN_2, COLOR_ORDER>(leds[1], NUM_LEDS_PER_STRIP);
  FastLED.addLeds<LED_TYPE, PIN_3, COLOR_ORDER>(leds[2], NUM_LEDS_PER_STRIP);
  FastLED.addLeds<LED_TYPE, PIN_4, COLOR_ORDER>(leds[3], NUM_LEDS_PER_STRIP);
  FastLED.addLeds<LED_TYPE, PIN_5, COLOR_ORDER>(leds[4], NUM_LEDS_PER_STRIP);
  FastLED.addLeds<LED_TYPE, PIN_6, COLOR_ORDER>(leds[5], NUM_LEDS_PER_STRIP);
  FastLED.addLeds<LED_TYPE, PIN_7, COLOR_ORDER>(leds[6], NUM_LEDS_PER_STRIP);
  FastLED.setBrightness(200);
  Serial.println("LED strips initialized");

  randomSeed(analogRead(1));

  // WiFi for OTA
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Connecting to %s", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.printf("Connected! IP: http://%s\n", WiFi.localIP().toString().c_str());

  // Web OTA upload page
  otaServer.on("/update", HTTP_GET, []() {
    otaServer.send(200, "text/html",
      "<html><body style='font-family:monospace;background:#0a0a0f;color:#e0d4f5;text-align:center;padding:40px'>"
      "<h2>LED Controller OTA Update</h2>"
      "<form method='POST' action='/update' enctype='multipart/form-data'>"
      "<input type='file' name='firmware' accept='.bin' style='margin:20px'><br>"
      "<input type='submit' value='Upload & Flash' style='padding:10px 30px;font-size:16px;cursor:pointer'>"
      "</form></body></html>");
  });
  otaServer.on("/update", HTTP_POST, []() {
    otaServer.sendHeader("Connection", "close");
    otaServer.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK - Rebooting...");
    delay(500);
    ESP.restart();
  }, []() {
    HTTPUpload& upload = otaServer.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("OTA Update: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
        Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) Serial.printf("OTA Success: %u bytes\n", upload.totalSize);
      else Update.printError(Serial);
    }
  });
  otaServer.begin();

  // ArduinoOTA for PlatformIO network uploads
  ArduinoOTA.setHostname("wook-or-woke-leds");
  ArduinoOTA.onStart([]() { Serial.println("OTA Start"); });
  ArduinoOTA.onEnd([]() { Serial.println("\nOTA End"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();
  Serial.println("ArduinoOTA ready");

  Serial.println("=== Ready — waiting for commands from ESP32-CAM ===\n");
}

// ---------------------------------------------------------------------------
// MAIN LOOP
// ---------------------------------------------------------------------------
void loop() {
  readSerial();
  otaServer.handleClient();
  ArduinoOTA.handle();

  // Heartbeat + state debug every 2 seconds
  static unsigned long lastDebug = 0;
  static State lastPrintedState = IDLE;
  if (state != lastPrintedState || millis() - lastDebug > 2000) {
    const char* stateNames[] = {"IDLE", "BUILD", "COMPETE", "DECIDING", "REVEAL", "UNIDENTIFIED"};
    Serial.printf("[STATE] %s  (elapsed: %lums)\n", stateNames[state], millis() - stateStart);
    lastPrintedState = state;
    lastDebug = millis();
  }

  unsigned long elapsed = millis() - stateStart;

  switch (state) {
    case IDLE:
      updateIdle();
      break;

    case BUILD:
    {
      // Loop animation every 6s; reshuffle sequential each cycle
      unsigned long loopNum     = elapsed / 6000;
      unsigned long loopElapsed = elapsed % 6000;
      if (loopNum != buildLoopCount) {
        buildLoopCount = loopNum;
        if (buildAnim == 2) shuffleStrips();
      }
      switch (buildAnim) {
        case 1:  updateBuildAllFill(loopElapsed);    break;
        case 2:  updateBuildSequential(loopElapsed); break;
        case 3:  updateBuildRainbow(loopElapsed);    break;
        case 4:  updateBuildComet(loopElapsed);      break;
        case 5:  updateBuildBreathe(loopElapsed);    break;
        default: updateBuild(loopElapsed);           break;
      }
      if (elapsed > 6000) {
        competeAnim = random(4);
        competeScoreReceived = false;
        if (competeAnim == 2) initStarfield();
        if (competeAnim == 3) initFireflies();
        Serial.printf("[COMPETE] Animation %d selected\n", competeAnim);
        enterState(COMPETE);
      }
      break;
    }

    case COMPETE:
      switch (competeAnim) {
        case 1:  updateCompeteComet(elapsed);  break;
        case 2:  updateCompeteStarfield();     break;
        case 3:  updateCompeteFireflies();     break;
        default: updateCompete(elapsed);       break;
      }
      if (scoreReady && receivedScore >= 0 && !competeScoreReceived) {
        winnerPath = receivedScore;
        scoreReady = false;
        receivedScore = -1;
        competeScoreReceived = true;
        Serial.printf("[STATE] Winner is strip %d — waiting for 2s minimum\n", winnerPath);
      }
      if (competeScoreReceived && elapsed >= 3000) {
        Serial.println("[STATE] Transitioning to DECIDING");
        enterState(DECIDING);
      }
      // Safety timeout — 60s without a score
      if (elapsed > 60000) {
        Serial.println("[WARN] Score timeout — falling back to random");
        winnerPath = random(NUM_STRIPS);
        scoreReady = false;
        receivedScore = -1;
        enterState(DECIDING);
      }
      break;

    case DECIDING:
      updateDeciding(elapsed);
      if (elapsed > 2000) enterState(REVEAL);
      break;

    case UNIDENTIFIED:
      updateUnidentified(elapsed);
      if (elapsed > 5000) {
        Serial1.println("DONE");
        Serial.println("[STATE] Unidentified — returning to IDLE");
        enterState(IDLE);
      }
      break;

    case REVEAL:
      updateReveal(elapsed);
      if (elapsed > REVEAL_TOTAL_MS) {
        Serial1.println("DONE");
        Serial.println("[STATE] Sent DONE to ESP32-CAM — returning to IDLE");
        enterState(IDLE);
      }
      break;
  }

  FastLED.show();
  delay(20);
}
