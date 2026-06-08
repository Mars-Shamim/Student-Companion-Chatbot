/*
 * ============================================================
 *  AI Student Companion  v5.0 (Dual Mode)
 *  Hardware: ESP32-S3 N16R8 (16MB Flash / 8MB PSRAM)
 *    INMP441   — I2S Microphone
 *    MAX98357A — I2S Amplifier
 *    WS2812B   — RGB LED (Pin 48)
 *    Button 1 (GPIO 38)  — Talk/Stop | Long(5s)=Config
 *    Button 2 (GPIO 39) — Replay    | Long(3s)=Mode Switch
 *
 *  Button Logic:
 *    BTN1 short press  → Talk (idle) / Stop (listening) / Interrupt+Talk
 * (speaking) BTN1 5s hold      → Enter Config mode (WiFi AP) BTN2 short press
 * → Replay last response BTN2 3s hold      → Toggle Live ↔ Single mode
 *    Auto-sleep        → 1 min idle → sleep; BTN1 to wake
 * ============================================================
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <driver/i2s.h>
#include <mbedtls/base64.h>

#include "api_manager.h"
#include "config_server.h"
#include "led_controller.h"
#include "memory_tools.h"

// ── Display: uncomment next line when TFT is connected ───────
// #define DISPLAY_ENABLED
#ifdef DISPLAY_ENABLED
#include "display_manager.h"
#endif

// ── Pin Definitions ──────────────────────────────────────────
// INMP441 Microphone
#define MIC_WS_PIN 1  // WS  (LRCLK)
#define MIC_SCK_PIN 2 // SCK (BCLK)
#define MIC_SD_PIN 42 // SD  (DATA)
#define I2S_MIC_PORT I2S_NUM_0

// MAX98357A Amplifier
#define AMP_WS_PIN 46  // LRC
#define AMP_SCK_PIN 14 // BCLK
#define AMP_SD_PIN 41  // DIN
#define I2S_AMP_PORT I2S_NUM_1

// Buttons
#define BTN_TALK 38   // Changed from 0 to 38 for testing
#define BTN_REPLAY 39 // GPIO 39 (SD card D1 pin, unused) — Replay / Mode Switch

// ── Audio Config ─────────────────────────────────────────────
#define SAMPLE_RATE_MIC 16000
#define SAMPLE_RATE_AMP 24000
#define DMA_BUF_COUNT 8
#define DMA_BUF_LEN 512
#define MIC_SAMPLES 512

// Buffers Size Configuration
#define WS_CHUNK_BYTES 1024
#define OUT_BUF_SIZE (64 * 1024) // 64KB live playback ring buffer (PSRAM)
#define REPLAY_BUF_SIZE                                                        \
  (4 * 1024 * 1024) // 4MB replay store (~1 Min 27 Sec in PSRAM)
#define RECORD_BUF_SIZE                                                        \
  (16000 * 2 * 15) // 15 sec max record buffer (480KB in PSRAM)
#define JSON_RX_SIZE (48 * 1024)

// Timing
#define MAX_LISTEN_MS 45000UL
#define COOLDOWN_MS 60000UL
#define AUTO_SLEEP_MS 60000UL // 1 min idle → auto sleep

// ── State Machine ─────────────────────────────────────────────
enum State {
  S_IDLE,
  S_CONNECTING,
  S_LISTENING,
  S_PROCESSING,
  S_SPEAKING,
  S_REPLAYING,
  S_CONFIG,
  S_COOLDOWN,
  S_SLEEP // Low-power / quiet sleep state
};

// ── Globals ───────────────────────────────────────────────────
State appState = S_IDLE;
Preferences prefs;
ConfigServer configServer;
ApiManager apiManager;
MemoryTools memoryTools;
LedController led;
#ifdef DISPLAY_ENABLED
DisplayManager display;
#endif
WebSocketsClient wsClient;

bool wsConnected = false;
bool sessionReady = false;
bool turnComplete = false; // set when Gemini signals turn_complete

// Mic pipeline
int32_t micRaw[MIC_SAMPLES];
uint8_t wsBuf[WS_CHUNK_BYTES];
int wsBufPos = 0;

// Dynamic Buffers for PSRAM (Pointers instead of fixed arrays)
uint8_t *outRing = NULL;
uint8_t *replayBuf = NULL;
uint8_t *recordBuf = NULL;

volatile int outHead = 0;
volatile int outTail = 0;
int replayLen = 0; // bytes of valid data
int replayPos = 0; // read position during replay
int recordLen = 0; // recorded bytes in SINGLE mode
uint32_t silenceStartMs = 0;

// Accumulated text from Gemini (for display + Sheet save)
String currentText = "";
String lastSavedSubject = "";
String lastSavedIndex = "";

// Timing
unsigned long listenStartTime = 0;
unsigned long cooldownStart = 0;
unsigned long lastActivityTime = 0; // for auto-sleep

TaskHandle_t ledTaskHandle;

// Forward declarations
void wsEventHandler(WStype_t, uint8_t *, size_t);
bool connectToGemini();
void sendSetupMessage();
void sendAudioChunk(const uint8_t *, int);
void sendEndOfTurn();
void handleWsMessage(uint8_t *, size_t);
void appendToReplayBuf(const uint8_t *, size_t);
void startReplay();
void drainOutputBuffer();
void drainReplayBuffer();
void setupMicI2S();
void setupAmpI2S();
void updateIdleLed();
void interruptAI();
void handleButtons();
void switchMode();
bool connectWiFi();
void enterConfigMode();
void readMicAndSend();
void stopTalking();
void startTalking();
void rotateAndReconnect();
void writeToRing(const uint8_t *data, size_t len);
void performSingleModeRequest();
void playBeep(int freqHz, int durationMs, float volume = 0.3f);
void playStateSound(State s);

// ── LED Task ──────────────────────────────────────────────────
void ledTask(void *pvParameters) {
  for (;;) {
    led.update();
    vTaskDelay(pdMS_TO_TICKS(16)); // ~60fps smooth animation
  }
}

// ── setup() ───────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== AI Student Assistant v4.0 (Dual Mode) ===");

  // ── PSRAM Initialization & Allocation ──
  if (!psramInit()) {
    Serial.println("[ERROR] PSRAM Initialization Failed! Check IDE Settings.");
  } else {
    Serial.printf("[PSRAM] Total Size: %d KB\n", ESP.getPsramSize() / 1024);

    // Allocate large buffers in PSRAM
    outRing = (uint8_t *)ps_malloc(OUT_BUF_SIZE);
    replayBuf = (uint8_t *)ps_malloc(REPLAY_BUF_SIZE);
    recordBuf = (uint8_t *)ps_malloc(RECORD_BUF_SIZE);

    if (outRing == NULL || replayBuf == NULL || recordBuf == NULL) {
      Serial.println("[ERROR] PSRAM Buffer Allocation Failed!");
    } else {
      Serial.println("[PSRAM] Buffers Successfully Allocated ✓");
    }
  }

  pinMode(BTN_TALK, INPUT_PULLUP);
  // GPIO 39 = SD card D1 pin — safe to use as input when SD is not connected
  pinMode(BTN_REPLAY, INPUT_PULLUP);

  led.begin();
  xTaskCreatePinnedToCore(ledTask, "LED_Task", 2048, NULL, 1, &ledTaskHandle,
                          1);
  led.setState(LED_CONNECTING);

#ifdef DISPLAY_ENABLED
  display.begin();
#endif

  // Initialize I2S before any sounds can play
  setupMicI2S();
  setupAmpI2S();

  // Load config
  prefs.begin("ai-cfg", false);
  configServer.load(prefs);
  apiManager.load(prefs);
  memoryTools.load(prefs);

  wsClient.onEvent(wsEventHandler);

  // WiFi Connection
  if (configServer.getSSID().isEmpty()) {
    Serial.println("[SETUP] SSID is empty -> Config Mode");
    enterConfigMode();
    return;
  }
#ifdef DISPLAY_ENABLED
  display.showConnecting(configServer.getSSID().c_str());
#endif
  if (!connectWiFi()) {
    Serial.println("[SETUP] WiFi failed to connect -> Config Mode");
    enterConfigMode();
    return;
  }
#ifdef DISPLAY_ENABLED
  display.showConnected(WiFi.localIP().toString().c_str());
#endif
  // WiFi connected beep: ascending 3-tone
  playBeep(800, 80);
  delay(60);
  playBeep(1200, 80);
  delay(60);
  playBeep(1600, 120);
  delay(400);
  lastActivityTime = millis();

#ifdef DISPLAY_ENABLED
  display.showIdle();
#endif

  updateIdleLed();
  Serial.println("[READY] BTN1: 1x=Talk/Stop | 2x=Sleep/Wake | 3x=Config");
  Serial.println("[READY] BTN2: 1x=Replay    | 3x=Mode Switch (Live<->Single)");
}

// ── loop() ────────────────────────────────────────────────────
void loop() {
  if (appState == S_CONFIG) {
    configServer.handle();
    return;
  }
  if (appState == S_COOLDOWN) {
    handleButtons(); // ← বাটন কাজ করবে cooldown এ (mode switch, config)
    if (millis() - cooldownStart >= COOLDOWN_MS) {
      appState = S_IDLE;
      lastActivityTime = millis();
      updateIdleLed();
#ifdef DISPLAY_ENABLED
      display.showIdle();
#endif
    }
    return;
  }
  // In sleep mode: only poll buttons so BTN1 can wake the device
  if (appState == S_SLEEP) {
    handleButtons();
    delay(20);
    return;
  }

  // ── Auto-sleep: 1 minute idle ─────────────────────────────
  if (appState == S_IDLE && (millis() - lastActivityTime) >= AUTO_SLEEP_MS) {
    Serial.println("[AUTO] 1 min idle → sleep");
    enterSleep();
    return;
  }

  if (apiManager.isLiveMode()) {
    wsClient.loop();
  }

  handleButtons();

  switch (appState) {
  case S_LISTENING:
    readMicAndSend();
#ifdef DISPLAY_ENABLED
    display.updateListeningAnim();
#endif
    if (millis() - listenStartTime >= MAX_LISTEN_MS) {
      Serial.println("[MIC] Max time reached — auto stop");
      stopTalking();
    }
    break;

  case S_SPEAKING:
    drainOutputBuffer();
    if (outHead == outTail && turnComplete) {
      appState = S_IDLE;
      lastActivityTime = millis(); // reset sleep timer after response
      updateIdleLed();
#ifdef DISPLAY_ENABLED
      display.showIdle();
#endif
      Serial.println("[AMP] Playback complete");
    }
    break;

  case S_REPLAYING:
    drainReplayBuffer();
    if (replayPos >= replayLen) {
      appState = S_IDLE;
      updateIdleLed();
#ifdef DISPLAY_ENABLED
      if (!currentText.isEmpty())
        display.showSpeaking(currentText);
      else
        display.showIdle();
#endif
    }
    break;

  default:
    break;
  }
}

// ── LED Helpers ───────────────────────────────────────────────
void updateIdleLed() {
  if (apiManager.isLiveMode()) {
    if (apiManager.getLiveModelIndex() == 0)
      led.setIdleState(LED_IDLE_LIVE_PRIMARY);
    else
      led.setIdleState(LED_IDLE_LIVE_FALLBACK);
  } else {
    led.setIdleState(LED_IDLE_SINGLE);
  }

  // Set immediately to idle state if not currently playing an animation
  if (appState == S_IDLE) {
    if (apiManager.isLiveMode()) {
      if (apiManager.getLiveModelIndex() == 0)
        led.setState(LED_IDLE_LIVE_PRIMARY);
      else
        led.setState(LED_IDLE_LIVE_FALLBACK);
    } else {
      led.setState(LED_IDLE_SINGLE);
    }
  }
}

// ── WiFi ──────────────────────────────────────────────────────
bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(configServer.getSSID().c_str(),
             configServer.getPassword().c_str());
  Serial.printf("[WiFi] Connecting to '%s'", configServer.getSSID().c_str());
  uint32_t t = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t > 15000) {
      Serial.println(" TIMEOUT");
      return false;
    }
    delay(300);
    Serial.print(".");
  }
  Serial.printf(" OK  IP: %s\n", WiFi.localIP().toString().c_str());
  return true;
}

void enterConfigMode() {
  Serial.println("[CONFIG] Entering config mode...");
  playBeep(1800, 80);
  delay(60);
  playBeep(1800, 80);
  delay(60);
  playBeep(2400, 120);
  appState = S_CONFIG;
  WiFi.disconnect(true); // disconnect STA so AP appears on phone
  delay(200);
  configServer.startAP();
  configServer.startServer();
#ifdef DISPLAY_ENABLED
  display.showConfig();
#endif
  led.setState(LED_ERROR);
  Serial.println(
      "[CONFIG] AP: AI-Student-Config | Pass: student123 | IP: 192.168.4.1");
}

// ── I2S ───────────────────────────────────────────────────────
void setupMicI2S() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = SAMPLE_RATE_MIC;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = DMA_BUF_COUNT;
  cfg.dma_buf_len = DMA_BUF_LEN;
  i2s_pin_config_t pins = {};
  pins.bck_io_num = MIC_SCK_PIN;
  pins.ws_io_num = MIC_WS_PIN;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = MIC_SD_PIN;
  ESP_ERROR_CHECK(i2s_driver_install(I2S_MIC_PORT, &cfg, 0, NULL));
  ESP_ERROR_CHECK(i2s_set_pin(I2S_MIC_PORT, &pins));
}

void setupAmpI2S() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = SAMPLE_RATE_AMP;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = DMA_BUF_COUNT;
  cfg.dma_buf_len = DMA_BUF_LEN;
  cfg.tx_desc_auto_clear = true;
  i2s_pin_config_t pins = {};
  pins.bck_io_num = AMP_SCK_PIN;
  pins.ws_io_num = AMP_WS_PIN;
  pins.data_out_num = AMP_SD_PIN;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  ESP_ERROR_CHECK(i2s_driver_install(I2S_AMP_PORT, &cfg, 0, NULL));
  ESP_ERROR_CHECK(i2s_set_pin(I2S_AMP_PORT, &pins));
}

// ── Sleep helpers ─────────────────────────────────────────
void enterSleep() {
  if (appState != S_SLEEP)
    interruptAI();
  appState = S_SLEEP;
  led.setState(LED_OFF);
  playBeep(700, 100);
  delay(60);
  playBeep(500, 100);
  delay(60);
  playBeep(300, 180);
  Serial.println("[SLEEP] Sleeping. BTN1 to wake.");
#ifdef DISPLAY_ENABLED
  display.showIdle();
#endif
}

void wakeUp() {
  appState = S_IDLE;
  lastActivityTime = millis();
  updateIdleLed();
  playBeep(800, 80);
  delay(50);
  playBeep(1200, 100);
  Serial.println("[WAKE] Awake.");
#ifdef DISPLAY_ENABLED
  display.showIdle();
#endif
}

void switchMode() {
  interruptAI();
  apiManager.toggleMode();

  // ── returnState আগে সেট করতে হবে, তাহলে animation শেষে সঠিক রঙে ফিরবে ──
  LedState targetIdle;
  if (apiManager.isLiveMode()) {
    targetIdle = (apiManager.getLiveModelIndex() == 0) ? LED_IDLE_LIVE_PRIMARY
                                                       : LED_IDLE_LIVE_FALLBACK;
  } else {
    targetIdle = LED_IDLE_SINGLE; // Green
  }
  led.setIdleState(targetIdle);  // doubleBlink শেষে এখানে ফিরবে
  led.setState(LED_MODE_SWITCH); // White double blink শুরু
  lastActivityTime = millis();

  if (apiManager.isLiveMode()) {
    playBeep(1000, 80);
    delay(50);
    playBeep(1600, 120);
    Serial.println("[MODE] → LIVE");
  } else {
    playBeep(1600, 80);
    delay(50);
    playBeep(900, 120);
    Serial.println("[MODE] → SINGLE");
  }
  appState = S_IDLE;
#ifdef DISPLAY_ENABLED
  display.showIdle();
#endif
}

/*
 * handleButtons()  v5.0
 * ─────────────────────────────────────────────────────────
 * BTN1 (GPIO 38, active LOW):
 *   Short press  → Talk (idle) | Stop (listening) | Interrupt+Talk (busy)
 *                  Wake (sleep)
 *   Hold 5 sec   → Enter Config mode (WiFi disconnects, AP starts)
 *
 * BTN2 (GPIO 39, active LOW):
 *   Short press  → Replay last AI response
 *   Hold 3 sec   → Toggle Live ↔ Single mode
 * ─────────────────────────────────────────────────────────
 */
