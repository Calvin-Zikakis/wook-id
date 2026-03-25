#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <base64.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include "esp_camera.h"
#include "config.h"

// AI Thinker ESP32-CAM pin definitions
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

#define LED_FLASH 4
#define LED_PWM_CHANNEL 2  // channel 0 used by camera
#define LED_PWM_FREQ 5000
#define LED_PWM_RESOLUTION 8  // 0-255
#define BUTTON_PIN 12
#define LED_SERIAL_TX 14
#define LED_SERIAL_RX 15

WebServer server(80);

// ---------------------------------------------------------------------------
// Ring buffer for web serial console
// ---------------------------------------------------------------------------
#define LOG_BUF_SIZE 4096
char logBuf[LOG_BUF_SIZE];
volatile int logHead = 0;
volatile int logCount = 0;  // total chars ever written (cursor for clients)

void logStore(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        logBuf[logHead] = data[i];
        logHead = (logHead + 1) % LOG_BUF_SIZE;
    }
    logCount += len;
}

class TeeSerial : public Print {
public:
    size_t write(uint8_t c) override {
        Serial.write(c);
        logStore(&c, 1);
        return 1;
    }
    size_t write(const uint8_t *buf, size_t size) override {
        Serial.write(buf, size);
        logStore(buf, size);
        return size;
    }
};
TeeSerial Log;

volatile bool buttonPressed = false;
unsigned long lastButtonPress = 0;
String lastButtonResult = "";
bool buttonJudging = false;
bool waitingForLedDone = false;  // true while LED controller is animating
bool ledManualOn = false;  // true when LED is manually toggled on via web UI
int ledBrightness = 255;  // 0-255 PWM brightness for flash
uint8_t *lastPhoto = NULL;
size_t lastPhotoLen = 0;

// Take a photo with flash and store it
camera_fb_t* flashCapture() {
    if (!ledManualOn) ledcWrite(LED_PWM_CHANNEL, ledBrightness);
    delay(500);
    for (int i = 0; i < 5; i++) {
        camera_fb_t *discard = esp_camera_fb_get();
        if (discard) esp_camera_fb_return(discard);
        delay(100);
    }
    camera_fb_t *fb = esp_camera_fb_get();
    if (!ledManualOn) ledcWrite(LED_PWM_CHANNEL, 0);

    // Store a copy for the /photo endpoint
    if (fb) {
        if (lastPhoto) free(lastPhoto);
        lastPhoto = (uint8_t *)malloc(fb->len);
        if (lastPhoto) {
            memcpy(lastPhoto, fb->buf, fb->len);
            lastPhotoLen = fb->len;
        }
    }
    return fb;
}
unsigned long ledDoneTimeout = 0;
bool ledControllerPresent = false;
#define LED_DONE_TIMEOUT_MS 60000  // 60s safety timeout

bool checkLedController() {
    for (int attempt = 0; attempt < 3; attempt++) {
        while (Serial2.available()) Serial2.read();  // flush stale data
        Serial2.println("PING");
        Log.printf("PING attempt %d...\n", attempt + 1);
        unsigned long start = millis();
        while (millis() - start < 500) {  // 500ms timeout per attempt
            if (Serial2.available()) {
                String msg = Serial2.readStringUntil('\n');
                msg.trim();
                if (msg == "PONG") {
                    Log.println("PONG received!");
                    return true;
                }
                Log.printf("Got unexpected response: '%s'\n", msg.c_str());
            }
        }
        Log.println("No PONG - retrying...");
    }
    return false;
}

void IRAM_ATTR buttonISR() {
    unsigned long now = millis();
    if (now - lastButtonPress > 500) { // debounce 500ms
        buttonPressed = true;
        lastButtonPress = now;
    }
}

static const char *CLAUDE_PROMPT =
    "You are the analyst for \"Wook or Woke,\" an art installation. "
    "You will see either a crystal or a human. Rate them 1-7 on the wook-to-woke spectrum: "
    "1=max wook (raw, earthy, chaotic, festival energy), 7=max woke (polished, geometric, precise, museum-ready). "
    "For crystals: color (warm=wook, cool=woke), clarity (cloudy=wook, clear=woke), "
    "shape (irregular=wook, geometric=woke), surface (rough=wook, polished=woke). "
    "For humans: vibe, outfit, hair, jewelry, accessories, energy (tie-dye/dreads/crystals=wook, "
    "minimalist/clean-cut/techwear=woke). "
    "If the image is neither a crystal nor a human, score 0. "
    "Respond ONLY with raw JSON, no markdown, no code blocks: "
    "{\"score\":<0-7>,\"subject\":\"crystal\" or \"human\" or \"unknown\",\"description\":\"<playful, max 80 chars>\"}";

