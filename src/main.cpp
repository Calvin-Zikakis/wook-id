#include <Arduino.h>
#include <FastLED.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include "mbedtls/base64.h"
#include "config.h"

// ---------------------------------------------------------------------------
// LED hardware
// ---------------------------------------------------------------------------
#define NUM_STRIPS          8
#define NUM_LEDS_PER_STRIP  99
#define LED_TYPE            WS2812
#define COLOR_ORDER         GRB

#define PIN_1   1   // Strip 1 data (confirmed working)
#define PIN_2   2   // Strip 2 data (confirmed working)
#define PIN_3   3   // Strip 3 data (confirmed working)
#define PIN_4   5   // Strip 4 data (GPIO 4 skipped)
#define PIN_5   6   // Strip 5 data (confirmed working)
#define PIN_6   7   // Strip 6 data (confirmed working)
#define PIN_7   8   // Strip 7 data (confirmed working)
#define PIN_8   9   // Strip 8 data (confirmed working)

// Reserved pins:
// GPIO 10-13 — SPI (future)
// GPIO 17    — UART TX to ESP32-CAM
// GPIO 18    — UART RX from ESP32-CAM

#define CAM_UART_TX  17
#define CAM_UART_RX  18

// ---------------------------------------------------------------------------
// One distinct color per strip / "type"
// ---------------------------------------------------------------------------
const CRGB PATH_COLORS[NUM_STRIPS] = {
  CRGB(255,  30,   0),   // 1 Adventurer — fiery red
  CRGB(  0,  80, 255),   // 2 Thinker   — electric blue
  CRGB(  0, 210,  50),   // 3 Creator   — vivid green
  CRGB(180,   0, 255),   // 4 Leader    — deep purple
  CRGB(255, 140,   0),   // 5 Dreamer   — amber
  CRGB(  0, 220, 220),   // 6 Guardian  — cyan
  CRGB(255, 255,   0),   // 7 Explorer  — yellow
  CRGB(255,  20, 147),   // 8 Mystic    — hot pink
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
// Image buffer (received from ESP32-CAM over UART)
// ---------------------------------------------------------------------------
uint8_t* imageBuffer = NULL;
uint32_t imageSize   = 0;

// ---------------------------------------------------------------------------
// Web server — keeps a copy of the last image + Claude's response
// ---------------------------------------------------------------------------
WebServer server(80);
uint8_t*  lastImage     = NULL;
uint32_t  lastImageSize = 0;
String    lastApiResponse = "(no API call yet)";
int       lastWinner      = -1;

// ---------------------------------------------------------------------------
// API result (written by background task on Core 0)
// ---------------------------------------------------------------------------
volatile int  apiResult = -1;
volatile bool apiDone   = false;

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
// LED EFFECTS
// ---------------------------------------------------------------------------

// IDLE — all dark
void updateIdle() {
  FastLED.clear();
}

// BUILD — all strips fill from both ends toward the tip, then retract.
// Gets faster and brighter over 6 seconds.
void updateBuild(unsigned long elapsed) {
  float progress        = constrain(elapsed / 6000.0f, 0.0f, 1.0f);
  float cycleDurationMs = 1800.0f - progress * 1200.0f;  // 1.8s → 0.6s
  float fillFraction    = triangleWave(elapsed, cycleDurationMs);
  uint8_t brightness    = (uint8_t)(50 + progress * 205);

  FastLED.clear();
  int half = NUM_LEDS_PER_STRIP / 2;  // 49
  int fillCount = (int)(fillFraction * (half + 1));

  for (int s = 0; s < NUM_STRIPS; s++) {
    CRGB c = PATH_COLORS[s];
    c.nscale8(brightness);
    for (int i = 0; i < fillCount; i++) {
      leds[s][i] = c;                              // From start
      leds[s][NUM_LEDS_PER_STRIP - 1 - i] = c;    // From end
    }
  }
}

// COMPETE — all strips swell organically; runs until API returns.
void updateCompete(unsigned long elapsed) {
  for (int s = 0; s < NUM_STRIPS; s++) {
    float wave   = sinf(elapsed * 0.002f * (0.8f + s * 0.15f) + s * 0.9f);
    float target = 0.55f + 0.40f * wave + (random8() - 128) / 600.0f;
    target = constrain(target, 0.05f, 1.0f);
    pathIntensity[s] += (target - pathIntensity[s]) * 0.12f;
    fillStrip(s, PATH_COLORS[s], (uint8_t)(pathIntensity[s] * 255.0f));
  }
}

// DECIDING — winner brightens, others fade out over 4 seconds.
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

// REVEAL — winning strip breathes for 10 seconds, all others dark.
void updateReveal(unsigned long elapsed) {
  FastLED.clear();
  uint8_t angle  = (uint8_t)(elapsed / 4);
  uint8_t bright = scale8(sin8(angle), 80) + 160;  // 160–240
  fillStrip(winnerPath, PATH_COLORS[winnerPath], bright);

  // Sparkles
  if (random8() > 200) {
    int pos = random(NUM_LEDS_PER_STRIP);
    leds[winnerPath][pos] = CRGB::White;
  }
}

// ---------------------------------------------------------------------------
// CLAUDE API CALL — runs on Core 0 as a FreeRTOS task so LEDs keep animating
// ---------------------------------------------------------------------------
void apiCallTask(void* param) {
  Serial.printf("[API] Starting — image is %u bytes\n", imageSize);
  Serial.printf("[API] Free heap: %u, free PSRAM: %u\n", ESP.getFreeHeap(), ESP.getFreePsram());

  // Calculate required buffer size
  size_t base64Len = 0;
  mbedtls_base64_encode(NULL, 0, &base64Len, imageBuffer, imageSize);
  Serial.printf("[API] Need %u bytes for base64\n", base64Len);

  // Try PSRAM first, fall back to heap
  char* base64Buf = (char*)ps_malloc(base64Len + 1);
  if (!base64Buf) {
    Serial.println("[API] ps_malloc failed, trying regular malloc...");
    base64Buf = (char*)malloc(base64Len + 1);
  }
  if (!base64Buf) {
    Serial.println("[API] All allocation failed — falling back to random");
    apiResult = random(NUM_STRIPS);
    apiDone   = true;
    vTaskDelete(NULL);
    return;
  }
  Serial.println("[API] Base64 buffer allocated");

  mbedtls_base64_encode((unsigned char*)base64Buf, base64Len + 1,
                         &base64Len, imageBuffer, imageSize);
  base64Buf[base64Len] = '\0';

  // Free image buffer — no longer needed
  free(imageBuffer);
  imageBuffer = NULL;

  Serial.printf("[API] Sending request (%u bytes base64)...\n", base64Len);

  // Build HTTPS request
  WiFiClientSecure client;
  client.setInsecure();  // Skip cert verification (fine for art installation)

  HTTPClient http;
  http.begin(client, "https://api.anthropic.com/v1/messages");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-api-key", CLAUDE_API_KEY);
  http.addHeader("anthropic-version", "2023-06-01");
  http.setTimeout(30000);

  // Pre-reserve the full payload size to avoid reallocation failures
  String payload;
  if (!payload.reserve(base64Len + 500)) {
    Serial.println("[API] Failed to reserve String memory — falling back to random");
    free(base64Buf);
    http.end();
    apiResult = random(NUM_STRIPS);
    apiDone = true;
    vTaskDelete(NULL);
    return;
  }

  payload = "{\"model\":\"" CLAUDE_MODEL "\","
            "\"max_tokens\":10,"
            "\"messages\":[{\"role\":\"user\",\"content\":["
            "{\"type\":\"image\",\"source\":{"
            "\"type\":\"base64\",\"media_type\":\"image/jpeg\","
            "\"data\":\"";
  payload += base64Buf;

  // Free base64 buffer immediately to reclaim memory for TLS
  free(base64Buf);
  base64Buf = NULL;

  payload += "\"}},"
             "{\"type\":\"text\",\"text\":\"" CLASSIFY_PROMPT "\"}]}]}";

  Serial.printf("[API] Payload built (%u bytes), free heap: %u\n", payload.length(), ESP.getFreeHeap());
  int httpCode = http.POST(payload);
  Serial.printf("[API] HTTP response code: %d\n", httpCode);
  payload = "";  // Free the String memory

  String response = http.getString();
  lastApiResponse = "HTTP " + String(httpCode) + "\n" + response;
  Serial.printf("[API] HTTP %d — full response:\n", httpCode);
  Serial.println(response);
  Serial.println("[API] --- end response ---");

  if (httpCode == 200) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, response);

    if (!error) {
      const char* text = doc["content"][0]["text"];
      if (text) {
        Serial.printf("[API] Claude's answer: \"%s\"\n", text);
        int result = atoi(text);
        if (result >= 1 && result <= 8) {
          apiResult = result - 1;  // Convert to 0-indexed
          lastWinner = apiResult;
          Serial.printf("[API] Chose type %d (strip index %d)\n", result, apiResult);
        } else {
          Serial.printf("[API] Not a valid 1-8 — falling back to random\n");
          apiResult = random(NUM_STRIPS);
        }
      } else {
        Serial.println("[API] No text in response — falling back to random");
        apiResult = random(NUM_STRIPS);
      }
    } else {
      Serial.printf("[API] JSON parse error: %s\n", error.c_str());
      apiResult = random(NUM_STRIPS);
    }
  } else {
    Serial.println("[API] Request failed — falling back to random");
    apiResult = random(NUM_STRIPS);
  }

  http.end();
  apiDone = true;
  vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// UART — receive image from ESP32-CAM (non-blocking, called from loop)