void handleButtons() {
  // ── Startup guard: ignore first 2s to prevent false triggers ──
  static bool btnReady = false;
  if (!btnReady) {
    if (millis() < 2000)
      return;
    btnReady = true;
    Serial.println("[BTN] Button handler active.");
  }

  static bool btn1Last = HIGH;
  static bool btn2Last = HIGH;
  static uint32_t btn1DownAt =
      0; // set on falling edge only (never 0 after first press)
  static uint32_t btn2DownAt = 0;
  static bool btn1LongFired = false;
  static bool btn2LongFired = false;

  const uint32_t BTN1_LONG_MS = 5000; // 5s → config
  const uint32_t BTN2_LONG_MS = 3000; // 3s → mode switch

  bool btn1 = digitalRead(BTN_TALK);
  bool btn2 = digitalRead(BTN_REPLAY);
  uint32_t now = millis();

  // ── BTN1 falling edge (pressed down) ──────────────────────
  if (btn1 == LOW && btn1Last == HIGH) {
    btn1DownAt = now;
    btn1LongFired = false;
  }

  // ── BTN1 long-press trigger (while held) ──────────────────
  if (btn1 == LOW && !btn1LongFired && btn1DownAt > 0 &&
      (now - btn1DownAt) >= BTN1_LONG_MS) {
    btn1LongFired = true;
    Serial.println("[BTN] BTN1 held 5s -> Config Mode");
    enterConfigMode();
  }

  // ── BTN1 rising edge (released) ───────────────────────────
  if (btn1 == HIGH && btn1Last == LOW) {
    if (!btn1LongFired) {
      // Short press action
      if (appState == S_SLEEP) {
        wakeUp();
      } else if (appState == S_IDLE) {
        startTalking();
      } else if (appState == S_LISTENING) {
        stopTalking();
      } else if (appState == S_SPEAKING || appState == S_PROCESSING ||
                 appState == S_REPLAYING) {
        interruptAI();
        startTalking();
      }
    }
    btn1LongFired = false;
  }

  // ── BTN2 falling edge (pressed down) ──────────────────────
  if (btn2 == LOW && btn2Last == HIGH) {
    btn2DownAt = now;
    btn2LongFired = false;
  }

  // ── BTN2 long-press trigger (while held) ──────────────────
  if (btn2 == LOW && !btn2LongFired && appState != S_SLEEP &&
      (now - btn2DownAt) >= BTN2_LONG_MS) {
    btn2LongFired = true;
    switchMode();
  }

  // ── BTN2 rising edge (released) ───────────────────────────
  if (btn2 == HIGH && btn2Last == LOW) {
    if (!btn2LongFired && appState != S_SLEEP) {
      // Short press → Replay
      if (appState == S_SPEAKING || appState == S_PROCESSING) {
        // ignore while active
      } else if (replayLen > 0) {
        startReplay();
      } else {
        playBeep(400, 80);
        Serial.println("[Replay] Nothing to replay yet");
      }
    }
    btn2LongFired = false;
  }

  btn1Last = btn1;
  btn2Last = btn2;
}