void initCamera() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sscb_sda = SIOD_GPIO_NUM;
    config.pin_sscb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = FRAMESIZE_VGA;  // 640x480
    config.jpeg_quality = 12;
    config.fb_count = 1;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Log.printf("Camera init failed with error 0x%x\n", err);
        return;
    }
    Log.println("Camera initialized");
}

// ---------------------------------------------------------------------------
// Upload result + photo to logging server
// ---------------------------------------------------------------------------
void uploadResult(int score, const String &description) {
    if (!lastPhoto || lastPhotoLen == 0) {
        Log.println("No photo to upload");
        return;
    }

    WiFiClientSecure client;
    client.setInsecure();  // skip cert verification (same as Claude API calls)
    if (!client.connect(LOG_SERVER_HOST, LOG_SERVER_PORT)) {
        Log.println("Upload: failed to connect to log server");
        return;
    }

    String boundary = "----WookBoundary";

    // Build the multipart fields (score + description)
    String fields;
    fields += "--" + boundary + "\r\n";
    fields += "Content-Disposition: form-data; name=\"wokeScore\"\r\n\r\n";
    fields += String(score) + "\r\n";
    fields += "--" + boundary + "\r\n";
    fields += "Content-Disposition: form-data; name=\"description\"\r\n\r\n";
    fields += description + "\r\n";

    // Photo part header + footer
    String photoHeader = "--" + boundary + "\r\n";
    photoHeader += "Content-Disposition: form-data; name=\"photo\"; filename=\"crystal.jpg\"\r\n";
    photoHeader += "Content-Type: image/jpeg\r\n\r\n";
    String footer = "\r\n--" + boundary + "--\r\n";

    int contentLength = fields.length() + photoHeader.length() + lastPhotoLen + footer.length();

    client.printf("POST /api/upload HTTP/1.1\r\n");
    client.printf("Host: %s\r\n", LOG_SERVER_HOST);
    client.printf("X-API-Key: %s\r\n", LOG_API_KEY);
    client.printf("Content-Type: multipart/form-data; boundary=%s\r\n", boundary.c_str());
    client.printf("Content-Length: %d\r\n", contentLength);
    client.println("Connection: close\r\n");

    client.print(fields);
    client.print(photoHeader);

    // Send photo in chunks to avoid memory issues
    const size_t chunkSize = 2048;
    for (size_t i = 0; i < lastPhotoLen; i += chunkSize) {
        size_t len = min(chunkSize, lastPhotoLen - i);
        client.write(lastPhoto + i, len);
    }

    client.print(footer);

    // Read response
    unsigned long start = millis();
    while (client.connected() && millis() - start < 5000) {
        if (client.available()) {
            String line = client.readStringUntil('\n');
            Log.println("Upload response: " + line);
            break;
        }
        delay(10);
    }
    client.stop();
    Log.println("Upload complete");
}