// ---------------------------------------------------------------------------
void receiveImage() {
  static enum { WAIT_START, WAIT_SIZE, WAIT_DATA } rxState = WAIT_START;
  static uint32_t bytesReceived     = 0;
  static uint8_t  sizeBytes[4];
  static uint8_t  sizeBytesReceived = 0;

  while (Serial1.available()) {
    uint8_t b = Serial1.read();

    switch (rxState) {
      case WAIT_START:
        if (b == MSG_START) {
          Serial.println("[UART] START received");
          sizeBytesReceived = 0;
          rxState = WAIT_SIZE;
          // Kick off the LED animation immediately
          enterState(BUILD);
        }
        break;

      case WAIT_SIZE:
        sizeBytes[sizeBytesReceived++] = b;
        if (sizeBytesReceived == 4) {
          imageSize = *(uint32_t*)sizeBytes;
          Serial.printf("[UART] Expecting %u bytes\n", imageSize);

          // Sanity check — QVGA JPEG should be under 100KB
          if (imageSize == 0 || imageSize > 100000) {
            Serial.printf("[UART] Bad size (%u) — discarding, back to WAIT_START\n", imageSize);
            rxState = WAIT_START;
            break;
          }

          if (imageBuffer) { free(imageBuffer); imageBuffer = NULL; }

          // Try PSRAM first, fall back to regular RAM
          imageBuffer = (uint8_t*)ps_malloc(imageSize);
          if (!imageBuffer) imageBuffer = (uint8_t*)malloc(imageSize);

          if (!imageBuffer) {
            Serial.println("[UART] Failed to allocate image buffer!");
            rxState = WAIT_START;
            break;
          }

          Serial.printf("[UART] Buffer allocated (%u bytes)\n", imageSize);
          bytesReceived = 0;
          rxState = WAIT_DATA;
        }
        break;

      case WAIT_DATA:
        imageBuffer[bytesReceived++] = b;
        // Progress log every 25%
        if (bytesReceived == imageSize / 4)  Serial.println("[UART] 25%...");
        if (bytesReceived == imageSize / 2)  Serial.println("[UART] 50%...");
        if (bytesReceived == imageSize * 3 / 4) Serial.println("[UART] 75%...");
        if (bytesReceived >= imageSize) {
          Serial.printf("[UART] Image received (%u bytes)\n", imageSize);
          Serial.printf("[UART] First bytes: %02X %02X %02X %02X (valid JPEG starts with FF D8 FF)\n",
            imageBuffer[0], imageBuffer[1], imageBuffer[2], imageBuffer[3]);

          // Save a copy for the web server
          if (lastImage) free(lastImage);
          lastImage = (uint8_t*)malloc(imageSize);
          if (lastImage) {
            memcpy(lastImage, imageBuffer, imageSize);
            lastImageSize = imageSize;
          }

          Serial.println("[UART] Launching API call...");
          apiDone   = false;
          apiResult = -1;

          // Launch API call on Core 0 (LEDs run on Core 1)
          xTaskCreatePinnedToCore(
            apiCallTask, "api_call", 32768, NULL, 1, NULL, 0
          );

          rxState = WAIT_START;
        }
        break;
    }
  }
}

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------
void connectWiFi() {
  Serial.printf("Connecting to %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nConnected — IP: %s\n", WiFi.localIP().toString().c_str());
}