void interruptAI() {
  outHead = outTail = 0;
  i2s_zero_dma_buffer(I2S_AMP_PORT);
  appState = S_IDLE;
  updateIdleLed();
  playBeep(600, 80); // low blip = interrupted
  Serial.println("[BTN] AI interrupted");
}

// ── Talk control ──────────────────────────────────────────────
void startTalking() {
  if (!apiManager.hasKeys()) {
#ifdef DISPLAY_ENABLED
    display.showError("No API keys!\nOpen config page");
#endif
    led.setState(LED_ERROR);
    delay(1000);
    updateIdleLed();
    return;
  }

  outHead = outTail = 0;
  i2s_zero_dma_buffer(I2S_AMP_PORT);

  // Clean text and replay memories on new session
  currentText = "";
  replayLen = 0;
  replayPos = 0;
  turnComplete = false;
  recordLen = 0;
  silenceStartMs = 0;

  if (apiManager.isLiveMode()) {
    if (!wsConnected || !sessionReady) {
      appState = S_CONNECTING;
      led.setState(LED_CONNECTING);
#ifdef DISPLAY_ENABLED
      display.showProcessing();
#endif
      if (!connectToGemini()) {
#ifdef DISPLAY_ENABLED
        display.showError("Connection failed");
        delay(2000);
        display.showIdle();
#endif
        appState = S_IDLE;
        updateIdleLed();
        return;
      }
    }
    wsBufPos = 0;
  }

  listenStartTime = millis();
  lastActivityTime = millis(); // reset sleep timer on activity
  appState = S_LISTENING;
  led.setState(LED_LISTENING);
  playBeep(1200, 80); // ascending blip = mic on
#ifdef DISPLAY_ENABLED
  display.showListening();
#endif
  Serial.printf("[MIC] Recording (%s Mode)...\n", apiManager.getModeName());
}

