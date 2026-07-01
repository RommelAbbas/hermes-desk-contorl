/*
  ============================================================================
   HERMES COMMAND DECK  -  ESP32  *** FINAL GENERIC RENDERER ***
  ============================================================================
   ESP32 = dumb screen driven entirely by server end. Modes, menu, colors, rotation,
   brightness, todos, face/video/gif/image, AND custom layouts - all from onyx.
   New features later = onyx only. THIS IS THE LAST UPLOAD.

   MODES (switch from device menu or onyx):
     face | video | gif | image | data | layout
       face/video/gif/image -> pixels streamed from onyx /frame
       data                 -> dashboard drawn on ESP32 from onyx JSON
       layout               -> onyx sends draw-ops (text/rect/line/dot anywhere)

   LIBRARIES: Adafruit ST7735 and ST7789 | Adafruit GFX | ArduinoJson v7 | WiFiManager (tzapu)
   (esp32 core 3.x required for analogWrite brightness)

   WIFI/URL: unknown wifi OR hold SELECT at boot OR menu "WIFI SETUP"
     -> join hotspot "Hermes-Setup" (pass hermes123) -> set wifi + Onyx URL -> Save

   WIRING:
     Display: VCC->3V3 GND->GND SCL->18 SDA->23 RES->4 DC->2 CS->5
              BLK->D21 (brightness) or 3V3 (always full)
     Buttons: MENU->32 UP->33 DOWN->13 SELECT->14   (other leg -> GND)
  ============================================================================
*/
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>

const char* DEFAULT_URL = "http://192.168.10.2:8090/panel"; // change according to your server
const char* AP_NAME = "Hermes-Setup";
const char* AP_PASS = "hermes123";

#define TFT_CS 5
#define TFT_DC 2
#define TFT_RST 4
#define BL_PIN 21

#define B_MENU 0
#define B_UP   1
#define B_DOWN 2
#define B_SEL  3
const uint8_t BTN_PIN[4] = {32, 33, 13, 14};
bool          btnLast[4] = {true, true, true, true};
unsigned long btnT[4]    = {0, 0, 0, 0};

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
Preferences prefs;
String onyxUrl, baseUrl;

#define C_BG ST77XX_BLACK
#define C_TEXT 0xC618
#define C_SEL ST77XX_YELLOW
#define C_OK 0x07E0
#define C_BAD 0xF800
#define C_DIM 0x7BEF

uint16_t accent = ST77XX_CYAN;
int cfgRotation = 0, cfgBright = 255;

String mode = "face";
bool frameMode() { return mode == "face" || mode == "video" || mode == "gif" || mode == "image"; }

String menuLabel[12]; String menuAction[12]; int menuN = 0;
bool menuOpen = false; int sel = 0;

bool d_onyx=false, d_ragnar=false, d_vps=false;
String d_uptime="--"; float d_temp=0, d_load=0;
String d_todos[8]; int d_todoN=0;
String d_cdName[4], d_cdLeft[4]; int d_cdN=0;
bool netOK=false;

// custom layout draw-ops (from onyx)
String opT[24], opV[24], opC[24];
int opX[24], opY[24], opA[24], opB[24], opS[24];
bool opFill[24];
int opN = 0;

unsigned long lastPoll=0;
const unsigned long POLL_MS=5000;
uint16_t frameRow[128];

bool pressed(int i) {
  bool n = digitalRead(BTN_PIN[i]);
  if (btnLast[i] && !n && (millis() - btnT[i] > 180)) {
    btnT[i] = millis(); btnLast[i] = n; return true;
  }
  btnLast[i] = n; return false;
}

