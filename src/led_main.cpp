#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <FastLED.h>
#include "config.h"

// ---------------------------------------------------------------------------
// LED hardware — 8 strips of WS2812, 99 LEDs each
// ---------------------------------------------------------------------------
#define NUM_STRIPS          8
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
#define PIN_8   32  // Strip 8

// UART to ESP32-CAM (Serial2 default pins on ESP32)
#define CAM_UART_TX  17
#define CAM_UART_RX  16

// ---------------------------------------------------------------------------
// One distinct color per strip / "personality type"
// ---------------------------------------------------------------------------
const CRGB PATH_COLORS[NUM_STRIPS] = {
  CRGB(255,  30,   0),   // 0 Adventurer — fiery red
  CRGB(  0,  80, 255),   // 1 Thinker   — electric blue
  CRGB(  0, 210,  50),   // 2 Creator   — vivid green
  CRGB(180,   0, 255),   // 3 Leader    — deep purple
  CRGB(255, 140,   0),   // 4 Dreamer   — amber
  CRGB(  0, 220, 220),   // 5 Guardian  — cyan
  CRGB(255, 255,   0),   // 6 Explorer  — yellow
  CRGB(255,  20, 147),   // 7 Mystic    — hot pink
};

CRGB leds[NUM_STRIPS][NUM_LEDS_PER_STRIP];

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------
enum State { IDLE, BUILD, COMPETE, DECIDING, REVEAL };
State state = IDLE;
unsigned long stateStart = 0;

int   winnerPath = -1;
float pathIntensity[NUM_STRIPS];

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

// ---------------------------------------------------------------------------
// LED EFFECTS (unchanged from partner's working code)
// ---------------------------------------------------------------------------

// IDLE — all dark
void updateIdle() {
  FastLED.clear();
}

// BUILD — all strips fill from both ends toward center, 6 seconds, accelerating
void updateBuild(unsigned long elapsed) {
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

// COMPETE — all strips swell organically with sine waves
void updateCompete(unsigned long elapsed) {
  for (int s = 0; s < NUM_STRIPS; s++) {
    float wave   = sinf(elapsed * 0.002f * (0.8f + s * 0.15f) + s * 0.9f);
    float target = 0.55f + 0.40f * wave + (random8() - 128) / 600.0f;
    target = constrain(target, 0.05f, 1.0f);
    pathIntensity[s] += (target - pathIntensity[s]) * 0.12f;
    fillStrip(s, PATH_COLORS[s], (uint8_t)(pathIntensity[s] * 255.0f));
  }
}

// DECIDING — winner brightens/sparkles, others fade (4 seconds)
void updateDeciding(unsigned long elapsed) {
  float t     = constrain(elapsed / 4000.0f, 0.0f, 1.0f);
  float eased = t * t * (3.0f - 2.0f * t);

  for (int s = 0; s < NUM_STRIPS; s++) {
    float intensity;
    if (s == winnerPath) {
      intensity = pathIntensity[s] + (1.0f - pathIntensity[s]) * eased;
      if (random8() > 230) intensity *= 0.75f + 0.25f * (random8() / 255.0f);
    } else {
      intensity = pathIntensity[s] * (1.0f - eased * eased);
    }
    fillStrip(s, PATH_COLORS[s], (uint8_t)(constrain(intensity, 0.0f, 1.0f) * 255));
  }
}

// REVEAL — winning strip breathes with white sparkles (10 seconds)
void updateReveal(unsigned long elapsed) {
  FastLED.clear();
  uint8_t angle  = (uint8_t)(elapsed / 4);
  uint8_t bright = scale8(sin8(angle), 80) + 160;  // 160–240
  fillStrip(winnerPath, PATH_COLORS[winnerPath], bright);

  if (random8() > 200) {
    int pos = random(NUM_LEDS_PER_STRIP);
    leds[winnerPath][pos] = CRGB::White;
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
    enterState(BUILD);
  }
  else if (cmd.startsWith("SCORE:")) {
    int score = cmd.substring(6).toInt();
    Serial.printf("[CMD] Received score: %d\n", score);

    if (score >= 1 && score <= 7) {
      // Map score 1-7 to strip index 0-6
      receivedScore = score - 1;
      scoreReady = true;
    } else {
      // Score 0 — not a crystal or person, abort animation
      Serial.println("[CMD] Score 0 — not a crystal/person, returning to IDLE");
      Serial1.println("DONE");
      enterState(IDLE);
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

  Serial.println("Initializing 8 LED strips...");
  FastLED.addLeds<LED_TYPE, PIN_1, COLOR_ORDER>(leds[0], NUM_LEDS_PER_STRIP);
  FastLED.addLeds<LED_TYPE, PIN_2, COLOR_ORDER>(leds[1], NUM_LEDS_PER_STRIP);
  FastLED.addLeds<LED_TYPE, PIN_3, COLOR_ORDER>(leds[2], NUM_LEDS_PER_STRIP);
  FastLED.addLeds<LED_TYPE, PIN_4, COLOR_ORDER>(leds[3], NUM_LEDS_PER_STRIP);
  FastLED.addLeds<LED_TYPE, PIN_5, COLOR_ORDER>(leds[4], NUM_LEDS_PER_STRIP);
  FastLED.addLeds<LED_TYPE, PIN_6, COLOR_ORDER>(leds[5], NUM_LEDS_PER_STRIP);
  FastLED.addLeds<LED_TYPE, PIN_7, COLOR_ORDER>(leds[6], NUM_LEDS_PER_STRIP);
  FastLED.addLeds<LED_TYPE, PIN_8, COLOR_ORDER>(leds[7], NUM_LEDS_PER_STRIP);
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
    const char* stateNames[] = {"IDLE", "BUILD", "COMPETE", "DECIDING", "REVEAL"};
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
      updateBuild(elapsed);
      if (elapsed > 6000) {
        for (int s = 0; s < NUM_STRIPS; s++) pathIntensity[s] = 0.5f;
        enterState(COMPETE);
      }
      break;

    case COMPETE:
      updateCompete(elapsed);
      // Transition once we have a score from camera
      if (scoreReady && receivedScore >= 0) {
        winnerPath = receivedScore;
        scoreReady = false;
        receivedScore = -1;
        Serial.printf("[STATE] Winner is strip %d — transitioning to DECIDING\n", winnerPath);
        enterState(DECIDING);
      }
      // Safety timeout — 30s without a score
      if (elapsed > 30000) {
        Serial.println("[WARN] Score timeout — falling back to random");
        winnerPath = random(NUM_STRIPS);
        scoreReady = false;
        receivedScore = -1;
        enterState(DECIDING);
      }
      break;

    case DECIDING:
      updateDeciding(elapsed);
      if (elapsed > 4000) enterState(REVEAL);
      break;

    case REVEAL:
      updateReveal(elapsed);
      if (elapsed > 10000) {
        Serial1.println("DONE");
        Serial.println("[STATE] Sent DONE to ESP32-CAM — returning to IDLE");
        enterState(IDLE);
      }
      break;
  }

  FastLED.show();
  delay(20);
}