void stopTalking() {
  if (apiManager.isLiveMode()) {
    if (wsBufPos > 0) {
      sendAudioChunk(wsBuf, wsBufPos);
      wsBufPos = 0;
    }
    sendEndOfTurn();
    appState = S_PROCESSING;
    led.setState(LED_CONNECTING);
    playBeep(800, 60); // descending blip = mic off
#ifdef DISPLAY_ENABLED
    display.showProcessing();
#endif
    Serial.println("[MIC] Done — waiting for response");
  } else {
    appState = S_PROCESSING;
    led.setState(LED_CONNECTING);
    playBeep(800, 60);
#ifdef DISPLAY_ENABLED
    display.showProcessing();
#endif
    Serial.println("[MIC] Processing Single Mode recording...");
    performSingleModeRequest();
  }
}

// ── Replay ────────────────────────────────────────────────────
void startReplay() {
  if (replayLen == 0 || replayBuf == NULL)
    return;
  replayPos = 0;
  appState = S_REPLAYING;
  led.setState(LED_SPEAKING);
  playBeep(900, 60);
  delay(40);
  playBeep(1200, 80); // 2-tone replay cue
#ifdef DISPLAY_ENABLED
  display.showReplay();
  if (!currentText.isEmpty())
    display.showSpeaking(currentText);
#endif
  Serial.printf("[Replay] Playing %d bytes from PSRAM\n", replayLen);
}

