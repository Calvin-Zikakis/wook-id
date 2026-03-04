#include <Arduino.h>
#include <FastLED.h>

#define LED_PIN          18
#define BUTTON_PIN        4   // GPIO 4 = Touch pin T0 — connect a wire or foil pad
#define TOUCH_THRESHOLD  20   // Lower = needs more pressure; raise if too sensitive
#define NUM_LEDS        199
#define LED_TYPE        WS2812
#define COLOR_ORDER     GRB
#define NUM_PATHS         6

CRGB leds[NUM_LEDS];

// Each path is a contiguous segment of the strip that physically routes
// out to a sign and back to the center, repeated 6 times.
// Within each segment: first half = LEDs going OUT, second half = LEDs coming BACK.
const int PATH_START[NUM_PATHS] = {  0,  33,  66,  99, 132, 165 };
const int PATH_END[NUM_PATHS]   = { 32,  65,  98, 131, 164, 198 };

// One distinct color per "type" of person
const CRGB PATH_COLORS[NUM_PATHS] = {
  CRGB(255,  30,   0),   // Fiery red
  CRGB(  0,  80, 255),   // Electric blue
  CRGB(  0, 210,  50),   // Vivid green
  CRGB(180,   0, 255),   // Deep purple
  CRGB(255, 140,   0),   // Amber
  CRGB(  0, 220, 220),   // Cyan
};

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------
enum State { IDLE, BUILD, PATHS_COMPETE, DECIDING, REVEAL };
State state = IDLE;

int   winnerPath    = 0;
int   effectVariant = 0;
unsigned long stateStart = 0;

float pathIntensity[NUM_PATHS];

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
void enterState(State s) {
  state      = s;
  stateStart = millis();
}

void startSequence() {
  winnerPath    = random(NUM_PATHS);
  effectVariant = random(3);
  for (int p = 0; p < NUM_PATHS; p++) pathIntensity[p] = 0.5f;
  enterState(BUILD);
}

void fillPath(int p, CRGB color, uint8_t bright) {
  CRGB c = color;
  c.nscale8(bright);
  for (int i = PATH_START[p]; i <= PATH_END[p]; i++) leds[i] = c;
}

// Triangle wave: returns 0.0→1.0→0.0 over one cycle
float triangleWave(unsigned long elapsed, float cycleDurationMs) {
  float phase = fmod((float)elapsed, cycleDurationMs) / cycleDurationMs;
  return (phase < 0.5f) ? phase * 2.0f : (1.0f - phase) * 2.0f;
}

// ---------------------------------------------------------------------------
// IDLE — completely dark, waiting for touch
// ---------------------------------------------------------------------------
void updateIdle() {
  FastLED.clear();
}

// ---------------------------------------------------------------------------
// BUILD — all paths fill in from center and retract, building excitement.
// Runs for 6 seconds, getting faster and brighter each cycle.
// ---------------------------------------------------------------------------
void updateBuild(unsigned long elapsed) {
  float progress = constrain(elapsed / 6000.0f, 0.0f, 1.0f);

  // Cycle duration shrinks from 1800ms to 600ms as excitement builds
  float cycleDurationMs = 1800.0f - progress * 1200.0f;

  float fillFraction = triangleWave(elapsed, cycleDurationMs);

  // Brightness ramps from dim to full
  uint8_t brightness = (uint8_t)(50 + progress * 205);

  FastLED.clear();
  for (int p = 0; p < NUM_PATHS; p++) {
    int len       = PATH_END[p] - PATH_START[p] + 1;
    int fillCount = (int)(fillFraction * len);
    CRGB c        = PATH_COLORS[p];
    c.nscale8(brightness);
    for (int i = PATH_START[p]; i < PATH_START[p] + fillCount; i++) {
      leds[i] = c;
    }
  }
}

