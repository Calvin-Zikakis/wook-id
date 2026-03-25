# Wook or Woke

An art installation that photographs crystals and people, then rates them on a wook-to-woke spectrum using Claude's vision API. Runs on two ESP32 boards: one for the camera/brain, one for LED animations.

## Hardware

### Board 1: ESP32-CAM (camera + brain)

- **AI Thinker ESP32-CAM** — captures photos, calls Claude API, serves web UI
- **Physical button** on GPIO 12 (other leg to GND, uses internal pull-up)
- **Flash LED** on GPIO 4 (PWM-controlled brightness)
- **UART to LED board:** GPIO 14 (TX), GPIO 15 (RX)

### Board 2: ESP32 LED Controller (Lolin S3)

- **8x WS2812 LED strips**, 99 LEDs each (792 LEDs total)
- Strip pins: GPIO 13, 12, 14, 27, 26, 25, 33, 32
- **UART from CAM board:** GPIO 16 (RX), GPIO 17 (TX)
- Each strip represents a "personality type" with a distinct color (Adventurer, Thinker, Creator, Leader, Dreamer, Guardian, Explorer, Mystic)

### Wiring

```
ESP32-CAM          Button
---------          ------
GPIO 12  --------  one leg
GND      --------  other leg

ESP32-CAM          LED Controller (Lolin S3)
---------          -------------------------
GPIO 14 (TX) ----  GPIO 16 (RX)
GPIO 15 (RX) ----  GPIO 17 (TX)
GND          ----  GND
```

## Setup

### Prerequisites