uint16_t hex565(const String& h) {
  if (h.length() < 6) return accent;
  long r = strtol(h.substring(0,2).c_str(), NULL, 16);
  long g = strtol(h.substring(2,4).c_str(), NULL, 16);
  long b = strtol(h.substring(4,6).c_str(), NULL, 16);
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

void buildBase() {
  baseUrl = onyxUrl;
  int i = baseUrl.lastIndexOf("/panel");
  if (i > 0) baseUrl = baseUrl.substring(0, i);
}

void loadUrl() {
  prefs.begin("hermes", true);
  onyxUrl = prefs.getString("url", DEFAULT_URL);
  prefs.end(); buildBase();
}
void saveUrl(const String& u) {
  if (u.length() < 8) return;
  onyxUrl = u;
  prefs.begin("hermes", false); prefs.putString("url", onyxUrl); prefs.end();
  buildBase();
}

void runPortal(bool force) {
  tft.fillScreen(C_BG); tft.setTextSize(1);
  tft.setTextColor(C_SEL); tft.setCursor(4,4); tft.print("WIFI SETUP");
  tft.setTextColor(C_TEXT); tft.setCursor(4,22); tft.print("join "); tft.print(AP_NAME);
  tft.setCursor(4,36); tft.print("pass "); tft.print(AP_PASS);
  tft.setTextColor(C_DIM); tft.setCursor(4,60); tft.print("set wifi + URL");
  WiFiManager wm; wm.setConfigPortalTimeout(180);
  WiFiManagerParameter p("onyxurl", "Onyx URL (full)", onyxUrl.c_str(), 120);
  wm.addParameter(&p);
  if (force) wm.startConfigPortal(AP_NAME, AP_PASS); else wm.autoConnect(AP_NAME, AP_PASS);
  saveUrl(String(p.getValue()));
  netOK = (WiFi.status() == WL_CONNECTED);
}
void bootWiFi() {
  if (digitalRead(BTN_PIN[B_SEL]) == LOW) { runPortal(true); return; }
  tft.fillScreen(C_BG); tft.setTextColor(ST77XX_WHITE); tft.setCursor(4,4); tft.print("WiFi...");
  runPortal(false);
}
void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) { netOK = true; return; }
  netOK = false; WiFi.reconnect();
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 5000) delay(100);
  netOK = (WiFi.status() == WL_CONNECTED);
}

void sendAction(const String& action) {
  if (!netOK) return;
  String url;
  if (action.startsWith("mode:")) { url = baseUrl + "/set?mode=" + action.substring(5); mode = action.substring(5); }
  else if (action.startsWith("set:")) { url = baseUrl + "/set?" + action.substring(4); }
  else return;
  HTTPClient http; http.begin(url); http.setTimeout(3000); http.GET(); http.end();
}

void poll() {
  ensureWiFi();
  if (!netOK) { d_onyx = false; return; }
  HTTPClient http; http.begin(onyxUrl); http.setTimeout(3000);
  int code = http.GET(); Serial.print("poll -> "); Serial.println(code);
  if (code == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString())) {
      d_onyx = true;
      if (!menuOpen) mode = String((const char*)(doc["mode"] | "face"));
      d_ragnar = doc["ragnar"] | false;
      d_vps    = doc["vps"]    | false;
      d_uptime = String((const char*)(doc["uptime"] | "--"));
      d_temp   = doc["temp"] | 0.0;
      d_load   = doc["load"] | 0.0;
      d_todoN = 0;
      for (JsonVariant v : doc["todos"].as<JsonArray>())
        if (d_todoN < 8) d_todos[d_todoN++] = String(v.as<const char*>());
      d_cdN = 0;
      for (JsonObject c : doc["countdowns"].as<JsonArray>())
        if (d_cdN < 4) { d_cdName[d_cdN] = String((const char*)(c["name"] | "?")); d_cdLeft[d_cdN] = String((const char*)(c["left"] | "?")); d_cdN++; }
      menuN = 0;
      for (JsonObject it : doc["menu"].as<JsonArray>())
        if (menuN < 12) { menuLabel[menuN] = String((const char*)(it["label"] | "?")); menuAction[menuN] = String((const char*)(it["action"] | "")); menuN++; }
      // custom layout ops
      opN = 0;
      for (JsonObject o : doc["layout"].as<JsonArray>()) {
        if (opN >= 24) break;
        opT[opN]    = String((const char*)(o["t"] | ""));
        opX[opN]    = o["x"] | 0;
        opY[opN]    = o["y"] | 0;
        opA[opN]    = o["w"] | (o["x2"] | (o["r"] | 0));
        opB[opN]    = o["h"] | (o["y2"] | 0);
        opS[opN]    = o["s"] | 1;
        opFill[opN] = o["f"] | false;
        opC[opN]    = String((const char*)(o["c"] | ""));
        opV[opN]    = String((const char*)(o["v"] | ""));
        opN++;
      }
      JsonObject cfg = doc["cfg"].as<JsonObject>();
      if (!cfg.isNull()) {
        int r = cfg["rotation"] | cfgRotation;
        int br = cfg["bright"] | cfgBright;
        String acc = String((const char*)(cfg["accent"] | ""));
        if (r != cfgRotation) { cfgRotation = r; tft.setRotation(r); }
        if (br != cfgBright) { cfgBright = br; analogWrite(BL_PIN, br); }
        if (acc.length() >= 6) accent = hex565(acc);
      }
    } else d_onyx = false;
  } else d_onyx = false;
  http.end();
}

