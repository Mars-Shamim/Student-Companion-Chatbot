/*
 * led_controller.h  v1.0
 * Non-blocking RGB LED state machine using WS2812B (NeoPixel)
 *
 * LED States:
 *   LIVE + gemini-3.1-flash-live-preview           → Cyan breathing
 *   LIVE + gemini-2.5-flash-native-audio-preview   → Blue breathing
 *   SINGLE + gemini-2.5-flash                      → Green solid
 *   Connecting                       → Orange spin
 *   Listening                        → Green fast pulse
 *   Speaking                         → Yellow pulse
 *   Error                            → Red flash
 *   Mode switch                      → White double blink
 *
 * Hardware: WS2812B / SK6812 single LED or strip
 * Library:  Adafruit NeoPixel
 */

#pragma once
#include <Adafruit_NeoPixel.h>
#include <Arduino.h>

// ── LED Pin (override in main.ino before include if needed) ──
#ifndef LED_NEO_PIN
#define LED_NEO_PIN 48
#endif

#ifndef LED_NEO_COUNT
#define LED_NEO_COUNT 1
#endif



// ── LED States ────────────────────────────────────────────────
enum LedState {
  LED_OFF,
  LED_IDLE_LIVE_PRIMARY,  // Cyan breathing  (gemini-3.1-flash-live)
  LED_IDLE_LIVE_FALLBACK, // Blue breathing   (gemini-2.5-flash-live)
  LED_IDLE_SINGLE,        // Green solid      (gemini-2.5-flash)
  LED_CONNECTING,         // Orange spin
  LED_LISTENING,          // Green fast pulse
  LED_SPEAKING,           // Yellow pulse
  LED_ERROR,              // Red flash
  LED_MODE_SWITCH         // White double blink
};

class LedController {
public:
  LedController() : strip(LED_NEO_COUNT, LED_NEO_PIN, NEO_GRB + NEO_KHZ800) {}

  void begin() {
    strip.begin();
    strip.setBrightness(80); // Reasonable default
    strip.clear();
    strip.show();
    state = LED_OFF;
    Serial.println("[LED] NeoPixel initialized");
  }

  // ── Set LED state ─────────────────────────────────────────
  void setState(LedState newState) {
    if (newState == state && newState != LED_MODE_SWITCH)
      return;
    state = newState;
    stateStart = millis();
    animPhase = 0;

    // For mode switch, auto-return to previous after animation
    if (newState == LED_MODE_SWITCH) {
      modeSwitchDone = false;
    }
  }

  // ── Set the idle state that we return to after transient states ──
  void setIdleState(LedState idle) { returnState = idle; }

  // ── Non-blocking update (call every loop) ─────────────────
  void update() {
    uint32_t now = millis();
    uint32_t elapsed = now - stateStart;

    switch (state) {
    case LED_OFF:
      setColor(0, 0, 0);
      break;

    case LED_IDLE_LIVE_PRIMARY: // Cyan breathing
      breathe(0, 255, 255, elapsed);
      break;

    case LED_IDLE_LIVE_FALLBACK: // Blue breathing
      breathe(0, 80, 255, elapsed);
      break;

    case LED_IDLE_SINGLE: // Green solid
      setColor(0, 200, 0);
      break;

    case LED_CONNECTING: // Orange spin (pulsing since single LED)
      spinOrange(elapsed);
      break;

    case LED_LISTENING:                   // Green fast pulse
      fastPulse(0, 255, 0, elapsed, 200); // 200ms period
      break;

    case LED_SPEAKING:                      // Yellow pulse
      fastPulse(255, 200, 0, elapsed, 500); // 500ms period
      break;

    case LED_ERROR:                        // Red flash
      flashColor(255, 0, 0, elapsed, 150); // 150ms on/off
      break;

    case LED_MODE_SWITCH: // White double blink
      doubleBlink(255, 255, 255, elapsed);
      break;
    }

    strip.show();
  }

  // ── Check if mode switch animation is done ────────────────
  bool isModeSwitchDone() { return modeSwitchDone; }

  // ── Quick flash for feedback (blocking, use sparingly) ────
  void quickFlash(uint8_t r, uint8_t g, uint8_t b, int count = 2,
                  int ms = 120) {
    for (int i = 0; i < count; i++) {
      setColor(r, g, b);
      strip.show();
      delay(ms);
      setColor(0, 0, 0);
      strip.show();
      delay(ms);
    }
  }

private:
  Adafruit_NeoPixel strip;
  LedState state = LED_OFF;
  LedState returnState = LED_OFF;
  uint32_t stateStart = 0;
  int animPhase = 0;
  bool modeSwitchDone = false;

  // ── Set all pixels to one color ───────────────────────────
  void setColor(uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; i < LED_NEO_COUNT; i++) {
      strip.setPixelColor(i, strip.Color(r, g, b));
    }
  }

  // ── Breathing effect (smooth sine wave) ───────────────────
  void breathe(uint8_t r, uint8_t g, uint8_t b, uint32_t elapsed) {
    // Period: ~3 seconds for a full breath cycle
    float phase = (elapsed % 3000) / 3000.0f * 2.0f * PI;
    float brightness = (sin(phase) + 1.0f) / 2.0f; // 0.0 to 1.0
    brightness = 0.05f + brightness * 0.95f;       // Never fully off

    setColor((uint8_t)(r * brightness), (uint8_t)(g * brightness),
             (uint8_t)(b * brightness));
  }

  // ── Spin effect for connecting (orange pulsing) ───────────
  void spinOrange(uint32_t elapsed) {
    // Fast breathing with orange color (period: 800ms)
    float phase = (elapsed % 800) / 800.0f * 2.0f * PI;
    float brightness = (sin(phase) + 1.0f) / 2.0f;
    brightness = 0.1f + brightness * 0.9f;

    setColor((uint8_t)(255 * brightness), (uint8_t)(100 * brightness), 0);
  }

  // ── Fast pulse ────────────────────────────────────────────
  void fastPulse(uint8_t r, uint8_t g, uint8_t b, uint32_t elapsed,
                 uint16_t periodMs) {
    float phase = (elapsed % periodMs) / (float)periodMs * 2.0f * PI;
    float brightness = (sin(phase) + 1.0f) / 2.0f;
    brightness = brightness * brightness; // More aggressive curve

    setColor((uint8_t)(r * brightness), (uint8_t)(g * brightness),
             (uint8_t)(b * brightness));
  }

  // ── Flash on/off ──────────────────────────────────────────
  void flashColor(uint8_t r, uint8_t g, uint8_t b, uint32_t elapsed,
                  uint16_t periodMs) {
    bool on = ((elapsed / periodMs) % 2) == 0;
    if (on)
      setColor(r, g, b);
    else
      setColor(0, 0, 0);
  }

  // ── Double blink then stop ────────────────────────────────
  void doubleBlink(uint8_t r, uint8_t g, uint8_t b, uint32_t elapsed) {
    // Timeline: ON(100) OFF(100) ON(100) OFF(200) = 500ms total
    if (elapsed < 100)
      setColor(r, g, b); // 1st blink ON
    else if (elapsed < 200)
      setColor(0, 0, 0); // gap
    else if (elapsed < 300)
      setColor(r, g, b); // 2nd blink ON
    else if (elapsed < 500)
      setColor(0, 0, 0); // gap
    else {
      setColor(0, 0, 0);
      modeSwitchDone = true;
      // Return to idle state
      state = returnState;
      stateStart = millis();
    }
  }
};
