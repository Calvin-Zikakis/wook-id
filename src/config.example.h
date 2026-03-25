#pragma once

// ---------------------------------------------------------------------------
// WiFi credentials
// ---------------------------------------------------------------------------
#define WIFI_SSID "your_wifi_ssid"
#define WIFI_PASS "your_wifi_password"

// ---------------------------------------------------------------------------
// Claude API key (ESP32-CAM only)
// ---------------------------------------------------------------------------
#define API_KEY "sk-ant-..."

// ---------------------------------------------------------------------------
// Logging server (optional — comment out to disable)
// ---------------------------------------------------------------------------
#define LOG_SERVER_HOST "wook2woke.com"
#define LOG_SERVER_PORT 443
#define LOG_API_KEY     "your_log_api_key"

// ---------------------------------------------------------------------------
// UART config — must match between ESP32-CAM and LED controller
// ---------------------------------------------------------------------------
#define UART_BAUD 9600
