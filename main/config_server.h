/*
 * config_server.h  v2.0
 * OTA Web Configuration
 *
 * Fixed from v1:
 *  - Preferences stored by reference in load(), not as pointer
 *  - All config fields exposed cleanly
 *  - isToggle() added (was missing, causing toggleMode to always be false)
 *  - Masked key display on reload
 *  - AP + STA web server both supported
 */

#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <vector>

#define CFG_SSID "wifi_ssid"
#define CFG_PASS "wifi_pass"
#define CFG_SI "sys_inst"
#define CFG_VOICE "voice"
#define CFG_BTN "btn_mode"
#define CFG_DEVNAME "dev_name"
#define CFG_KEY_COUNT "key_count"

class ConfigServer {
public:
  ConfigServer() : server(80) {}

  // ── Load from NVS ──────────────────────────────────────────
  void load(Preferences &p) {
    ssid = p.getString(CFG_SSID, "");
    pass = p.getString(CFG_PASS, "");
    si = p.getString(CFG_SI, defaultSI());
    voice = p.getString(CFG_VOICE, "Aoede");
    btn = p.getString(CFG_BTN, "ptt");
    name = p.getString(CFG_DEVNAME, "AI Student Bot");
    scriptUrl = p.getString("script_url", "");
  }

  String getScriptUrl() { return scriptUrl; }

  // ── Getters ────────────────────────────────────────────────
  String getSSID() { return ssid; }
  String getPassword() { return pass; }
  String getSystemInstruction() { return si; }
  String getVoice() { return voice; }
  String getDeviceName() { return name; }
  bool isToggle() { return btn == "toggle"; }

  // ── AP mode ────────────────────────────────────────────────
  void startAP() {
    WiFi.softAP("AI-Student-Config", "student123");
    delay(200);
    Serial.printf("[AP] IP: %s\n", WiFi.softAPIP().toString().c_str());
  }

  // ── Web server ─────────────────────────────────────────────
  void startServer() {
    server.on("/", HTTP_GET, [this]() { serveRoot(); });
    server.on("/save", HTTP_POST, [this]() { handleSave(); });
    server.on("/restart", HTTP_POST, [this]() { handleRestart(); });
    server.onNotFound([this]() {
      server.sendHeader("Location", "/");
      server.send(302);
    });
    server.begin();
  }

  void handle() { server.handleClient(); }

private:
  WebServer server;
  String ssid, pass, si, voice, btn, name, scriptUrl;

  // Saved Preferences reference for save handler
  // We reopen NVS in handleSave to avoid dangling pointer
  void handleSave() {
    Preferences p;
    p.begin("ai-cfg", false);

    String newSSID = server.arg("ssid");
    String newPass = server.arg("pass");
    if (!newSSID.isEmpty()) {
      ssid = newSSID;
      p.putString(CFG_SSID, ssid);
    }
    if (!newPass.isEmpty()) {
      pass = newPass;
      p.putString(CFG_PASS, pass);
    }

    // API keys
    int count = 0;
    for (int i = 0; i < 5; i++) {
      String k = server.arg("k" + String(i));
      if (!k.isEmpty() && k.indexOf("****") == -1) {
        p.putString(("api_key_" + String(i)).c_str(), k);
        count++;
      }
    }
    if (count > 0)
      p.putInt(CFG_KEY_COUNT, count);

    si = server.arg("si");
    p.putString(CFG_SI, si);
    voice = server.arg("voice");
    p.putString(CFG_VOICE, voice);
    btn = server.arg("btn");
    p.putString(CFG_BTN, btn);
    name = server.arg("name");
    p.putString(CFG_DEVNAME, name);
    String su = server.arg("script_url");
    if (!su.isEmpty()) {
      scriptUrl = su;
      p.putString("script_url", su);
    }

    p.end();
    Serial.println("[Config] Saved");
    server.sendHeader("Location", "/?ok=1");
    server.send(302);
    delay(1000);
    ESP.restart();
  }

  void handleRestart() {
    server.send(200, "text/plain", "Restarting...");
    delay(500);
    ESP.restart();
  }