void appendToReplayBuf(const uint8_t *data, size_t len) {
  if (replayBuf == NULL)
    return;
  if (replayLen + (int)len > REPLAY_BUF_SIZE) {
    Serial.println(
        "[Replay Store] Buffer full, stopping recording for this turn.");
    return;
  }
  memcpy(replayBuf + replayLen, data, len);
  replayLen += len;
}

// ── Mic read ─────────────────────────────────────────────────
void readMicAndSend() {
  size_t bytesRead = 0;
  i2s_read(I2S_MIC_PORT, micRaw, sizeof(int32_t) * MIC_SAMPLES, &bytesRead,
           pdMS_TO_TICKS(5));
  if (!bytesRead)
    return;

  int samples = bytesRead / sizeof(int32_t);
  int16_t pcm16[MIC_SAMPLES];
  uint64_t sumSq = 0;

  for (int i = 0; i < samples; i++) {
    int32_t s = micRaw[i] >> 14;
    pcm16[i] = (int16_t)constrain(s, -32768, 32767);
    if (apiManager.isSingleMode()) {
      sumSq += pcm16[i] * pcm16[i];
    }
  }

  uint8_t *src = (uint8_t *)pcm16;
  int bytes = samples * 2;

  if (apiManager.isLiveMode()) {
    while (bytes > 0) {
      int space = WS_CHUNK_BYTES - wsBufPos;
      int copy = min(bytes, space);
      memcpy(wsBuf + wsBufPos, src, copy);
      wsBufPos += copy;
      src += copy;
      bytes -= copy;
      if (wsBufPos >= WS_CHUNK_BYTES) {
        sendAudioChunk(wsBuf, WS_CHUNK_BYTES);
        wsBufPos = 0;
      }
    }
  } else {
    // SINGLE Mode recording
    if (recordBuf && recordLen + bytes <= RECORD_BUF_SIZE) {
      memcpy(recordBuf + recordLen, src, bytes);
      recordLen += bytes;
    }

    uint16_t rms = sqrt(sumSq / samples);
    if (rms < 300) { // Silence Threshold
      if (silenceStartMs == 0) {
        silenceStartMs = millis();
      } else if (millis() - silenceStartMs > 1200 &&
                 recordLen > SAMPLE_RATE_MIC * 2) {
        Serial.println("[MIC] Silence detected - Auto Stop");
        stopTalking();
      }
    } else {
      silenceStartMs = 0;
    }

    if (recordLen >= RECORD_BUF_SIZE) {
      Serial.println("[MIC] Max record buffer reached - Auto Stop");
      stopTalking();
    }
  }
}

// ── Audio output ──────────────────────────────────────────────
void writeToRing(const uint8_t *data, size_t len) {
  if (outRing == NULL)
    return;
  for (size_t i = 0; i < len; i++) {
    int next = (outHead + 1) % OUT_BUF_SIZE;
    if (next == outTail)
      break;
    outRing[outHead] = data[i];
    outHead = next;
  }
}