// ---------------------------------------------------------------------------
// Web server routes
// ---------------------------------------------------------------------------
void handleRoot() {
  String html = "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>LED Art Debug</title>"
    "<style>body{font-family:monospace;background:#111;color:#eee;padding:20px;max-width:800px;margin:0 auto}"
    "img{max-width:100%;border:2px solid #444;margin:10px 0}"
    ".winner{font-size:2em;padding:10px;border-radius:8px;display:inline-block;margin:10px 0}"
    "pre{background:#222;padding:12px;overflow-x:auto;border-radius:4px}"
    "a{color:#4af}</style></head><body>"
    "<h1>LED Art Installation — Debug</h1>"
    "<h2>Last Captured Image</h2>";

  if (lastImageSize > 0) {
    html += "<img src='/image'><br>";
    html += "<p>" + String(lastImageSize) + " bytes</p>";
  } else {
    html += "<p>No image captured yet</p>";
  }

  html += "<h2>Claude Response</h2>";
  if (lastWinner >= 0) {
    html += "<div class='winner'>Type " + String(lastWinner + 1) + " — Strip " + String(lastWinner) + "</div><br>";
  }
  html += "<pre>" + lastApiResponse + "</pre>";
  html += "<br><p><a href='/'>Refresh</a></p>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleImage() {
  if (lastImage && lastImageSize > 0) {
    server.send_P(200, "image/jpeg", (const char*)lastImage, lastImageSize);
  } else {
    server.send(404, "text/plain", "No image yet");
  }
}