String callClaude(camera_fb_t *fb) {
    Log.println("Encoding image...");
    String imageBase64 = base64::encode(fb->buf, fb->len);
    Log.printf("Base64 size: %d bytes\n", imageBase64.length());

    // Build JSON prefix and suffix separately, sandwich base64 in between
    // This avoids one giant string that can corrupt in memory
    String prefix = "{\"model\":\"claude-haiku-4-5-20251001\",\"max_tokens\":150,\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"image\",\"source\":{\"type\":\"base64\",\"media_type\":\"image/jpeg\",\"data\":\"";

    String suffix = "\"}},{\"type\":\"text\",\"text\":\"";
    // Escape the prompt for JSON (quotes)
    String prompt = String(CLAUDE_PROMPT);
    prompt.replace("\"", "\\\"");
    suffix += prompt;
    suffix += "\"}]}]}";

    size_t totalLen = prefix.length() + imageBase64.length() + suffix.length();
    Log.printf("Payload size: %d bytes\n", totalLen);
    Log.println("Connecting to Claude API...");

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(30);

    if (!client.connect("api.anthropic.com", 443)) {
        Log.println("Connection to API failed!");
        return "{\"error\":\"Connection failed\"}";
    }

    // Send HTTP headers
    client.println("POST /v1/messages HTTP/1.1");
    client.println("Host: api.anthropic.com");
    client.printf("x-api-key: %s\r\n", API_KEY);
    client.println("anthropic-version: 2023-06-01");
    client.println("content-type: application/json");
    client.printf("content-length: %d\r\n", totalLen);
    client.println("Connection: close");
    client.println();

    // Stream body in three parts: prefix, base64, suffix
    const size_t chunkSize = 2048;

    // Send prefix
    client.write((const uint8_t *)prefix.c_str(), prefix.length());
    prefix = "";

    // Send base64 in chunks
    for (size_t i = 0; i < imageBase64.length(); i += chunkSize) {
        size_t len = min(chunkSize, imageBase64.length() - i);
        client.write((const uint8_t *)(imageBase64.c_str() + i), len);
        delay(1); // yield to prevent watchdog
    }
    imageBase64 = "";

    // Send suffix
    client.write((const uint8_t *)suffix.c_str(), suffix.length());
    suffix = "";

    Log.println("Waiting for response...");

    // Wait for response
    unsigned long start = millis();
    while (!client.available() && millis() - start < 30000) {
        delay(100);
    }

    // Read response
    String response = "";
    bool bodyStarted = false;
    while (client.available() || client.connected()) {
        String line = client.readStringUntil('\n');
        if (!bodyStarted) {
            if (line == "\r") bodyStarted = true;
            continue;
        }
        response += line;
    }
    client.stop();

    Log.println("Raw API response:");
    Log.println(response);

    // Extract text content directly with string search
    // (avoids full JSON parse of potentially corrupted outer response)
    int textIdx = response.indexOf("\"text\":\"");
    if (textIdx < 0) {
        // Check for error
        int errIdx = response.indexOf("\"message\":\"");
        if (errIdx >= 0) {
            errIdx += 11;
            int errEnd = response.indexOf("\"", errIdx);
            String errMsg = response.substring(errIdx, errEnd);
            Log.printf("API error: %s\n", errMsg.c_str());
            return "{\"error\":\"" + errMsg + "\"}";
        }
        return "{\"error\":\"Could not parse response\"}";
    }
    textIdx += 8;

    // Find closing quote for the text value
    int endIdx = textIdx;
    while (endIdx < (int)response.length()) {
        if (response[endIdx] == '"' && response[endIdx - 1] != '\\') break;
        endIdx++;
    }

    String text = response.substring(textIdx, endIdx);
    text.replace("\\\"", "\"");
    text.replace("\\n", "");

    // Strip markdown code blocks if present
    if (text.startsWith("```json")) text = text.substring(7);
    else if (text.startsWith("```")) text = text.substring(3);
    if (text.endsWith("```")) text = text.substring(0, text.length() - 3);
    text.trim();

    Log.printf("Claude says: %s\n", text.c_str());
    return text;
}