void drainOutputBuffer() {
  if (outRing == NULL)
    return;
  int avail = (outHead - outTail + OUT_BUF_SIZE) % OUT_BUF_SIZE;
  if (!avail)
    return;
  int toWrite = min(avail, 1024);
  uint8_t tmp[1024];
  int c1 = min(toWrite, OUT_BUF_SIZE - outTail);
  memcpy(tmp, outRing + outTail, c1);
  if (c1 < toWrite)
    memcpy(tmp + c1, outRing, toWrite - c1);
  outTail = (outTail + toWrite) % OUT_BUF_SIZE;
  size_t written = 0;
  i2s_write(I2S_AMP_PORT, tmp, toWrite, &written, pdMS_TO_TICKS(5));
}

void drainReplayBuffer() {
  if (replayBuf == NULL)
    return;
  int avail = replayLen - replayPos;
  if (avail <= 0)
    return;
  int toWrite = min(avail, 1024);
  size_t written = 0;
  i2s_write(I2S_AMP_PORT, replayBuf + replayPos, toWrite, &written,
            pdMS_TO_TICKS(5));
  replayPos += toWrite;
}

// ── Beep tones via I2S speaker ────────────────────────────────
void playBeep(int freqHz, int durationMs, float volume) {
  const int SR = SAMPLE_RATE_AMP;
  int totalSamples = (SR * durationMs) / 1000;
  const int CHUNK = 256;
  int16_t buf[CHUNK];
  size_t bw = 0;
  float rampSamples = SR * 0.008f; // 8ms fade
  for (int i = 0; i < totalSamples; i += CHUNK) {
    int n = min(CHUNK, totalSamples - i);
    for (int j = 0; j < n; j++) {
      int idx = i + j;
      float t = (float)idx / SR;
      float env = 1.0f;
      if (idx < (int)rampSamples)
        env = idx / rampSamples;
      else if (idx > totalSamples - (int)rampSamples)
        env = (totalSamples - idx) / rampSamples;
      buf[j] =
          (int16_t)(sinf(2.0f * PI * freqHz * t) * 32767.0f * volume * env);
    }
    i2s_write(I2S_AMP_PORT, buf, n * sizeof(int16_t), &bw, pdMS_TO_TICKS(50));
  }
}

// ── SINGLE MODE REST Request ──────────────────────────────────
void performSingleModeRequest() {
  // ── Single attempt only — loop নেই ──────────────────────────
  // 429/error হলে rotate() করে IDLE এ ফিরবে।
  // User আবার button press করলে পরের key দিয়ে try হবে।
  // এতে একটা question এ সর্বোচ্চ ১টাই request যাবে।

  String key = apiManager.getCurrentKey();
  if (key.isEmpty()) {
    Serial.println("[Single] No keys available!");
    appState = S_IDLE;
    updateIdleLed();
    return;
  }

  String url = "https://generativelanguage.googleapis.com/v1beta/models/" +
               apiManager.getCurrentModel() + ":generateContent?key=" + key;

  size_t b64Size = ((recordLen + 2) / 3) * 4 + 1;
  char *b64 = (char *)ps_malloc(b64Size);
  if (!b64) {
    Serial.println("[Single] Failed to allocate b64 buffer");
    appState = S_IDLE;
    updateIdleLed();
    return;
  }
  size_t b64Len = 0;
  mbedtls_base64_encode((uint8_t *)b64, b64Size, &b64Len, recordBuf, recordLen);
  b64[b64Len] = '\0';

  String payload;
  {
    JsonDocument doc;
    String si = configServer.getSystemInstruction();
    if (si.length() > 0) {
      doc["system_instruction"]
          .createNestedArray("parts")
          .createNestedObject()["text"] = si;
    }

    JsonArray tools = doc["tools"].to<JsonArray>();
    tools.createNestedObject().createNestedObject("googleSearch");
    if (memoryTools.isEnabled()) {
      memoryTools.addToolDefinitions(tools);
    }

    doc["generationConfig"]["responseModalities"][0] = "AUDIO";
    doc["generationConfig"]["responseModalities"][1] = "TEXT";
    doc["generationConfig"]["speechConfig"]["voiceConfig"]
       ["prebuiltVoiceConfig"]["voiceName"] = configServer.getVoice();
    serializeJson(doc, payload);
  }
  payload.remove(payload.length() - 1);
  payload += ",\"contents\":[{\"parts\":[{\"inlineData\":{\"mimeType\":"
             "\"audio/pcm;rate=16000\",\"data\":\"";
  payload += b64;
  payload += "\"}}]}]}";
  free(b64);

  Serial.printf("[Single] POST → Key %d | %s\n", apiManager.keyIndex() + 1,
                apiManager.getCurrentModel().c_str());

  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(45000);
  int code = http.POST(payload);

  if (code == 200) {
    WiFiClient *stream = http.getStreamPtr();
    JsonDocument filter;
    filter["candidates"][0]["content"]["parts"][0]["text"] = true;
    filter["candidates"][0]["content"]["parts"][0]["inlineData"]["data"] = true;
    filter["candidates"][0]["content"]["parts"][0]["functionCall"] = true;

    JsonDocument doc;
    DeserializationError err =
        deserializeJson(doc, *stream, DeserializationOption::Filter(filter));
    http.end();

    if (!err) {
      JsonVariant parts = doc["candidates"][0]["content"]["parts"];
      for (JsonVariant part : parts.as<JsonArray>()) {
        if (!part["functionCall"].isNull()) {
          String toolName = part["functionCall"]["name"].as<String>();
          JsonObject args = part["functionCall"]["args"].as<JsonObject>();
          auto result = memoryTools.handleToolCall(toolName, args);
          if (result.didSave) {
            lastSavedSubject = result.savedSubject;
            lastSavedIndex = result.savedIndex;
#ifdef DISPLAY_ENABLED
            display.showSaved(result.savedSubject, result.savedIndex);
#endif
            delay(1500);
          }
        }
        if (!part["text"].isNull()) {
          currentText += part["text"].as<String>();
          Serial.printf("[Text] %s\n", part["text"].as<String>().c_str());
#ifdef DISPLAY_ENABLED
          display.showSpeaking(currentText);
#endif
        }
        if (!part["inlineData"]["data"].isNull()) {
          const char *audioB64 = part["inlineData"]["data"];
          size_t bLen = strlen(audioB64);
          size_t outSize = (bLen * 3) / 4 + 8;
          uint8_t *pcm = (uint8_t *)ps_malloc(outSize);
          if (pcm) {
            size_t decoded = 0;
            mbedtls_base64_decode(pcm, outSize, &decoded,
                                  (const uint8_t *)audioB64, bLen);
            memcpy(replayBuf, pcm, decoded);
            replayLen = decoded;
            free(pcm);
            appState = S_REPLAYING;
            replayPos = 0;
            led.setState(LED_SPEAKING);
            return;
          }
        }
      }
    } else {
      Serial.printf("[Single] Parse error: %s\n", err.c_str());
    }

    appState = S_IDLE;
    updateIdleLed();

  } else {
    http.end();
    Serial.printf("[Single] HTTP %d (Key %d) → rotate\n", code,
                  apiManager.keyIndex() + 1);

    if (apiManager.rotate()) {
      // সব key শেষ → cooldown
      Serial.println("[Single] All keys exhausted → cooldown");
      appState = S_COOLDOWN;
      cooldownStart = millis();
      led.setState(LED_ERROR);
    } else {
      // পরের key আছে → IDLE, user আবার press করলে সেই key দিয়ে try হবে
      Serial.printf(
          "[Single] Next key ready (Key %d) → press button to retry\n",
          apiManager.keyIndex() + 1);
      appState = S_IDLE;
      updateIdleLed();
      playBeep(500, 80); // নিচু beep = retry দরকার
    }
  }
}