void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/image", handleImage);
  server.begin();
  Serial.printf("Web server at http://%s/\n", WiFi.localIP().toString().c_str());
}

// ---------------------------------------------------------------------------
// SETUP
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(3000);  // USB CDC needs time to connect
  Serial.println("\n\n=== Lolin S3 Startup ===");

  Serial.printf("UART to CAM: TX=%d, RX=%d, baud=%d\n", CAM_UART_TX, CAM_UART_RX, UART_BAUD);
  Serial1.setRxBufferSize(16384);  // 16KB buffer to handle full image transfer
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

  Serial.printf("Connecting to WiFi: %s\n", WIFI_SSID);
  connectWiFi();
  setupWebServer();
  randomSeed(analogRead(1));

  Serial.println("=== Lolin S3 ready — waiting for photo from ESP32-CAM ===\n");
}

// ---------------------------------------------------------------------------
// MAIN LOOP (runs on Core 1)
// ---------------------------------------------------------------------------
void loop() {
  server.handleClient();
  receiveImage();

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
      // Transition once Claude responds (or timeout after 20s)
      if (apiDone && apiResult >= 0) {
        winnerPath = apiResult;
        enterState(DECIDING);
      } else if (elapsed > 20000) {
        Serial.println("[WARN] API timeout — falling back to random");
        winnerPath = random(NUM_STRIPS);
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
        Serial1.write(MSG_READY);
        Serial.println("Sent READY to ESP32-CAM");
        enterState(IDLE);
      }
      break;
  }

  FastLED.show();
  delay(20);
}