void handleRoot() {
    String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Wook or Woke</title>
    <style>
        @import url('https://fonts.googleapis.com/css2?family=Space+Mono:wght@400;700&display=swap');
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body { font-family: 'Space Mono', monospace; text-align: center;
               background: #0a0a0f; color: #e0d4f5; min-height: 100vh;
               overflow-x: hidden; }

        /* Animated background - sacred geometry vibes */
        body::before { content: ''; position: fixed; top: 0; left: 0; width: 100%; height: 100%;
            background:
                radial-gradient(circle at 20% 50%, rgba(139,69,255,0.08) 0%, transparent 50%),
                radial-gradient(circle at 80% 50%, rgba(255,107,53,0.08) 0%, transparent 50%),
                radial-gradient(circle at 50% 20%, rgba(78,205,196,0.05) 0%, transparent 50%);
            z-index: -1; animation: breathe 8s ease-in-out infinite; }
        @keyframes breathe { 0%,100% { opacity: 0.6; } 50% { opacity: 1; } }

        .container { max-width: 500px; margin: 0 auto; padding: 20px; }

        /* Wide screen: side-by-side layout */
        @media (min-width: 800px) {
            .container { max-width: 960px; }
            .main-layout { display: flex; gap: 30px; align-items: flex-start; }
            .left-col { flex: 1; min-width: 0; }
            .right-col { width: 320px; flex-shrink: 0; position: sticky; top: 20px; }
            #result { margin: 0; }
            #spinner { margin: 10px 0; }
            #score-text { font-size: 2.2em; }
        }

        /* Title */
        h1 { font-size: 2.2em; margin: 20px 0 5px; letter-spacing: 4px;
             background: linear-gradient(135deg, #ff6b35, #e84393, #8b45ff, #4ecdc4);
             -webkit-background-clip: text; -webkit-text-fill-color: transparent;
             background-size: 300% 300%; animation: shimmer 4s ease infinite; }
        @keyframes shimmer { 0%{background-position:0% 50%} 50%{background-position:100% 50%} 100%{background-position:0% 50%} }
        .subtitle { font-size: 0.7em; color: #666; letter-spacing: 6px; margin-bottom: 20px; text-transform: uppercase; }

        /* Camera feed */
        .cam-frame { position: relative; border-radius: 12px; overflow: hidden;
                     border: 1px solid rgba(139,69,255,0.3); margin: 15px 0;
                     box-shadow: 0 0 30px rgba(139,69,255,0.1), inset 0 0 30px rgba(0,0,0,0.3); }
        .cam-frame img { width: 100%; display: block; }
        .cam-frame::after { content: ''; position: absolute; top: 0; left: 0; right: 0; bottom: 0;
            border-radius: 12px; pointer-events: none;
            background: linear-gradient(135deg, rgba(139,69,255,0.05), transparent, rgba(255,107,53,0.05)); }
        .cam-loading { position: absolute; top: 0; left: 0; right: 0; bottom: 0;
            background: rgba(10,10,15,0.85); display: none; align-items: center;
            justify-content: center; flex-direction: column; z-index: 2; }
        .cam-loading.active { display: flex; }
        .cam-loading .crystal-spin { width: 50px; height: 50px; margin-bottom: 12px;
            border: 2px solid rgba(139,69,255,0.2); border-top-color: #8b45ff;
            border-radius: 50%; animation: spin 1s linear infinite; }
        .cam-loading .spinner-text { color: #8b45ff; font-size: 11px; letter-spacing: 3px; }

        /* Buttons */
        .controls { display: flex; gap: 8px; justify-content: center; margin: 15px 0; flex-wrap: wrap; }
        button { font-family: 'Space Mono', monospace; padding: 10px 20px; font-size: 13px;
                 border: 1px solid rgba(255,255,255,0.15); border-radius: 50px; cursor: pointer;
                 color: #e0d4f5; letter-spacing: 2px; text-transform: uppercase;
                 transition: all 0.3s ease; position: relative; overflow: hidden; }
        .btn-judge { background: linear-gradient(135deg, rgba(139,69,255,0.3), rgba(232,67,147,0.3));
                     border-color: rgba(139,69,255,0.5); }
        .btn-judge:hover { background: linear-gradient(135deg, rgba(139,69,255,0.6), rgba(232,67,147,0.6));
                           box-shadow: 0 0 25px rgba(139,69,255,0.4); transform: scale(1.05); }
        .btn-cap { background: rgba(78,205,196,0.15); border-color: rgba(78,205,196,0.4); }
        .btn-cap:hover { background: rgba(78,205,196,0.3); box-shadow: 0 0 20px rgba(78,205,196,0.3); }

        /* Spinner */
        #spinner { display: none; margin: 20px; }
        .crystal-spin { width: 40px; height: 40px; margin: 0 auto 10px;
            border: 2px solid rgba(139,69,255,0.2); border-top-color: #8b45ff;
            border-radius: 50%; animation: spin 1s linear infinite; }
        @keyframes spin { to { transform: rotate(360deg); } }
        .spinner-text { color: #8b45ff; font-size: 12px; letter-spacing: 3px; }

        /* Results */
        #result { display: none; margin: 20px 0; padding: 25px;
                  background: rgba(255,255,255,0.03); border-radius: 16px;
                  border: 1px solid rgba(139,69,255,0.15);
                  backdrop-filter: blur(10px); }

        /* Score spectrum bar */
        .spectrum { position: relative; margin: 15px auto; max-width: 380px; }
        #score-bar { height: 6px; border-radius: 3px;
            background: linear-gradient(to right,
                #ff6b35, #e84393, #8b45ff, #6c5ce7, #4ecdc4, #45b7d1, #a0e4f1);
            position: relative; opacity: 0.8; }
        #score-marker { width: 16px; height: 16px; border-radius: 50%;
            background: #fff; position: absolute; top: -5px;
            transition: left 0.8s cubic-bezier(0.34, 1.56, 0.64, 1);
            box-shadow: 0 0 15px rgba(255,255,255,0.6), 0 0 30px rgba(139,69,255,0.4); }
        .labels { display: flex; justify-content: space-between; max-width: 380px;
                  margin: 8px auto 0; font-size: 10px; letter-spacing: 3px; color: #666; }

        /* Score display */
        #score-text { font-size: 3em; font-weight: 700; margin: 15px 0 5px;
            background: linear-gradient(135deg, #ff6b35, #e84393, #8b45ff, #4ecdc4);
            -webkit-background-clip: text; -webkit-text-fill-color: transparent; }
        #description { font-size: 0.85em; color: #9988aa; font-style: italic;
                       max-width: 350px; margin: 0 auto; line-height: 1.6; }

        /* Thumbnail reel */
        .reel { display: flex; gap: 8px; overflow-x: auto; padding: 10px 0; margin-top: 15px; }
        .reel::-webkit-scrollbar { height: 4px; }
        .reel::-webkit-scrollbar-thumb { background: rgba(139,69,255,0.3); border-radius: 2px; }
        .thumb { flex-shrink: 0; width: 80px; cursor: pointer; position: relative; border-radius: 8px;
                 overflow: hidden; border: 1px solid rgba(139,69,255,0.2);
                 transition: transform 0.2s, border-color 0.2s; }
        .thumb:hover { transform: scale(1.1); border-color: rgba(139,69,255,0.6); }
        .thumb img { width: 100%; display: block; }
        .thumb-score { position: absolute; bottom: 0; left: 0; right: 0; padding: 2px;
                       font-size: 9px; letter-spacing: 1px; text-align: center;
                       background: rgba(0,0,0,0.7); color: #e0d4f5; }
        .reel-label { font-size: 10px; letter-spacing: 3px; color: #666; text-transform: uppercase;
                      margin-top: 15px; }

        /* Serial console */
        .console-toggle { margin-top: 20px; font-size: 10px; letter-spacing: 3px; color: #666;
            cursor: pointer; text-transform: uppercase; user-select: none; }
        .console-toggle:hover { color: #8b45ff; }
        #console-wrap { display: none; margin-top: 10px; }
        #console { background: #111118; border: 1px solid rgba(139,69,255,0.2); border-radius: 8px;
            padding: 12px; height: 200px; overflow-y: auto; text-align: left;
            font-size: 11px; line-height: 1.5; color: #8f8; white-space: pre-wrap;
            word-break: break-all; }
        #console::-webkit-scrollbar { width: 6px; }
        #console::-webkit-scrollbar-thumb { background: rgba(139,69,255,0.3); border-radius: 3px; }

        /* Decorative elements */
        .hex { position: fixed; opacity: 0.03; pointer-events: none; }
        .hex1 { top: 10%; left: 5%; font-size: 200px; animation: float1 12s ease-in-out infinite; }
        .hex2 { bottom: 10%; right: 5%; font-size: 150px; animation: float2 10s ease-in-out infinite; }
        @keyframes float1 { 0%,100%{transform:translateY(0) rotate(0deg)} 50%{transform:translateY(-20px) rotate(5deg)} }
        @keyframes float2 { 0%,100%{transform:translateY(0) rotate(0deg)} 50%{transform:translateY(15px) rotate(-5deg)} }
    </style>
</head>
<body>
    <div class="hex hex1">&#x2B21;</div>
    <div class="hex hex2">&#x2B21;</div>
    <div class="container">
        <h1>WOOK or WOKE</h1>
        <div class="subtitle">crystal &amp; human energy analysis</div>

        <div class="main-layout">
            <div class="left-col">
                <div class="cam-frame">
                    <img id="photo" src="/capture" alt="Awaiting crystal...">

                    <div class="cam-loading" id="cam-loading">
                        <div class="crystal-spin"></div>
                        <div class="spinner-text">capturing...</div>
                    </div>
                </div>
                <div class="controls">
                    <button class="btn-cap" onclick="capture()">Capture</button>
                    <button class="btn-judge" onclick="judge()">Judge Crystal</button>

                    <button class="btn-cap" id="led-btn" onclick="toggleLed()">LED On</button>
                </div>
                <div class="controls">
                    <label style="font-size:11px;letter-spacing:2px;color:#666;">FLASH BRIGHTNESS</label>
                    <input type="range" id="brightness" min="0" max="255" value="255" style="width:100%;accent-color:#8b45ff;" oninput="setBrightness(this.value)">
                </div>
            </div>
            <div class="right-col">
                <div id="spinner">
                    <div class="crystal-spin"></div>
                    <div class="spinner-text">reading crystal energy...</div>
                </div>
                <div id="result">
                    <div id="score-text"></div>
                    <div class="spectrum">
                        <div id="score-bar"><div id="score-marker" style="left:50%"></div></div>
                        <div class="labels"><span>W O O K</span><span>W O K E</span></div>
                    </div>
                    <div id="description"></div>
                </div>
            </div>
        </div>
        <div class="reel-label" id="reel-label" style="display:none;">JUDGMENTS</div>
        <div class="reel" id="reel"></div>
        <div class="console-toggle" onclick="toggleConsole()">&#x25B6; SERIAL CONSOLE</div>
        <div id="console-wrap"><div id="console"></div></div>
    </div>
    <script>
        var history = [];
        var ledOn = false;
        function setBrightness(v) { fetch('/brightness?v=' + v); }
        function toggleLed() {
            ledOn = !ledOn;
            fetch('/led?state=' + (ledOn ? '1' : '0'));
            document.getElementById('led-btn').innerText = ledOn ? 'LED Off' : 'LED On';
        }
        function capture() {
            document.getElementById('photo').style.display = 'block';
            document.getElementById('photo').src = '/capture?' + Date.now();
        }
        var vibes = ['','LEVEL 3 WOOK','LEVEL 2 WOOK','LEVEL 1 WOOK','NORMIE','LEVEL 1 WOKE','LEVEL 2 WOKE','LEVEL 3 WOKE'];
        function addThumb(photoUrl, data) {
            var idx = history.length;
            history.push({photo: photoUrl, data: data});
            document.getElementById('reel-label').style.display = 'block';
            var reel = document.getElementById('reel');
            var div = document.createElement('div');
            div.className = 'thumb';
            div.onclick = function() { showFromHistory(idx); };
            var timg = document.createElement('img');
            timg.src = photoUrl;
            div.appendChild(timg);
            var label = document.createElement('div');
            label.className = 'thumb-score';
            if (data.error) { label.innerText = 'ERR'; }
            else if (data.score == 0) { label.innerText = '?'; }
            else { label.innerText = vibes[data.score]; }
            div.appendChild(label);
            reel.insertBefore(div, reel.firstChild);
            reel.scrollLeft = 0;
        }
        function showFromHistory(idx) {
            var h = history[idx];
            document.getElementById('photo').src = h.photo;
            document.getElementById('photo').style.display = 'block';

            displayScore(h.data);
        }
        function displayScore(data) {
            document.getElementById('spinner').style.display = 'none';
            document.getElementById('result').style.display = 'block';
            if (data.error) {
                document.getElementById('score-text').innerText = 'ERROR';
                document.getElementById('description').innerText = data.error;
                return;
            }
            var pct = (data.score / 7) * 100;
            document.getElementById('score-marker').style.left = 'calc(' + pct + '% - 8px)';
            if (data.score == 0) {
                document.getElementById('score-text').innerText = data.subject === 'human' ? 'UNREADABLE HUMAN' : 'NOT A CRYSTAL';
            } else {
                document.getElementById('score-text').innerText = vibes[data.score];
            }
            document.getElementById('description').innerText = data.description;
        }
        function showResult(data) {
            var img = document.getElementById('photo');
            var photoUrl = '/photo?' + Date.now();
            img.onload = function() {
                document.getElementById('cam-loading').classList.remove('active');
                // Save thumbnail as data URL
                var c = document.createElement('canvas');
                c.width = img.naturalWidth; c.height = img.naturalHeight;
                c.getContext('2d').drawImage(img, 0, 0);
                try { addThumb(c.toDataURL('image/jpeg', 0.6), data); } catch(e) { addThumb(photoUrl, data); }
                img.onload = null;
            };
            img.src = photoUrl;
            img.style.display = 'block';

            displayScore(data);
        }
        function judge() {
            document.getElementById('cam-loading').classList.add('active');
            document.getElementById('spinner').style.display = 'block';
            document.getElementById('result').style.display = 'none';
            fetch('/judge').then(r => r.json()).then(showResult).catch(e => {
                document.getElementById('cam-loading').classList.remove('active');
                document.getElementById('spinner').style.display = 'none';
                document.getElementById('result').style.display = 'block';
                document.getElementById('score-text').innerText = 'ERROR';
                document.getElementById('description').innerText = e.toString();
            });
        }
        // Serial console
        var consoleCursor = 0;
        var consoleOpen = false;
        var consoleTimer = null;
        function toggleConsole() {
            consoleOpen = !consoleOpen;
            var w = document.getElementById('console-wrap');
            var t = document.querySelector('.console-toggle');
            w.style.display = consoleOpen ? 'block' : 'none';
            t.innerHTML = (consoleOpen ? '&#x25BC;' : '&#x25B6;') + ' SERIAL CONSOLE';
            if (consoleOpen && !consoleTimer) {
                pollLog();
                consoleTimer = setInterval(pollLog, 1000);
            } else if (!consoleOpen && consoleTimer) {
                clearInterval(consoleTimer);
                consoleTimer = null;
            }
        }
        function pollLog() {
            fetch('/log?since=' + consoleCursor).then(r => r.json()).then(d => {
                if (d.data.length > 0) {
                    var el = document.getElementById('console');
                    el.textContent += d.data;
                    // Keep buffer from growing forever in DOM
                    if (el.textContent.length > 16000)
                        el.textContent = el.textContent.slice(-12000);
                    el.scrollTop = el.scrollHeight;
                }
                consoleCursor = d.cursor;
            }).catch(function(){});
        }
        // Poll for button-triggered results
        setInterval(function() {
            fetch('/button-result').then(r => r.json()).then(data => {
                if (data.status === 'judging') {
                    document.getElementById('cam-loading').classList.add('active');
                    document.getElementById('spinner').style.display = 'block';
                    document.getElementById('result').style.display = 'none';
                } else if (data.score !== undefined || data.error) {
                    showResult(data);
                }
            }).catch(function(){});
        }, 1500);
    </script>
</body>
</html>
)rawliteral";
    server.send(200, "text/html", html);
}

void handleCapture() {
    camera_fb_t *fb = flashCapture();
    if (!fb) {
        server.send(500, "text/plain", "Camera capture failed");
        return;
    }
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Content-Disposition", "inline; filename=capture.jpg");
    server.send_P(200, "image/jpeg", (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
}

// Serve the last captured photo instantly (no new capture)
void handlePhoto() {
    if (!lastPhoto || lastPhotoLen == 0) {
        server.send(404, "text/plain", "No photo yet");
        return;
    }
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.send_P(200, "image/jpeg", (const char *)lastPhoto, lastPhotoLen);
}

void handleJudge() {
    Log.println("\n=== JUDGING CRYSTAL ===");

    camera_fb_t *fb = flashCapture();
    if (!fb) {
        server.send(500, "application/json", "{\"error\":\"Camera capture failed\"}");
        return;
    }

    Log.printf("Captured image: %d bytes\n", fb->len);

    String result = callClaude(fb);
    esp_camera_fb_return(fb);

    server.send(200, "application/json", result);
}

void handleStream() {
    WiFiClient client = server.client();
    String boundary = "frame";
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: multipart/x-mixed-replace; boundary=" + boundary);
    client.println();

    while (client.connected()) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) continue;

        client.println("--" + boundary);
        client.println("Content-Type: image/jpeg");
        client.printf("Content-Length: %d\r\n\r\n", fb->len);
        client.write(fb->buf, fb->len);
        client.println();
        esp_camera_fb_return(fb);
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    ledcSetup(LED_PWM_CHANNEL, LED_PWM_FREQ, LED_PWM_RESOLUTION);
    ledcAttachPin(LED_FLASH, LED_PWM_CHANNEL);
    ledcWrite(LED_PWM_CHANNEL, 0);

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, FALLING);

    Serial2.begin(9600, SERIAL_8N1, LED_SERIAL_RX, LED_SERIAL_TX);
    Log.println("LED serial on TX=14, RX=15");

    Log.println("\n=== Wook or Woke ===");

    initCamera();

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Log.printf("Connecting to %s", WIFI_SSID);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Log.print(".");
    }
    Log.println();
    Log.printf("Connected! IP: http://%s\n", WiFi.localIP().toString().c_str());

    server.on("/", handleRoot);
    server.on("/capture", handleCapture);
    server.on("/photo", handlePhoto);
    server.on("/judge", handleJudge);
    server.on("/stream", handleStream);
    server.on("/led", []() {
        String state = server.arg("state");
        if (state == "1") {
            ledManualOn = true;
            ledcWrite(LED_PWM_CHANNEL, ledBrightness);
            server.send(200, "text/plain", "on");
        } else {
            ledManualOn = false;
            ledcWrite(LED_PWM_CHANNEL, 0);
            server.send(200, "text/plain", "off");
        }
    });
    server.on("/brightness", []() {
        ledBrightness = server.arg("v").toInt();
        if (ledBrightness < 0) ledBrightness = 0;
        if (ledBrightness > 255) ledBrightness = 255;
        if (ledManualOn) ledcWrite(LED_PWM_CHANNEL, ledBrightness);
        server.send(200, "text/plain", String(ledBrightness));
    });
    // Web serial log endpoint — returns ring buffer contents since cursor
    server.on("/log", []() {
        int since = server.arg("since").toInt();
        int avail = logCount - since;
        if (avail <= 0) {
            server.send(200, "application/json", "{\"cursor\":" + String(logCount) + ",\"data\":\"\"}");
            return;
        }
        if (avail > LOG_BUF_SIZE) avail = LOG_BUF_SIZE;
        int start = (logHead - avail + LOG_BUF_SIZE) % LOG_BUF_SIZE;
        String out;
        out.reserve(avail + 40);
        out = "{\"cursor\":";
        out += String(logCount);
        out += ",\"data\":\"";
        for (int i = 0; i < avail; i++) {
            char c = logBuf[(start + i) % LOG_BUF_SIZE];
            if (c == '"') out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else if (c == '\r') continue;
            else if (c >= 32) out += c;
        }
        out += "\"}";
        server.send(200, "application/json", out);
    });
    // OTA firmware update page
    server.on("/update", HTTP_GET, []() {
        server.send(200, "text/html",
            "<html><body style='font-family:monospace;background:#0a0a0f;color:#e0d4f5;text-align:center;padding:40px'>"
            "<h2>ESP32-CAM OTA Update</h2>"
            "<form method='POST' action='/update' enctype='multipart/form-data'>"
            "<input type='file' name='firmware' accept='.bin' style='margin:20px'><br>"
            "<input type='submit' value='Upload & Flash' style='padding:10px 30px;font-size:16px;cursor:pointer'>"
            "</form></body></html>");
    });
    server.on("/update", HTTP_POST, []() {
        server.sendHeader("Connection", "close");
        server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK - Rebooting...");
        delay(500);
        ESP.restart();
    }, []() {
        HTTPUpload& upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
            Log.printf("OTA Update: %s\n", upload.filename.c_str());
            esp_camera_deinit();  // free ~370KB of frame buffer RAM for OTA
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Log);
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
                Update.printError(Log);
        } else if (upload.status == UPLOAD_FILE_END) {
            if (Update.end(true)) Log.printf("OTA Success: %u bytes\n", upload.totalSize);
            else Update.printError(Log);
        }
    });
    server.on("/button-result", []() {
        if (buttonJudging) {
            server.send(200, "application/json", "{\"status\":\"judging\"}");
        } else if (lastButtonResult.length() > 0) {
            String result = lastButtonResult;
            lastButtonResult = "";
            server.send(200, "application/json", result);
        } else {
            server.send(200, "application/json", "{\"status\":\"idle\"}");
        }
    });
    server.begin();
    Log.println("Web server started");

    ArduinoOTA.setHostname("wook-or-woke-cam");
    ArduinoOTA.onStart([]() { esp_camera_deinit(); Log.println("OTA Start"); });
    ArduinoOTA.onEnd([]() { Log.println("\nOTA End"); });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Log.printf("OTA Progress: %u%%\r", (progress / (total / 100)));
    });
    ArduinoOTA.onError([](ota_error_t error) {
        Log.printf("OTA Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) Log.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR) Log.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) Log.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) Log.println("Receive Failed");
        else if (error == OTA_END_ERROR) Log.println("End Failed");
    });
    ArduinoOTA.begin();
    Log.println("ArduinoOTA ready");
}

