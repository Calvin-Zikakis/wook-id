#include <Arduino.h>
#include "esp_camera.h"
#include "config.h"

// ---------------------------------------------------------------------------
// AI-Thinker ESP32-CAM camera pin definitions
// ---------------------------------------------------------------------------
#define PWDN_GPIO_NUM    32
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM     0
#define SIOD_GPIO_NUM    26
#define SIOC_GPIO_NUM    27
#define Y9_GPIO_NUM      35
#define Y8_GPIO_NUM      34
#define Y7_GPIO_NUM      39
#define Y6_GPIO_NUM      36
#define Y5_GPIO_NUM      21
#define Y4_GPIO_NUM      19
#define Y3_GPIO_NUM      18
#define Y2_GPIO_NUM       5
#define VSYNC_GPIO_NUM   25
#define HREF_GPIO_NUM    23
#define PCLK_GPIO_NUM    22

// ---------------------------------------------------------------------------
// Button and UART pins
// ---------------------------------------------------------------------------
#define BUTTON_PIN   12
#define FLASH_PIN     4   // Onboard flash LED
#define CAM_TX       14   // → Lolin S3 GPIO 18 (RX)
#define CAM_RX       15   // ← Lolin S3 GPIO 17 (TX)

bool canPress = true;
unsigned long lastPhotoTime = 0;
#define CAM_TIMEOUT_MS  15000  // Re-enable button after 15s if no READY received

// ---------------------------------------------------------------------------
void initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_VGA;     // 640x480 — good balance of quality vs size
  config.jpeg_quality = 12;                // 10=best, 63=worst
  config.fb_count     = 1;
  config.grab_mode    = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
  } else {
    Serial.println("Camera initialized");
  }
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== ESP32-CAM Startup ===");

  Serial.printf("Initializing UART1 at %d baud (TX=%d, RX=%d)...\n", UART_BAUD, CAM_TX, CAM_RX);
  Serial1.begin(UART_BAUD, SERIAL_8N1, CAM_RX, CAM_TX);

  Serial.printf("Button on GPIO %d (INPUT_PULLUP)\n", BUTTON_PIN);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Serial.printf("Flash LED on GPIO %d\n", FLASH_PIN);
  pinMode(FLASH_PIN, OUTPUT);
  digitalWrite(FLASH_PIN, LOW);

  Serial.println("Initializing camera...");
  initCamera();

  Serial.println("=== Ready — press button to capture ===\n");
}

// ---------------------------------------------------------------------------
void loop() {
  // Check for READY message from Lolin S3
  while (Serial1.available()) {
    if (Serial1.read() == MSG_READY) {
      canPress = true;
      Serial.println("Ready for next photo");
    }
  }

  // Timeout — re-enable button if Lolin hasn't responded in 15s
  if (!canPress && (millis() - lastPhotoTime > CAM_TIMEOUT_MS)) {
    canPress = true;
    Serial.println("[WARN] Timeout — re-enabling button");
  }

  // Debug: print button state every second
  static unsigned long lastDebugPrint = 0;
  if (millis() - lastDebugPrint > 1000) {
    Serial.printf("[DEBUG] button=%s  canPress=%s\n",
      digitalRead(BUTTON_PIN) == LOW ? "PRESSED" : "open",
      canPress ? "yes" : "no");
    lastDebugPrint = millis();
  }

  // Check button (active low with pullup)
  if (canPress && digitalRead(BUTTON_PIN) == LOW) {
    delay(50);  // Simple debounce
    if (digitalRead(BUTTON_PIN) == LOW) {
      canPress = false;
      lastPhotoTime = millis();
      Serial.println("Button pressed — capturing photo...");

      // Flash on, flush stale frame, capture fresh, flash off
      digitalWrite(FLASH_PIN, HIGH);
      delay(150);  // Let the flash illuminate the scene

      // Discard any pre-buffered frame from before the flash
      camera_fb_t* stale = esp_camera_fb_get();
      if (stale) esp_camera_fb_return(stale);

      // Now capture a fresh frame with the flash on
      camera_fb_t* fb = esp_camera_fb_get();
      digitalWrite(FLASH_PIN, LOW);
      if (!fb) {
        Serial.println("Camera capture failed");
        canPress = true;
        return;
      }

      Serial.printf("Photo captured: %u bytes\n", fb->len);

      // Send START marker
      Serial.println("[UART] Sending START marker");
      Serial1.write(MSG_START);

      // Send image size (4 bytes, little-endian)
      uint32_t len = fb->len;
      Serial.printf("[UART] Sending image size: %u bytes\n", len);
      Serial1.write((uint8_t*)&len, 4);

      // Send image data
      Serial.println("[UART] Sending image data...");
      unsigned long sendStart = millis();
      Serial1.write(fb->buf, fb->len);
      Serial1.flush();
      unsigned long sendTime = millis() - sendStart;

      Serial.printf("[UART] Done — sent %u bytes in %lu ms\n", len, sendTime);
      Serial.println("Waiting for READY from Lolin S3...");
      esp_camera_fb_return(fb);
    }
  }
}