// ── WebSocket (Live Mode) ─────────────────────────────────────
bool connectToGemini() {
  String key = apiManager.getCurrentKey();
  if (key.isEmpty())
    return false;

  // reset to primary on successful connect intent
  apiManager.resetLiveToPrimary();

  String path = "/ws/google.ai.generativelanguage.v1beta.GenerativeService"
                ".BidiGenerateContent?key=" +
                key;
  wsConnected = sessionReady = false;
  wsClient.beginSSL("generativelanguage.googleapis.com", 443, path.c_str(), "",
                    "");
  uint32_t t = millis();
  while (!wsConnected && millis() - t < 10000) {
    wsClient.loop();
    delay(10);
  }
  if (!wsConnected)
    return false;
  sendSetupMessage();
  t = millis();
  while (!sessionReady && millis() - t < 8000) {
    wsClient.loop();
    delay(10);
  }
  return sessionReady;
}

void sendSetupMessage() {
  DynamicJsonDocument doc(8192);
  JsonObject setup = doc.createNestedObject("setup");
  setup["model"] = "models/" + apiManager.getCurrentModel();

  String si = configServer.getSystemInstruction();
  if (!si.isEmpty()) {
    setup["systemInstruction"]
        .createNestedArray("parts")
        .createNestedObject()["text"] = si;
  }

  JsonObject gc = setup.createNestedObject("generationConfig");
  JsonArray mods = gc.createNestedArray("responseModalities");
  mods.add("AUDIO");
  mods.add("TEXT");
  gc["speechConfig"]["voiceConfig"]["prebuiltVoiceConfig"]["voiceName"] =
      configServer.getVoice();

  // Live API তে googleSearch tool দিলে server সাথে সাথে disconnect করে।
  // শুধু memory tools পাঠাও (enabled থাকলে)।
  if (memoryTools.isEnabled()) {
    JsonArray tools = setup.createNestedArray("tools");
    memoryTools.addToolDefinitions(tools);
  }

  String out;
  serializeJson(doc, out);
  Serial.printf("[WS] Sending Setup: %s\n", out.c_str());
  wsClient.sendTXT(out);
  Serial.printf("[WS] Setup sent (model: %s)\n",
                apiManager.getCurrentModel().c_str());
}

void sendAudioChunk(const uint8_t *data, int len) {
  if (!wsConnected || !sessionReady)
    return;
  size_t b64Size = ((len + 2) / 3) * 4 + 1;
  uint8_t *b64 = (uint8_t *)malloc(b64Size);
  if (!b64)
    return;
  size_t b64Len = 0;
  mbedtls_base64_encode(b64, b64Size, &b64Len, data, len);
  b64[b64Len] = '\0';
  DynamicJsonDocument doc(b64Len + 256);
  JsonObject chunk = doc.createNestedObject("realtimeInput")
                         .createNestedArray("mediaChunks")
                         .createNestedObject();
  chunk["mimeType"] = "audio/pcm;rate=16000";
  chunk["data"] = (const char *)b64;
  String out;
  serializeJson(doc, out);
  wsClient.sendTXT(out);
  free(b64);
}