void loop() {
    server.handleClient();
    ArduinoOTA.handle();

    // Check for DONE from LED controller
    if (Serial2.available()) {
        String msg = Serial2.readStringUntil('\n');
        msg.trim();
        if (msg == "DONE") {
            Log.println("LED controller finished animation");
            waitingForLedDone = false;
        }
    }

    // Safety timeout for LED done
    if (waitingForLedDone && millis() - ledDoneTimeout > LED_DONE_TIMEOUT_MS) {
        Log.println("LED DONE timeout - unlocking button");
        waitingForLedDone = false;
    }

    if (buttonPressed && !waitingForLedDone) {
        buttonPressed = false;
        buttonJudging = true;
        Log.println("\n=== BUTTON PRESSED - JUDGING CRYSTAL ===");

        // Check if LED controller is connected
        ledControllerPresent = checkLedController();
        Log.printf("LED controller: %s\n", ledControllerPresent ? "CONNECTED" : "not found");

        if (ledControllerPresent) {
            waitingForLedDone = true;
            ledDoneTimeout = millis();
            Serial2.println("START");
            Log.println("Sent START to LED controller");
        }

        // Flash and capture
        camera_fb_t *fb = flashCapture();

        if (fb) {
            Log.printf("Captured image: %d bytes\n", fb->len);
            lastButtonResult = callClaude(fb);
            esp_camera_fb_return(fb);

            // Check if Claude call failed
            bool hasFailed = lastButtonResult.indexOf("\"error\"") >= 0;
            if (hasFailed) {
                Log.println("Claude call failed - unlocking button");
                if (ledControllerPresent) Serial2.println("SCORE:0");
                waitingForLedDone = false;
            } else {
                // Extract score and description, send to LED controller + log server
                int scoreIdx = lastButtonResult.indexOf("\"score\":");
                int descIdx = lastButtonResult.indexOf("\"description\":\"");
                String description = "";
                if (descIdx >= 0) {
                    int start = descIdx + 15;  // skip past "description":"
                    int end = lastButtonResult.indexOf("\"", start);
                    if (end > start) description = lastButtonResult.substring(start, end);
                }
                if (scoreIdx >= 0) {
                    int score = lastButtonResult.charAt(scoreIdx + 8) - '0';
                    if (ledControllerPresent) {
                        Serial2.printf("SCORE:%d\n", score);
                        Log.printf("Sent SCORE:%d to LED controller\n", score);
                    }
                    uploadResult(score, description);
                }
            }
        } else {
            lastButtonResult = "{\"error\":\"Camera capture failed\"}";
            if (ledControllerPresent) Serial2.println("SCORE:0");
            waitingForLedDone = false;
        }
        buttonJudging = false;
        Log.println("Button judge complete");
    } else if (buttonPressed && waitingForLedDone) {
        buttonPressed = false; // ignore press while animating
        Log.println("Button ignored - waiting for LED animation to finish");
    }
}