  // ── Config web page ────────────────────────────────────────
  void serveRoot() {
    // Build key fields
    Preferences p;
    p.begin("ai-cfg", true); // read-only
    String keyFields = "";
    int kCount = p.getInt(CFG_KEY_COUNT, 0);
    for (int i = 0; i < 5; i++) {
      String val =
          (i < kCount) ? p.getString(("api_key_" + String(i)).c_str(), "") : "";
      String display =
          val.isEmpty()
              ? ""
              : ("****" + val.substring(max(0, (int)val.length() - 4)));
      keyFields += "<label>API Key " + String(i + 1) + "</label>";
      keyFields += "<input name='k" + String(i) +
                   "' type='password' placeholder='AIza...' value='" + display +
                   "'>";
    }
    p.end();

    bool saved = server.arg("ok") == "1";

    String pg = "<!DOCTYPE html><html><head>";
    pg += "<meta charset='UTF-8'><meta name='viewport' "
          "content='width=device-width,initial-scale=1'>";
    pg += "<title>AI Student Config</title>";
    pg += "<style>";
    pg += "*{box-sizing:border-box;margin:0;padding:0}";
    pg += "body{background:#0d1117;color:#e6edf3;font-family:system-ui,sans-"
          "serif;padding:16px}";
    pg += "h1{font-size:18px;margin-bottom:4px;color:#58a6ff}";
    pg += ".sub{color:#8b949e;font-size:12px;margin-bottom:20px}";
    pg += ".card{background:#161b22;border:1px solid "
          "#30363d;border-radius:10px;padding:16px;margin-bottom:14px}";
    pg += ".card "
          "h2{font-size:11px;text-transform:uppercase;letter-spacing:.08em;"
          "color:#8b949e;margin-bottom:12px}";
    pg += "label{display:block;font-size:13px;color:#8b949e;margin:10px 0 4px}";
    pg += "input,textarea,select{width:100%;background:#0d1117;border:1px "
          "solid #30363d;border-radius:6px;";
    pg += "padding:9px "
          "10px;color:#e6edf3;font-size:14px;font-family:inherit;outline:none}";
    pg += "input:focus,textarea:focus,select:focus{border-color:#58a6ff}";
    pg += "textarea{min-height:90px;resize:vertical;line-height:1.5}";
    pg += ".row{display:flex;gap:10px;margin-top:4px}";
    pg += ".opt{flex:1;display:flex;align-items:center;gap:8px;background:#"
          "0d1117;";
    pg += "border:1px solid "
          "#30363d;border-radius:6px;padding:9px;cursor:pointer}";
    pg += ".opt input{width:auto}";
    pg += ".btn{width:100%;padding:12px;border:none;border-radius:8px;font-"
          "size:15px;font-weight:600;cursor:pointer;margin-top:4px}";
    pg += ".save{background:#238636;color:#fff}";
    pg += ".rst{background:#21262d;color:#e6edf3;margin-top:8px}";
    pg += ".toast{display:none;background:#238636;color:#fff;padding:10px "
          "16px;border-radius:6px;margin-bottom:14px;font-size:14px}";
    pg += ".ip{font-size:12px;color:#58a6ff;margin-top:4px}";
    pg += "</style></head><body>";

    pg += "<h1>🤖 AI Student Assistant</h1>";
    pg += "<p class='sub'>ESP32-S3 Configuration</p>";
    if (WiFi.status() == WL_CONNECTED) {
      pg += "<p class='ip'>WiFi: " + WiFi.SSID() + " | " +
            WiFi.localIP().toString() + "</p>";
    }

    if (saved)
      pg += "<div class='toast' style='display:block'>✓ Saved &amp; "
            "restarted</div>";

    pg += "<form method='POST' action='/save'>";

    // WiFi
    pg += "<div class='card'><h2>WiFi</h2>";
    pg += "<label>SSID</label><input name='ssid' value='" + ssid +
          "' placeholder='Network name'>";
    pg += "<label>Password</label><input name='pass' type='password' "
          "placeholder='Leave blank to keep'>";
    pg += "</div>";

    // API Keys
    pg += "<div class='card'><h2>Google API Keys (up to 5)</h2>";
    pg += "<p "
          "style='font-size:12px;color:#8b949e;margin-bottom:8px'>Auto-rotates "
          "when quota exceeded</p>";
    pg += keyFields;
    pg += "</div>";

    // Assistant
    pg += "<div class='card'><h2>Assistant Settings</h2>";
    pg += "<label>System Instruction</label>";
    pg += "<textarea name='si'>" + si + "</textarea>";
    pg += "<label>Voice</label><select name='voice'>";
    const char *voices[] = {"Aoede", "Puck", "Charon", "Kore", "Fenrir"};
    const char *vDesc[] = {"Aoede (Female, Warm)", "Puck (Male, Bright)",
                           "Charon (Male, Deep)", "Kore (Female, Clear)",
                           "Fenrir (Male, Strong)"};
    for (int i = 0; i < 5; i++) {
      pg += "<option value='";
      pg += voices[i];
      pg += "'";
      if (voice == voices[i])
        pg += " selected";
      pg += ">";
      pg += vDesc[i];
      pg += "</option>";
    }
    pg += "</select>";
    pg += "<label>Button Mode</label><div class='row'>";
    pg += "<label class='opt'><input type='radio' name='btn' value='ptt'";
    if (btn != "toggle")
      pg += " checked";
    pg += "><span>Push-to-Talk</span></label>";
    pg += "<label class='opt'><input type='radio' name='btn' value='toggle'";
    if (btn == "toggle")
      pg += " checked";
    pg += "><span>Toggle</span></label></div>";
    pg += "<label>Device Name</label><input name='name' value='" + name + "'>";
    pg += "<label>Google Apps Script URL</label>";
    pg += "<input name='script_url' type='url' value='" + scriptUrl +
          "' placeholder='https://script.google.com/macros/s/...'>";
    pg += "<p style='font-size:12px;color:#8b949e;margin-top:4px'>Memory "
          "feature এর জন্য — deploy করার পর URL paste করুন</p>";
    pg += "</div>";

    pg +=
        "<button type='submit' class='btn save'>💾 Save &amp; Restart</button>";
    pg += "</form>";
    pg += "<form method='POST' action='/restart'>";
    pg += "<button type='submit' class='btn rst'>⟳ Restart Only</button>";
    pg += "</form></body></html>";

    server.send(200, "text/html; charset=utf-8", pg);
  }

  String defaultSI() {
    return "You are a helpful AI tutor for students. "
           "Answer in the same language the student uses (Bangla or English). "
           "For math and science, explain step by step. Keep answers concise "
           "and clear. "
           "You have Google Search access for current information.";
  }
};
