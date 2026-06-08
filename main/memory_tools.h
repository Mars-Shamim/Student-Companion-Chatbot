/*
 * memory_tools.h  v3
 * Multi-spreadsheet subject + chapter based memory
 * Gemini tool definitions + HTTP call handler
 */

#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>

class MemoryTools {
public:
  void load(Preferences &p) {
    scriptUrl = p.getString("script_url", "");
    Serial.printf("[Memory] %s\n",
                  scriptUrl.isEmpty() ? "disabled" : "enabled");
  }

  bool isEnabled() { return !scriptUrl.isEmpty(); }

  // ── Gemini tool definitions ───────────────────────────────
  void addToolDefinitions(JsonArray &tools) {
    if (!isEnabled())
      return;

    JsonObject memTool = tools.createNestedObject();
    JsonArray funcDecls = memTool.createNestedArray("functionDeclarations");

    // remember(subject, chapter, index, explanation)
    {
      JsonObject fn = funcDecls.createNestedObject();
      fn["name"] = "remember";
      fn["description"] =
          "Student এর প্রশ্নের উত্তর Google Sheets Database এ সংরক্ষণ করে। "
          "User যখন বলে এটা মনে রেখো বা save করো। "
          "subject হলো বিষয়ের নাম যেমন Physics Chemistry Biology Math English "
          "General। "
          "chapter হলো chapter নাম যেমন Physics 1 বা Chemistry 2। "
          "index হলো সংক্ষিপ্ত keyword যেমন Newtons Laws। "
          "explanation হলো Gemini এর পুরো text উত্তর।";
      JsonObject p = fn.createNestedObject("parameters");
      p["type"] = "object";
      JsonObject props = p.createNestedObject("properties");
      props["subject"]["type"] = "string";
      props["subject"]["description"] =
          "বিষয়: Physics Chemistry Biology Math English General";
      props["chapter"]["type"] = "string";
      props["chapter"]["description"] =
          "Chapter নাম যেমন Physics 1 বা Chemistry 2";
      props["index"]["type"] = "string";
      props["index"]["description"] = "সংক্ষিপ্ত keyword বা topic";
      props["explanation"]["type"] = "string";
      props["explanation"]["description"] = "পুরো text উত্তর যা save হবে";
      JsonArray req = p.createNestedArray("required");
      req.add("subject");
      req.add("index");
      req.add("explanation");
    }

    // recall(query, subject?, chapter?)
    {
      JsonObject fn = funcDecls.createNestedObject();
      fn["name"] = "recall";
      fn["description"] =
          "Google Sheets থেকে আগে save করা তথ্য খোঁজে। "
          "subject দিলে শুধু সেই বিষয়ে, chapter দিলে শুধু সেই chapter এ খোঁজে। "
          "কিছু না দিলে সব database এ খোঁজে।";
      JsonObject p = fn.createNestedObject("parameters");
      p["type"] = "object";
      JsonObject props = p.createNestedObject("properties");
      props["query"]["type"] = "string";
      props["query"]["description"] = "কী খুঁজতে হবে";
      props["subject"]["type"] = "string";
      props["subject"]["description"] = "নির্দিষ্ট বিষয় optional";
      props["chapter"]["type"] = "string";
      props["chapter"]["description"] = "নির্দিষ্ট chapter optional";
      p.createNestedArray("required").add("query");
    }

    // list_subjects()
    {
      JsonObject fn = funcDecls.createNestedObject();
      fn["name"] = "list_subjects";
      fn["description"] = "কোন কোন subject database আছে, কোন কোন chapter আছে "
                          "এবং প্রতিটায় কতটা entry আছে।";
      // No parameters needed
    }
  }

  // ── Handle tool call ──────────────────────────────────────
  struct ToolResult {
    String wsMessage;
    String savedSubject;
    String savedChapter;
    String savedIndex;
    bool didSave = false;
  };

  ToolResult handleToolCall(const String &toolName, JsonObject &args) {
    ToolResult result;
    if (!isEnabled()) {
      result.wsMessage = buildResponse(
          toolName, "{\"ok\":false,\"error\":\"Memory not configured\"}");
      return result;
    }

    Serial.printf("[Tool] %s\n", toolName.c_str());

    // ArduinoJson 7-এর নিয়ম অনুযায়ী DynamicJsonDocument-এর জায়গায় JsonDocument
    // ব্যবহার করা ভালো
    JsonDocument payload;

    if (toolName == "remember") {
      String subject = args["subject"] | "General";
      // এখানে explicit কাস্টিং করা হয়েছে এরর দূর করার জন্য:
      String chapter = args["chapter"] | String(subject + " 1");
      String index = args["index"] | "";
      String explanation = args["explanation"] | "";

      payload["action"] = "remember";
      payload["subject"] = subject;
      payload["chapter"] = chapter;
      payload["index"] = index;
      payload["explanation"] = explanation;

      result.didSave = true;
      result.savedSubject = subject;
      result.savedChapter = chapter;
      result.savedIndex = index;

    } else if (toolName == "recall") {
      payload["action"] = "recall";
      payload["query"] = args["query"] | "";
      payload["subject"] = args["subject"] | "";
      payload["chapter"] = args["chapter"] | "";

    } else if (toolName == "list_subjects") {
      payload["action"] = "list_subjects";
    }

    String body;
    serializeJson(payload, body);
    String response = httpPost(body);
    Serial.printf("[Tool] %.80s\n", response.c_str());

    result.wsMessage = buildResponse(toolName, response);
    return result;
  }

private:
  String scriptUrl;

  String httpPost(const String &body) {
    HTTPClient http;
    http.begin(scriptUrl);
    http.addHeader("Content-Type", "application/json");
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(15000); // Apps Script এ একটু বেশি time লাগে
    int code = http.POST(body);
    String resp =
        (code > 0) ? http.getString()
                   : "{\"ok\":false,\"error\":\"HTTP " + String(code) + "\"}";
    http.end();
    return resp;
  }

  String buildResponse(const String &name, const String &resultJson) {
    JsonDocument doc;
    JsonDocument res;
    deserializeJson(res, resultJson);
    JsonObject fr = doc.createNestedObject("tool_response")
                        .createNestedArray("function_responses")
                        .createNestedObject();
    fr["name"] = name;
    fr["response"] = res.as<JsonObject>();
    String out;
    serializeJson(doc, out);
    return out;
  }
};