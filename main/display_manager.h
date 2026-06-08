/*
 * display_manager.h
 * TFT 1.8" ST7735 Display Manager
 *
 * Wiring (ESP32-S3):
 *   ST7735   ESP32-S3
 *   VCC   →  3.3V
 *   GND   →  GND
 *   SCL   →  GPIO 19  (SPI SCK)
 *   SDA   →  GPIO 20  (SPI MOSI)
 *   RES   →  GPIO 21  (Reset)
 *   DC    →  GPIO 47  (Data/Command)
 *   CS    →  GPIO 45  (Chip Select)
 *   BLK   →  3.3V or GPIO (backlight)
 *
 * Library: Adafruit ST7735 + Adafruit GFX
 * Install: Arduino Library Manager → "Adafruit ST7735"
 *          (Adafruit GFX will auto-install as dependency)
 *
 * Features:
 *   - Status bar (WiFi, API, state)
 *   - Scrolling text area for Gemini responses
 *   - Subject + Index display when recalling
 */

#pragma once
#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>

// ── Pin config ────────────────────────────────────────────────
#define TFT_CS    45
#define TFT_DC    47
#define TFT_RST   21
#define TFT_MOSI  20   // SDA
#define TFT_SCLK  19   // SCL
// SPI MOSI = GPIO 20, SCK = GPIO 19 (set via Arduino SPI config)

// ── Colors (RGB565) ───────────────────────────────────────────
#define C_BG        0x0841   // Dark blue-gray background
#define C_STATUS_BG 0x0228   // Darker status bar
#define C_TEXT      0xFFFF   // White
#define C_MUTED     0x8410   // Gray
#define C_ACCENT    0x051F   // Blue
#define C_GREEN     0x07E0   // Green
#define C_RED       0xF800   // Red
#define C_YELLOW    0xFFE0   // Yellow
#define C_CYAN      0x07FF   // Cyan

// ── Layout ────────────────────────────────────────────────────
// Screen: 160x128 (landscape) or 128x160 (portrait)
// Using landscape: 160w x 128h
#define SCREEN_W    160
#define SCREEN_H    128
#define STATUS_H    18   // Top status bar height
#define TEXT_Y      (STATUS_H + 4)
#define TEXT_H      (SCREEN_H - STATUS_H - 4)
#define CHAR_W      6    // pixels per char at textSize(1)
#define CHAR_H      8
#define LINE_H      10   // line height with spacing
#define LINES_MAX   ((TEXT_H) / LINE_H)   // ~11 lines

