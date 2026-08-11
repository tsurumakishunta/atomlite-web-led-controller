#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <ctype.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#if __has_include("wifi_config.h")
#include "wifi_config.h"
#else
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

#endif

namespace {
constexpr uint8_t LED_PIN = 27;
constexpr uint8_t LED_COUNT = 1;
constexpr uint8_t LED_BRIGHTNESS = 64;
constexpr uint32_t WIFI_TIMEOUT_MS = 15000;

constexpr char HOST_NAME[] = "atomlite";
constexpr char AP_SSID[] = "AtomLite-LED";
constexpr char AP_PASSWORD[] = "atomlite";

Adafruit_NeoPixel led(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
Preferences preferences;
WebServer server(80);

uint32_t currentColor = 0x33AAFF;

const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="ja">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Atom Lite LED</title>
  <style>
    :root { color-scheme: dark; font-family: system-ui, sans-serif; }
    * { box-sizing: border-box; }
    body {
      min-height: 100vh; margin: 0; display: grid; place-items: center;
      color: #eef3ff; background: radial-gradient(circle at top, #25304d, #0b0e16 60%);
    }
    main {
      width: min(92vw, 430px); padding: 30px; border: 1px solid #ffffff22;
      border-radius: 24px; background: #151a27e8; box-shadow: 0 24px 70px #0008;
    }
    h1 { margin: 0 0 6px; font-size: 1.65rem; }
    p { margin: 0 0 24px; color: #aeb8ce; }
    .picker { display: grid; grid-template-columns: 112px 1fr; gap: 22px; align-items: center; }
    input[type="color"] {
      width: 112px; height: 112px; padding: 5px; border: 0; border-radius: 18px;
      background: #ffffff18; cursor: pointer;
    }
    label { display: block; margin-bottom: 8px; color: #aeb8ce; font-size: .9rem; }
    input[type="text"] {
      width: 100%; padding: 12px; border: 1px solid #ffffff24; border-radius: 10px;
      color: white; background: #080b12; font: 600 1rem ui-monospace, monospace;
    }
    button {
      width: 100%; margin-top: 24px; padding: 13px; border: 0; border-radius: 12px;
      color: #081019; background: #79d8ff; font-weight: 800; font-size: 1rem; cursor: pointer;
    }
    button:hover { filter: brightness(1.08); }
    button:disabled { cursor: wait; opacity: .65; }
    #status { min-height: 1.3em; margin: 14px 0 0; text-align: center; color: #83efb3; }
  </style>
</head>
<body>
  <main>
    <h1>Atom Lite LED</h1>
    <p>好きな色を選んで、Atom Liteへ保存します。</p>
    <div class="picker">
      <input id="picker" type="color" value="#33aaff" aria-label="LEDの色">
      <div>
        <label for="hex">カラーコード</label>
        <input id="hex" type="text" value="#33AAFF" maxlength="7" spellcheck="false">
      </div>
    </div>
    <button id="save" type="button">この色を保存</button>
    <div id="status" role="status" aria-live="polite"></div>
  </main>
  <script>
    const picker = document.querySelector('#picker');
    const hex = document.querySelector('#hex');
    const save = document.querySelector('#save');
    const status = document.querySelector('#status');
    const valid = value => /^#[0-9a-f]{6}$/i.test(value);

    picker.addEventListener('input', () => { hex.value = picker.value.toUpperCase(); });
    hex.addEventListener('input', () => {
      const value = hex.value.trim();
      if (valid(value)) picker.value = value;
    });

    async function loadColor() {
      try {
        const response = await fetch('/api/color');
        if (!response.ok) throw new Error();
        const data = await response.json();
        picker.value = data.color;
        hex.value = data.color.toUpperCase();
      } catch (_) {
        status.textContent = '現在の色を取得できませんでした。';
      }
    }

    save.addEventListener('click', async () => {
      const color = hex.value.trim();
      if (!valid(color)) {
        status.textContent = '#RRGGBB 形式で入力してね。';
        return;
      }
      save.disabled = true;
      status.textContent = '保存中…';
      try {
        const body = new URLSearchParams({ color });
        const response = await fetch('/api/color', { method: 'POST', body });
        if (!response.ok) throw new Error(await response.text());
        const data = await response.json();
        picker.value = data.color;
        hex.value = data.color.toUpperCase();
        status.textContent = 'LEDの色を保存しました。';
      } catch (_) {
        status.textContent = '保存できませんでした。接続を確認してね。';
      } finally {
        save.disabled = false;
      }
    });

    loadColor();
  </script>
</body>
</html>
)HTML";

String colorToHex(uint32_t color) {
  char result[8];
  snprintf(result, sizeof(result), "#%06lX", static_cast<unsigned long>(color & 0xFFFFFF));
  return String(result);
}

bool parseColor(const String &value, uint32_t &color) {
  if (value.length() != 7 || value.charAt(0) != '#') {
    return false;
  }

  for (size_t i = 1; i < value.length(); ++i) {
    if (!isxdigit(static_cast<unsigned char>(value.charAt(i)))) {
      return false;
    }
  }

  color = strtoul(value.substring(1).c_str(), nullptr, 16) & 0xFFFFFF;
  return true;
}

void showColor(uint32_t color) {
  led.setPixelColor(0, color);
  led.show();
}

void sendColorJson() {
  server.send(200, "application/json; charset=utf-8",
              String("{\"color\":\"") + colorToHex(currentColor) + "\"}");
}

void handleSaveColor() {
  if (!server.hasArg("color")) {
    server.send(400, "text/plain; charset=utf-8", "color is required");
    return;
  }

  uint32_t newColor;
  if (!parseColor(server.arg("color"), newColor)) {
    server.send(400, "text/plain; charset=utf-8", "invalid color");
    return;
  }

  currentColor = newColor;
  showColor(currentColor);
  preferences.putUInt("color", currentColor);
  sendColorJson();
}

void startNetwork() {
  const bool credentialsConfigured = String(WIFI_SSID) != "YOUR_WIFI_SSID" &&
                                     String(WIFI_SSID).length() > 0;

  if (credentialsConfigured) {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(HOST_NAME);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("Wi-Fi '%s' に接続中", WIFI_SSID);

    const uint32_t startedAt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startedAt < WIFI_TIMEOUT_MS) {
      delay(500);
      Serial.print('.');
    }
    Serial.println();
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (MDNS.begin(HOST_NAME)) {
      MDNS.addService("http", "tcp", 80);
    }
    Serial.println("Wi-Fiに接続しました。");
    Serial.printf("URL: http://%s.local/\n", HOST_NAME);
    Serial.printf("IP : http://%s/\n", WiFi.localIP().toString().c_str());
    return;
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.println("アクセスポイントモードで起動しました。");
  Serial.printf("SSID: %s\n", AP_SSID);
  Serial.printf("PASS: %s\n", AP_PASSWORD);
  Serial.printf("URL : http://%s/\n", WiFi.softAPIP().toString().c_str());
}

void startWebServer() {
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
  });
  server.on("/api/color", HTTP_GET, sendColorJson);
  server.on("/api/color", HTTP_POST, handleSaveColor);
  server.onNotFound([]() {
    server.send(404, "text/plain; charset=utf-8", "Not found");
  });
  server.begin();
  Serial.println("Webサーバーを開始しました。");
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);

  led.begin();
  led.setBrightness(LED_BRIGHTNESS);
  preferences.begin("atom-led", false);
  currentColor = preferences.getUInt("color", currentColor);
  showColor(currentColor);

  startNetwork();
  startWebServer();
}

void loop() {
  server.handleClient();
  delay(2);
}