void drawFrame() {
  if (!netOK) return;
  HTTPClient http; http.begin(baseUrl + "/frame"); http.setTimeout(3000);
  int code = http.GET();
  if (code == 200) {
    WiFiClient* s = http.getStreamPtr();
    tft.startWrite(); tft.setAddrWindow(0, 0, 128, 128);
    for (int y = 0; y < 128; y++) {
      int need = 256; uint8_t* p = (uint8_t*)frameRow;
      while (need > 0) { int r = s->readBytes(p, need); if (r <= 0) break; p += r; need -= r; }
      for (int x = 0; x < 128; x++) { uint16_t v = (frameRow[x] >> 8) | (frameRow[x] << 8); tft.writePixels(&v, 1); }
    }
    tft.endWrite();
  }
  http.end();
}

void drawData() {
  tft.fillScreen(C_BG); tft.setTextSize(1);
  tft.setTextColor(accent); tft.setCursor(4,4); tft.print("HERMES");
  tft.drawFastHLine(0, 16, 128, C_DIM);
  int y = 22;
  tft.setTextColor(C_TEXT); tft.setCursor(4,y); tft.print("onyx ");
  tft.setTextColor(d_onyx ? C_OK : C_BAD); tft.print(d_onyx ? "OK" : "X");
  tft.setTextColor(C_TEXT); tft.print(" rag ");
  tft.setTextColor(d_ragnar ? C_OK : C_BAD); tft.print(d_ragnar ? "OK" : "X");
  tft.setTextColor(C_TEXT); tft.print(" vps ");
  tft.setTextColor(d_vps ? C_OK : C_BAD); tft.print(d_vps ? "OK" : "X");
  y = 36; tft.setTextColor(C_DIM); tft.setCursor(4,y);
  tft.print("up "); tft.print(d_uptime); tft.print(" "); tft.print(d_temp, 0); tft.print("C");
  y = 50; tft.setTextColor(accent); tft.setCursor(4,y); tft.print("TODO");
  y = 62;
  if (d_todoN == 0) { tft.setTextColor(C_DIM); tft.setCursor(4,y); tft.print("(none)"); }
  for (int i = 0; i < d_todoN; i++) {
    tft.setTextColor(C_OK); tft.setCursor(4,y); tft.print("- ");
    tft.setTextColor(C_TEXT); tft.print(d_todos[i].substring(0, 17));
    y += 11; if (y > 110) break;
  }
  if (d_cdN > 0) { tft.setTextColor(C_SEL); tft.setCursor(4, 118); tft.print(d_cdName[0]); tft.print(" "); tft.print(d_cdLeft[0]); }
}