class DisplayManager {
public:
  DisplayManager() : tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST) {}

  void begin() {
    tft.initR(INITR_BLACKTAB);      // ST7735S black tab
    tft.setRotation(1);             // Landscape
    tft.fillScreen(C_BG);
    drawStatusBar("Booting...", C_MUTED);
    showCenter("AI Student", "Assistant", C_ACCENT);
    Serial.println("[TFT] Display initialized");
  }

  // ── Status bar (top strip) ───────────────────────────────
  void drawStatusBar(const char* msg, uint16_t color = C_TEXT) {
    tft.fillRect(0, 0, SCREEN_W, STATUS_H, C_STATUS_BG);
    tft.setTextColor(color);
    tft.setTextSize(1);
    tft.setCursor(4, 5);
    tft.print(msg);
  }

  void setStatus(const char* left, const char* right, uint16_t color = C_TEXT) {
    tft.fillRect(0, 0, SCREEN_W, STATUS_H, C_STATUS_BG);
    tft.setTextColor(color);
    tft.setTextSize(1);
    tft.setCursor(4, 5);
    tft.print(left);
    // Right-align right text
    int rx = SCREEN_W - strlen(right) * CHAR_W - 4;
    tft.setCursor(rx, 5);
    tft.print(right);
    // Separator line
    tft.drawFastHLine(0, STATUS_H - 1, SCREEN_W, C_ACCENT);
  }

  // ── State screens ────────────────────────────────────────
  void showIdle() {
    setStatus("AI Student", "IDLE", C_MUTED);
    clearText();
    printLine("Press button", C_MUTED, 0);
    printLine("to ask a", C_MUTED, 1);
    printLine("question...", C_MUTED, 2);
  }

  void showListening() {
    setStatus("🎤 Listening...", "", C_GREEN);
    clearText();
    printLine("Listening...", C_GREEN, 0);
    // Animated dots drawn by updateListeningAnim()
  }

  void updateListeningAnim() {
    // Simple dot animation at line 1
    static int dotCount = 0;
    static uint32_t lastAnim = 0;
    if (millis() - lastAnim < 400) return;
    lastAnim = millis();
    dotCount = (dotCount + 1) % 4;

    tft.fillRect(0, TEXT_Y + LINE_H, SCREEN_W, LINE_H, C_BG);
    tft.setTextColor(C_GREEN);
    tft.setTextSize(1);
    tft.setCursor(4, TEXT_Y + LINE_H + 1);
    for (int i = 0; i < dotCount; i++) tft.print("● ");
  }

  void showProcessing() {
    setStatus("⏳ Processing...", "", C_YELLOW);
    clearText();
    printLine("Thinking...", C_YELLOW, 0);
  }

  void showSpeaking(const String& text) {
    setStatus("🔊 Speaking", "", C_CYAN);
    clearText();
    showWrappedText(text, C_TEXT);
  }

  void showRecalled(const String& subject, const String& index, const String& text) {
    // Subject + index on status
    String statusStr = subject + ": " + index;
    if (statusStr.length() > 22) statusStr = statusStr.substring(0, 22);
    setStatus(statusStr.c_str(), "📖", C_CYAN);
    clearText();
    showWrappedText(text, C_TEXT);
  }

  void showSaved(const String& subject, const String& index) {
    setStatus("✓ Saved", subject.c_str(), C_GREEN);
    clearText();
    printLine("Saved to:", C_GREEN, 0);
    printLine(subject.c_str(), C_ACCENT, 1);
    printLine(index.c_str(), C_TEXT, 2);
  }

  void showError(const String& msg) {
    setStatus("⚠ Error", "", C_RED);
    clearText();
    showWrappedText(msg, C_RED);
  }

  void showConfig() {
    setStatus("⚙ Config Mode", "", C_YELLOW);
    clearText();
    printLine("Connect to WiFi:", C_MUTED, 0);
    printLine("AI-Student-Config", C_ACCENT, 1);
    printLine("Pass: student123", C_MUTED, 2);
    printLine("", C_MUTED, 3);
    printLine("Visit:", C_MUTED, 4);
    printLine("192.168.4.1", C_CYAN, 5);
  }

  void showReplay() {
    setStatus("🔁 Replay", "", C_CYAN);
    // Keep existing text, just update status
  }

  // ── Center display (splash) ──────────────────────────────
  void showCenter(const char* line1, const char* line2, uint16_t color) {
    clearText();
    tft.setTextSize(2);
    tft.setTextColor(color);
    int x1 = (SCREEN_W - strlen(line1) * CHAR_W * 2) / 2;
    int y1 = TEXT_Y + (TEXT_H / 2) - 16;
    tft.setCursor(max(0, x1), y1);
    tft.print(line1);

    tft.setTextSize(1);
    tft.setTextColor(C_MUTED);
    int x2 = (SCREEN_W - strlen(line2) * CHAR_W) / 2;
    tft.setCursor(max(0, x2), y1 + 20);
    tft.print(line2);
  }

  // ── WiFi/API status overlay ──────────────────────────────
  void showConnecting(const char* ssid) {
    setStatus("Connecting...", "", C_YELLOW);
    clearText();
    printLine("WiFi:", C_MUTED, 0);
    printLine(ssid, C_ACCENT, 1);
  }

  void showConnected(const char* ip) {
    setStatus("✓ Connected", ip, C_GREEN);
  }

private:
  Adafruit_ST7735 tft;
  int  scrollPos  = 0;
  int  lineCount  = 0;

  void clearText() {
    tft.fillRect(0, TEXT_Y, SCREEN_W, TEXT_H, C_BG);
    lineCount  = 0;
    scrollPos  = 0;
  }

  void printLine(const char* text, uint16_t color, int lineIdx) {
    int y = TEXT_Y + lineIdx * LINE_H;
    if (y + LINE_H > SCREEN_H) return;
    tft.fillRect(0, y, SCREEN_W, LINE_H, C_BG);
    tft.setTextColor(color);
    tft.setTextSize(1);
    tft.setCursor(4, y + 1);
    // Truncate to fit screen width
    char buf[28];
    strncpy(buf, text, 27);
    buf[27] = '\0';
    tft.print(buf);
  }

  // Word-wrap text across multiple lines
  void showWrappedText(const String& text, uint16_t color) {
    int charsPerLine = (SCREEN_W - 8) / CHAR_W;  // ~25 chars
    int line = 0;

    for (int i = 0; i < (int)text.length() && line < LINES_MAX; ) {
      // Find word boundary
      int end = min((int)text.length(), i + charsPerLine);
      if (end < (int)text.length()) {
        // Back up to last space
        int sp = text.lastIndexOf(' ', end);
        if (sp > i) end = sp + 1;
      }
      String chunk = text.substring(i, end);
      chunk.trim();
      printLine(chunk.c_str(), color, line++);
      i = end;
      // Skip leading space
      while (i < (int)text.length() && text[i] == ' ') i++;
    }
  }
};
