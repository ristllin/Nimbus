// Provisioning + network test tool (standalone; not part of the product build).
// Takes credentials over serial and writes them to NVS (namespace "solide", the
// same one the firmware reads), so no secret is ever committed to a file. Also
// scans WiFi (2.4 GHz only - what the S3 can actually join) and tests STA.
//
// This is NOT the first-install path: it replaces Nimbus with a serial-only
// diagnostic and does not start the setup AP or web UI. Always provide the port:
//   pio run -e provision -t upload --upload-port /dev/cu.usbserial-XXXX
// Install the product with: python3 tools/setup_device.py
//
// Commands (newline-terminated): SCAN | SET key=value | SETI key=int |
//   CONNECT ssid|pass | CONNECTNVS | STATUS | TOKEN
#include <Arduino.h>
#include <WiFi.h>
#include <solide/memory.h>

static String line;

static void beginClean(const String& ss, const String& pw) {
  WiFi.disconnect(true, true);  // drop any prior assoc + clear config
  delay(150);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ss.c_str(), pw.c_str());
}

static void waitConnect(const char* tag) {
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(200);
  Serial.printf("%s status=%d ip=%s rssi=%d\n", tag, WiFi.status(),
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

static void handle(const String& cmd) {
  if (cmd == "SCAN") {
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks();
    Serial.printf("SCAN %d networks (2.4GHz only):\n", n);
    for (int i = 0; i < n; ++i)
      Serial.printf("  ssid='%s' rssi=%d ch=%d enc=%d\n", WiFi.SSID(i).c_str(),
                    WiFi.RSSI(i), WiFi.channel(i), int(WiFi.encryptionType(i)));
    Serial.println("SCAN done");
  } else if (cmd.startsWith("SET ")) {
    int eq = cmd.indexOf('=');
    if (eq > 4) {
      String k = cmd.substring(4, eq), v = cmd.substring(eq + 1);
      bool ok = solide::memory::setString(k.c_str(), v);
      Serial.printf("SET %s ok=%d len=%d\n", k.c_str(), int(ok), v.length());
    }
  } else if (cmd.startsWith("SETI ")) {
    int eq = cmd.indexOf('=');
    if (eq > 5) {
      String k = cmd.substring(5, eq);
      int v = cmd.substring(eq + 1).toInt();
      bool ok = solide::memory::setInt(k.c_str(), v);
      Serial.printf("SETI %s=%d ok=%d\n", k.c_str(), v, int(ok));
    }
  } else if (cmd.startsWith("CONNECT ")) {
    int bar = cmd.indexOf('|');
    String ss = cmd.substring(8, bar), pw = cmd.substring(bar + 1);
    Serial.printf("CONNECT '%s'...\n", ss.c_str());
    beginClean(ss, pw);
    waitConnect("CONNECT");
  } else if (cmd == "CONNECTNVS") {
    String ss = solide::memory::getString("staSsid", "");
    String pw = solide::memory::getString("staPass", "");
    Serial.printf("CONNECTNVS '%s'...\n", ss.c_str());
    beginClean(ss, pw);
    waitConnect("CONNECTNVS");
  } else if (cmd == "STATUS") {
    Serial.printf(
        "STATUS wifi=%d ip=%s mode=%d screen='%s' type='%s' staSsid='%s' staPassLen=%d "
        "oaiLen=%d antLen=%d\n",
        WiFi.status(), WiFi.localIP().toString().c_str(),
        int(solide::memory::getInt("nimbus_mode", 0)),
        solide::memory::getString("scrModel", "eink").c_str(),
        solide::memory::getString("otaType", "").c_str(),
        solide::memory::getString("staSsid", "").c_str(),
        solide::memory::getString("staPass", "").length(),
        solide::memory::getString("oaiKey", "").length(),
        solide::memory::getString("antKey", "").length());
  } else if (cmd == "TOKEN") {
    // Physical-UART recovery for a device whose display cannot show the sign-in
    // QR. Deliberately narrow: no generic NVS read command and no other secret.
    String t = solide::memory::getString("webTok", "");
    if (t.length()) Serial.printf("TOKEN %s\n", t.c_str());
    else Serial.println("TOKEN missing");
  }
}

void setup() {
  Serial.begin(115200);
  delay(800);
  solide::memory::begin();
  // Print the disconnect REASON code - decisive for wrong-password (15/205/2)
  // vs. AP-not-found (201) vs. other, and the IP on success.
  WiFi.onEvent([](WiFiEvent_t ev, WiFiEventInfo_t info) {
    if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
      Serial.printf("WIFI_DISCONNECTED reason=%d\n",
                    info.wifi_sta_disconnected.reason);
    else if (ev == ARDUINO_EVENT_WIFI_STA_GOT_IP)
      Serial.printf("WIFI_GOT_IP %s\n", WiFi.localIP().toString().c_str());
  });
  WiFi.mode(WIFI_STA);
  Serial.println("PROVISION READY");
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (line.length()) { handle(line); line = ""; }
    } else {
      line += c;
    }
  }
  delay(5);
}