void sendEndOfTurn() {
  if (!wsConnected)
    return;
  wsClient.sendTXT("{\"clientContent\":{\"turnComplete\":true}}");
}

void wsEventHandler(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
  case WStype_CONNECTED:
    wsConnected = true;
    sessionReady = false;
    Serial.println("[WS] Connected");
    break;
  case WStype_DISCONNECTED:
    wsConnected = sessionReady = false;
    Serial.println("[WS] Disconnected");
    if (appState == S_PROCESSING || appState == S_LISTENING) {
      appState = S_IDLE;
      updateIdleLed();
#ifdef DISPLAY_ENABLED
      display.showIdle();
#endif
    }
    break;
  case WStype_TEXT:
    handleWsMessage(payload, length);
    break;
  case WStype_ERROR:
    Serial.println("[WS] Error → rotating");
    rotateAndReconnect();
    break;
  default:
    break;
  }
}

void handleWsMessage(uint8_t *payload, size_t length) {
  static DynamicJsonDocument doc(JSON_RX_SIZE);
  doc.clear();

  if (deserializeJson(doc, payload, length))
    return;

  if (doc.containsKey("setupComplete")) {
    sessionReady = true;
    Serial.println("[Gemini] Session ready ✓");
    return;
  }

  if (doc.containsKey("toolCall")) {
    JsonArray calls = doc["toolCall"]["functionCalls"];
    for (JsonObject call : calls) {
      String toolName = call["name"].as<String>();
      JsonObject args = call["args"].as<JsonObject>();

      auto result = memoryTools.handleToolCall(toolName, args);
      wsClient.sendTXT(result.wsMessage);

      if (result.didSave) {
#ifdef DISPLAY_ENABLED
        display.showSaved(result.savedSubject, result.savedIndex);
#endif
        lastSavedSubject = result.savedSubject;
        lastSavedIndex = result.savedIndex;
        delay(1500);
#ifdef DISPLAY_ENABLED
        if (!currentText.isEmpty())
          display.showSpeaking(currentText);
#endif
      }
    }
    return;
  }

  if (doc.containsKey("serverContent")) {
    JsonObject sc = doc["serverContent"];

    if (sc.containsKey("modelTurn")) {
      JsonArray parts = sc["modelTurn"]["parts"];
      for (JsonObject part : parts) {
        if (part.containsKey("inlineData")) {
          const char *b64 = part["inlineData"]["data"];
          if (b64) {
            size_t b64Len = strlen(b64);
            size_t outSize = (b64Len * 3) / 4 + 8;
            uint8_t *pcm = (uint8_t *)malloc(outSize);
            if (pcm) {
              size_t decoded = 0;
              mbedtls_base64_decode(pcm, outSize, &decoded,
                                    (const uint8_t *)b64, b64Len);
              writeToRing(pcm, decoded);
              appendToReplayBuf(pcm, decoded);
              free(pcm);
              if (appState == S_PROCESSING) {
                appState = S_SPEAKING;
                led.setState(LED_SPEAKING);
                Serial.println("[AMP] Playing...");
              }
            }
          }
        }

        if (part.containsKey("text")) {
          const char *txt = part["text"];
          if (txt && strlen(txt) > 0) {
            currentText += String(txt);
#ifdef DISPLAY_ENABLED
            display.showSpeaking(currentText);
#endif
            Serial.printf("[Text] %s\n", txt);
          }
        }
      }
    }

    if (sc["turnComplete"].as<bool>()) {
      turnComplete = true;
      Serial.println("[Gemini] Turn complete");
      if (appState == S_PROCESSING) {
        appState = S_IDLE;
        updateIdleLed();
      }
    }
  }

  if (doc.containsKey("error")) {
    int code = doc["error"]["code"] | 0;
    Serial.printf("[API ERR] %d\n", code);
#ifdef DISPLAY_ENABLED
    display.showError("API Error " + String(code));
#endif
    if (code == 429 || code == 403 || code == 401 || code == 500)
      rotateAndReconnect();
#ifdef DISPLAY_ENABLED
    else {
      appState = S_IDLE;
      updateIdleLed();
      delay(2000);
      display.showIdle();
    }
#endif
  }
}

// ── API rotation ──────────────────────────────────────────────
void rotateAndReconnect() {
  wsClient.disconnect();
  wsConnected = sessionReady = false;
  bool exhausted = apiManager.rotate();
  if (exhausted) {
    Serial.println("[API] All exhausted — 60s cooldown");
    appState = S_COOLDOWN;
    cooldownStart = millis();
#ifdef DISPLAY_ENABLED
    display.showError("Quota exhausted\nWait 60s...");
#endif
    led.setState(LED_ERROR);
    return;
  }

  // Key বা model rotate হয়েছে — IDLE এ ফিরে যাও।
  // পরবর্তী startTalking() এ নতুন key দিয়ে fresh connect হবে।
  Serial.printf("[API] Rotated → Key %d | Model: %s\n",
                apiManager.keyIndex() + 1,
                apiManager.getCurrentModel().c_str());
  appState = S_IDLE;
  updateIdleLed();
#ifdef DISPLAY_ENABLED
  display.showIdle();
#endif
}