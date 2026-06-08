/*
 * api_manager.h  v3.1
 * Dual-mode API management: Live (WebSocket) + Single (REST)
 *
 * LIVE MODE:
 *   Primary:  gemini-3.1-flash-live-preview              (all keys)
 *   Fallback: gemini-2.5-flash-native-audio-preview-12-2025 (all keys)
 *   Reset → always returns to Primary
 *
 * SINGLE MODE:
 *   Model: gemini-2.5-flash (all keys)
 *   Key exhausted → same model, next key
 *   Question fail → rotate, user re-presses button
 *
 * rotate() returns true if ALL options exhausted (need cooldown)
 */

#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <vector>

#define MAX_API_KEYS 5

// ── Model Definitions ─────────────────────────────────────────
// Live mode models (WebSocket BidiGenerateContent)
static const char *LIVE_MODELS[] = {
    "gemini-3.1-flash-live-preview",                // Primary
    "gemini-2.5-flash-native-audio-preview-12-2025" // Fallback
};
static const int LIVE_MODEL_COUNT = 2;

// Single mode model (REST generateContent)
static const char *SINGLE_MODEL = "gemini-2.5-flash";

// ── Operation Mode ────────────────────────────────────────────
enum OpMode { MODE_LIVE, MODE_SINGLE };

class ApiManager {
public:
  void load(Preferences &p) {
    keys.clear();
    int n = p.getInt("key_count", 0);
    for (int i = 0; i < n && i < MAX_API_KEYS; i++) {
      String k = p.getString(("api_key_" + String(i)).c_str(), "");
      if (!k.isEmpty())
        keys.push_back(k);
    }
    keyIdx = 0;
    liveModelIdx = 0;
    opMode = MODE_LIVE; // Default: Live mode
    Serial.printf("[API] %d key(s) loaded\n", keys.size());
    Serial.printf("[API] Mode: LIVE | Model: %s\n", LIVE_MODELS[liveModelIdx]);
  }

  void save(Preferences &p, const std::vector<String> &newKeys) {
    keys = newKeys;
    p.putInt("key_count", (int)keys.size());
    for (int i = 0; i < (int)keys.size(); i++) {
      p.putString(("api_key_" + String(i)).c_str(), keys[i]);
    }
    keyIdx = liveModelIdx = 0;
  }

  // ── Mode Management ───────────────────────────────────────
  OpMode getMode() { return opMode; }
  bool isLiveMode() { return opMode == MODE_LIVE; }
  bool isSingleMode() { return opMode == MODE_SINGLE; }

  // Toggle between Live and Single mode
  void toggleMode() {
    if (opMode == MODE_LIVE) {
      opMode = MODE_SINGLE;
      Serial.printf("[API] → SINGLE MODE | Model: %s\n", SINGLE_MODEL);
    } else {
      opMode = MODE_LIVE;
      liveModelIdx = 0; // Reset → always return to Primary
      Serial.printf("[API] → LIVE MODE | Model: %s\n",
                    LIVE_MODELS[liveModelIdx]);
    }
    keyIdx = 0; // Reset key index on mode switch
  }

  // ── Key & Model Getters ───────────────────────────────────
  String getCurrentKey() { return keys.empty() ? "" : keys[keyIdx]; }
  bool hasKeys() { return !keys.empty(); }

  String getCurrentModel() {
    if (opMode == MODE_LIVE) {
      return String(LIVE_MODELS[liveModelIdx]);
    } else {
      return String(SINGLE_MODEL);
    }
  }

  // Get live model index (0 = primary, 1 = fallback)
  int getLiveModelIndex() { return liveModelIdx; }

  // ── Key Rotation ──────────────────────────────────────────
  // Returns true if ALL options exhausted (need cooldown)
  bool rotate() {
    if (keys.empty())
      return true;

    keyIdx++;
    if (keyIdx < (int)keys.size()) {
      Serial.printf("[API] → Key %d/%d\n", keyIdx + 1, (int)keys.size());
      return false;
    }

    // All keys tried for current model
    keyIdx = 0;

    if (opMode == MODE_LIVE) {
      // Try next Live model
      liveModelIdx++;
      if (liveModelIdx < LIVE_MODEL_COUNT) {
        Serial.printf("[API] → Fallback model: %s\n",
                      LIVE_MODELS[liveModelIdx]);
        return false;
      }
      // All Live models + keys exhausted
      liveModelIdx = 0;
      return true;
    } else {
      // Single mode: only one model, all keys tried
      return true;
    }
  }

  // Reset Live mode to primary model (called on reconnect/reset)
  void resetLiveToPrimary() {
    if (opMode == MODE_LIVE) {
      liveModelIdx = 0;
      keyIdx = 0;
      Serial.printf("[API] Reset → Primary: %s\n", LIVE_MODELS[0]);
    }
  }

  int keyCount() { return (int)keys.size(); }
  int keyIndex() { return keyIdx; }
  int modelIndex() { return opMode == MODE_LIVE ? liveModelIdx : 0; }

  // Get mode name for display
  const char *getModeName() { return opMode == MODE_LIVE ? "LIVE" : "SINGLE"; }

private:
  std::vector<String> keys;
  int keyIdx = 0;
  int liveModelIdx = 0;
  OpMode opMode = MODE_LIVE;
};