// ---------------------------------------------------------------------------
// PATHS_COMPETE — all 6 paths swell and battle organically
// ---------------------------------------------------------------------------
void updatePathsCompete(unsigned long elapsed) {
  for (int p = 0; p < NUM_PATHS; p++) {
    float wave   = sinf(elapsed * 0.002f * (0.8f + p * 0.15f) + p * 0.9f);
    float target = 0.55f + 0.40f * wave + (random8() - 128) / 600.0f;
    target = constrain(target, 0.05f, 1.0f);
    pathIntensity[p] += (target - pathIntensity[p]) * 0.12f;

    uint8_t bright = (uint8_t)(pathIntensity[p] * 255.0f);

    for (int i = PATH_START[p]; i <= PATH_END[p]; i++) {
      CRGB c = PATH_COLORS[p];
      uint8_t pixel_bright = (effectVariant == 1)
        ? scale8(bright, random8(170, 255))
        : bright;
      c.nscale8(pixel_bright);
      leds[i] = c;
    }
  }
}

// ---------------------------------------------------------------------------
// DECIDING — winner rises, losers fade over 4 seconds
// ---------------------------------------------------------------------------
void updateDeciding(unsigned long elapsed) {
  float t     = constrain(elapsed / 4000.0f, 0.0f, 1.0f);
  float eased = t * t * (3.0f - 2.0f * t);

  for (int p = 0; p < NUM_PATHS; p++) {
    float intensity;
    if (p == winnerPath) {
      intensity = pathIntensity[p] + (1.0f - pathIntensity[p]) * eased;
      if (random8() > 230) intensity *= 0.75f + 0.25f * (random8() / 255.0f);
    } else {
      intensity = pathIntensity[p] * (1.0f - eased * eased);
    }
    fillPath(p, PATH_COLORS[p], (uint8_t)(constrain(intensity, 0.0f, 1.0f) * 255));
  }
}

// ---------------------------------------------------------------------------
// REVEAL — winner pulses with a comet travelling out-and-back for 10 seconds
// ---------------------------------------------------------------------------
void updateReveal(unsigned long elapsed) {
  FastLED.clear();

  int start = PATH_START[winnerPath];
  int end   = PATH_END[winnerPath];
  int half  = (end - start + 1) / 2;

  // Slow triumphant pulse on the whole path
  uint8_t angle  = (uint8_t)(elapsed / 4);
  uint8_t bright = scale8(sin8(angle), 60) + 120;
  fillPath(winnerPath, PATH_COLORS[winnerPath], bright);

  // Comet travels out to the sign tip, then back to center
  int cometPeriod = half * 2;
  int tick        = (elapsed / 30) % cometPeriod;
  int cometPos    = (tick < half) ? start + tick : end - (tick - half);
  leds[cometPos]  = CRGB::White;
  if (cometPos - 1 >= start) leds[cometPos - 1].nscale8(180);
}

// ---------------------------------------------------------------------------
// Touch detection — triggers on the moment of contact, won't re-fire until
// finger is lifted and touches again.
// ---------------------------------------------------------------------------
void checkButton() {
  uint16_t raw     = touchRead(BUTTON_PIN);
  bool     touched = raw < TOUCH_THRESHOLD;

  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 200) {
    Serial.print("touch raw: ");
    Serial.print(raw);
    Serial.print("  touched: ");
    Serial.println(touched ? "YES" : "no");
    lastPrint = millis();
  }

  // Fire once on the initial contact; reset only after finger lifts
  static bool wasUntouched = true;
  if (touched && wasUntouched && state == IDLE) {
    Serial.println(">>> Touch triggered — starting sequence");
    startSequence();
  }
  wasUntouched = !touched;
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(200);
  randomSeed(analogRead(0));
}

void loop() {
  checkButton();
  unsigned long elapsed = millis() - stateStart;

  switch (state) {
    case IDLE:
      updateIdle();
      break;

    case BUILD:
      updateBuild(elapsed);
      if (elapsed > 6000) enterState(PATHS_COMPETE);
      break;

    case PATHS_COMPETE:
      updatePathsCompete(elapsed);
      if (elapsed > 4000) enterState(DECIDING);
      break;

    case DECIDING:
      updateDeciding(elapsed);
      if (elapsed > 4000) enterState(REVEAL);
      break;

    case REVEAL:
      updateReveal(elapsed);
      if (elapsed > 10000) enterState(IDLE);
      break;
  }

  FastLED.show();
  delay(20);
}