- [PlatformIO CLI](https://docs.platformio.org/en/latest/core/installation.html)
- Anthropic API key with credits

### Configuration

Copy the example config and fill in your credentials:

```bash
cp src/config.example.h src/config.h
```

Then edit `src/config.h` with your WiFi credentials and Anthropic API key.

### Upload

#### ESP32-CAM (serial)

The ESP32-CAM requires GPIO 0 connected to GND during upload (flash mode). Press reset after connecting GPIO 0 to GND, then:

```bash
pio run -e esp32cam -t upload
```

Disconnect GPIO 0 from GND and reset the board after flashing.

#### LED Controller (serial)

```bash
pio run -e esp32leds -t upload
```

#### OTA (over-the-air)

Both boards support OTA updates once connected to WiFi:

```bash
pio run -e esp32cam_ota -t upload    # cam board via wook-or-woke-cam.local
pio run -e esp32leds_ota -t upload   # LED board via wook-or-woke-leds.local
```

Both boards also serve a web-based OTA upload page at `/update`.

### Serial Monitor

```bash
pio device monitor                # whichever board is connected
```

Baud rate: 115200

## Usage

### Web Interface (ESP32-CAM)

Connect to the same WiFi network and open the IP address printed to serial (e.g. `http://192.168.1.x`). The web UI provides:

- **Capture** — take a still photo with flash
- **Judge** — photograph and send to Claude for rating
- **Stream** — live MJPEG stream from camera
- **LED toggle** — manually turn flash on/off
- **Brightness slider** — adjust flash LED brightness
- **Serial log** — live serial output in the browser

The UI is responsive: side-by-side layout on wide screens (camera left, results right), stacked on mobile.

### Physical Button

Press the button on GPIO 12 to trigger a judgment. The web UI polls for button-triggered results and displays them automatically.

### Scoring

Crystals and people are rated 0–7:

| Score | Label         | Vibe                                       |
| ----- | ------------- | ------------------------------------------ |
| 0     | NOT A CRYSTAL | Not a crystal or person at all             |
| 1     | LEVEL 3 WOOK  | Raw, earthy, chaotic, festival energy      |
| 2     | LEVEL 2 WOOK  | Warm, cloudy, irregular, rough             |
| 3     | LEVEL 1 WOOK  | Leaning wook                               |
| 4     | NORMIE        | Dead center, neither wook nor woke         |
| 5     | LEVEL 1 WOKE  | Leaning woke                               |
| 6     | LEVEL 2 WOKE  | Cool, clear, geometric, polished           |
| 7     | LEVEL 3 WOKE  | Polished, geometric, precise, museum-ready |

**For crystals:** color (warm=wook, cool=woke), clarity (cloudy=wook, clear=woke), shape (irregular=wook, geometric=woke), surface (rough=wook, polished=woke).

**For humans:** vibe, outfit, hair, jewelry, accessories, energy (tie-dye/dreads/crystals=wook, minimalist/clean-cut/techwear=woke).

## LED Animation Sequence

When a judgment is triggered, the LED controller runs through these states:

| State    | Duration | Effect                                                  |
| -------- | -------- | ------------------------------------------------------- |
| IDLE     | —        | All dark                                                |
| BUILD    | 6s       | All 8 strips fill from both ends toward center, accelerating |
| COMPETE  | varies   | All strips swell organically with sine waves (waits for score) |
| DECIDING | 4s       | Winning strip brightens with sparkles, others fade out  |
| REVEAL   | 10s      | Winner breathes with white sparkles                     |

The winning strip (1–7) maps to the Claude score. Score 0 aborts the animation.

## Serial Protocol (CAM ↔ LED)

UART at 9600 baud. The connection is auto-detected via PING/PONG handshake on each button press — if no LED controller is connected, the button works standalone.

### Messages

| Direction  | Message       | Meaning                                         |
| ---------- | ------------- | ----------------------------------------------- |
| CAM → LED | `PING\n`    | Are you there? (sent each button press)         |
| LED → CAM | `PONG\n`    | Yes, I'm here                                   |
| CAM → LED | `START\n`   | Button pressed, begin BUILD animation            |
| CAM → LED | `SCORE:X\n` | Claude's score (1–7), transition to DECIDING     |
| LED → CAM | `DONE\n`    | Animation complete, re-enable button             |

### Flow

1. Button pressed → CAM sends `PING`, waits up to 1.5s (3 attempts × 500ms) for `PONG`
2. If LED controller responds: CAM sends `START`, LED begins BUILD animation
3. CAM takes photo with flash, sends to Claude API
4. CAM receives score, sends `SCORE:X` to LED controller
5. LED enters COMPETE → DECIDING → REVEAL sequence
6. LED controller finishes REVEAL, sends `DONE`
7. Button is re-enabled

### Safety

- Button is locked out during the full cycle (photo + API + LED animation)
- If Claude API call fails, button unlocks immediately (sends `SCORE:0`)
- If camera capture fails, button unlocks immediately
- 60-second timeout unlocks the button if `DONE` is never received
- 30-second timeout on LED side if no score arrives (picks random winner)
- If no LED controller is detected, no lockout waiting occurs

## API

Uses Claude Haiku (`claude-haiku-4-5-20251001`) for cheap vision analysis. Images are captured at VGA (640×480), base64 encoded, and streamed to the API in chunks to avoid memory corruption on the ESP32.

## Endpoints (ESP32-CAM)

| Path               | Method | Description                             |
| ------------------ | ------ | --------------------------------------- |
| `/`              | GET    | Web UI                                  |
| `/capture`       | GET    | JPEG still with flash                   |
| `/photo`         | GET    | Last captured photo (no new capture)    |
| `/judge`         | GET    | Capture + Claude analysis, returns JSON |
| `/stream`        | GET    | MJPEG live stream                       |
| `/led`           | GET    | Toggle flash LED on/off                 |
| `/brightness`    | GET    | Set flash brightness (?value=0-255)     |
| `/log`           | GET    | Serial log stream (SSE)                 |
| `/update`        | GET/POST | Web OTA firmware upload               |
| `/button-result` | GET    | Poll for physical button results        |

## Endpoints (LED Controller)

| Path       | Method   | Description             |
| ---------- | -------- | ----------------------- |
| `/update`  | GET/POST | Web OTA firmware upload |
