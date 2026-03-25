# Wook or Woke

An art installation that photographs crystals and rates them on a wook-to-woke spectrum using Claude's vision API, running entirely on an ESP32-CAM.

## Hardware

- **ESP32-CAM** (AI Thinker) with OV2640 camera
- **Physical button** on GPIO 12 (other leg to GND, uses internal pull-up)
- **LED controller** (optional second ESP32) on Serial2: GPIO 14 (TX), GPIO 15 (RX)

### Wiring

```
ESP32-CAM          Button
---------          ------
GPIO 12  --------  one leg
GND      --------  other leg

ESP32-CAM          LED Controller ESP32
---------          --------------------
GPIO 14 (TX) ----  RX
GPIO 15 (RX) ----  TX
GND          ----  GND
```

## Setup

### Prerequisites

- [PlatformIO CLI](https://docs.platformio.org/en/latest/core/installation.html)
- Anthropic API key with credits

### Configuration

Edit `platformio.ini` build flags with your credentials:

```ini
build_flags =
    -DWIFI_SSID='"Your Network"'
    -DWIFI_PASS='"your_password"'
    -DAPI_KEY='"sk-ant-..."'
```

### Upload

The ESP32-CAM requires GPIO 0 connected to GND during upload (flash mode). Press reset after connecting GPIO 0 to GND, then:

```bash
pio run -e esp32cam -t upload
```

Disconnect GPIO 0 from GND and reset the board after flashing.

The `kill_monitor.py` pre-upload script automatically kills any running serial monitor so the port is free for flashing.

### Serial Monitor

```bash
pio device monitor
```

Baud rate: 115200

## Usage

### Web Interface

Connect to the same WiFi network and open the IP address printed to serial (e.g. `http://192.168.1.x`). The web UI provides:

- **Capture** - take a still photo with flash
- **Judge Crystal** - photograph and send to Claude for rating
- **Stream** - live MJPEG stream from camera

The UI is responsive: side-by-side layout on wide screens (camera left, results right), stacked on mobile.

### Physical Button

Press the button on GPIO 12 to trigger a crystal judgment. The web UI polls for button-triggered results and displays them automatically.

### Scoring

Crystals are rated 0-7:

| Score | Label | Vibe |
|-------|-------|------|
| 0 | NOT A CRYSTAL | Not a crystal at all |
| 1 | DEEP WOOK | Raw, earthy, chaotic, festival energy |
| 2 | WOOK | Warm, cloudy, irregular, rough |
| 3 | WOOKISH | Leaning wook |
| 4 | BALANCED | Equal parts wook and woke |
| 5 | WOKE-ISH | Leaning woke |
| 6 | WOKE | Cool, clear, geometric, polished |
| 7 | PEAK WOKE | Polished, geometric, precise, museum-ready |

Criteria: color (warm=wook, cool=woke), clarity (cloudy=wook, clear=woke), shape (irregular=wook, geometric=woke), surface (rough=wook, polished=woke).

## LED Controller Serial Protocol

The ESP32-CAM communicates with an optional LED controller ESP32 over Serial2 (9600 baud). The connection is auto-detected via PING/PONG handshake on each button press -- if no LED controller is connected, the button works standalone.

### Messages

| Direction | Message | Meaning |
|-----------|---------|---------|
| CAM -> LED | `PING\n` | Are you there? (sent each button press) |
| LED -> CAM | `PONG\n` | Yes, I'm here |
| CAM -> LED | `START\n` | Button pressed, begin spectacle animation |
| CAM -> LED | `SCORE:X\n` | Claude's score (0-7), transition to final state |
| LED -> CAM | `DONE\n` | Animation complete, re-enable button |

### Flow

1. Button pressed -> CAM sends `PING`, waits 100ms for `PONG`
2. If LED controller responds: CAM sends `START`, LED begins animation
3. CAM takes photo with flash, sends to Claude API
4. CAM receives score, sends `SCORE:X` to LED controller
5. LED controller finishes animation, sends `DONE`
6. Button is re-enabled

### Safety

- Button is locked out during the full cycle (photo + API + LED animation)
- If Claude API call fails, button unlocks immediately (sends `SCORE:0`)
- If camera capture fails, button unlocks immediately
- 60-second timeout unlocks the button if `DONE` is never received
- If no LED controller is detected, no lockout waiting occurs

## API

Uses Claude Haiku (`claude-haiku-4-5-20251001`) for cheap vision analysis. Images are captured at VGA (640x480), base64 encoded, and streamed to the API in chunks to avoid memory corruption on the ESP32.

## Endpoints

| Path | Method | Description |
|------|--------|-------------|
| `/` | GET | Web UI |
| `/capture` | GET | JPEG still with flash |
| `/judge` | GET | Capture + Claude analysis, returns JSON |
| `/stream` | GET | MJPEG live stream |
| `/button-result` | GET | Poll for physical button results |