void drawLayout() {
  tft.fillScreen(C_BG);
  for (int i = 0; i < opN; i++) {
    uint16_t col = (opC[i].length() >= 6) ? hex565(opC[i]) : accent;
    String t = opT[i];
    if (t == "fill") {
      tft.fillScreen(col);
    } else if (t == "text") {
      tft.setTextSize(opS[i] < 1 ? 1 : opS[i]);
      tft.setTextColor(col); tft.setCursor(opX[i], opY[i]); tft.print(opV[i]);
    } else if (t == "rect") {
      if (opFill[i]) tft.fillRect(opX[i], opY[i], opA[i], opB[i], col);
      else           tft.drawRect(opX[i], opY[i], opA[i], opB[i], col);
    } else if (t == "line") {
      tft.drawLine(opX[i], opY[i], opA[i], opB[i], col);
    } else if (t == "hline") {
      tft.drawFastHLine(opX[i], opY[i], opA[i], col);
    } else if (t == "vline") {
      tft.drawFastVLine(opX[i], opY[i], opA[i], col);
    } else if (t == "dot") {
      tft.fillCircle(opX[i], opY[i], opA[i] < 1 ? 2 : opA[i], col);
    } else if (t == "circle") {
      tft.drawCircle(opX[i], opY[i], opA[i] < 1 ? 2 : opA[i], col);
    }
  }
}

void drawMenu() {
  tft.fillScreen(C_BG); tft.setTextSize(1);
  tft.setTextColor(accent); tft.setCursor(4,4); tft.print("MENU");
  tft.drawFastHLine(0, 16, 128, C_DIM);
  int y = 22;
  for (int i = 0; i < menuN; i++) {
    tft.setTextColor(i == sel ? C_SEL : C_TEXT); tft.setCursor(4,y);
    tft.print(i == sel ? ">" : " "); tft.print(menuLabel[i]); y += 13;
  }
  tft.setTextColor(sel == menuN ? C_SEL : C_DIM); tft.setCursor(4,y);
  tft.print(sel == menuN ? ">" : " "); tft.print("WIFI SETUP");
  tft.setTextColor(C_DIM); tft.setCursor(4, 118); tft.print("MENU=close");
}

void setup() {
  Serial.begin(115200); delay(200);
  Serial.println("\n== COMMAND DECK (renderer) ==");
  for (int i = 0; i < 4; i++) pinMode(BTN_PIN[i], INPUT_PULLUP);
  pinMode(BL_PIN, OUTPUT); analogWrite(BL_PIN, cfgBright);
  tft.initR(INITR_144GREENTAB); tft.setRotation(cfgRotation);
  loadUrl(); bootWiFi(); poll(); lastPoll = millis();
}

void loop() {
  if (millis() - lastPoll > POLL_MS) { poll(); lastPoll = millis(); }

  if (pressed(B_MENU)) { menuOpen = !menuOpen; sel = 0; if (menuOpen) drawMenu(); }

  if (menuOpen) {
    int total = menuN + 1;
    if (pressed(B_UP))   { sel = (sel - 1 + total) % total; drawMenu(); }
    if (pressed(B_DOWN)) { sel = (sel + 1) % total; drawMenu(); }
    if (pressed(B_SEL))  {
      if (sel == menuN) { runPortal(true); menuOpen = false; }
      else { sendAction(menuAction[sel]); menuOpen = false; }
    }
    delay(20);
    return;
  }

  if (frameMode()) {
    drawFrame();
  } else if (mode == "data") {
    static unsigned long dt = 0;
    if (millis() - dt > 1500) { drawData(); dt = millis(); }
    delay(20);
  } else if (mode == "layout") {
    static unsigned long lt = 0;
    if (millis() - lt > 1000) { drawLayout(); lt = millis(); }
    delay(20);
  } else {
    delay(50);
  }
}