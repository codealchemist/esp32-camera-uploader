// yicam-bridge: ESP32 MQTT broker that turns Yi camera (yi-hack) motion
// events into photos and video clips in Google Drive.
//
// Flow:
//   camera --MQTT motion_start/stop--> ESP32 broker
//   ESP32 --snapshot.sh?file=--> camera saves the JPEG on its own SD card
//   ESP32 streams camera file --> Drive API (1-hour token from the Apps Script)
//   camera --MQTT motion_files--> ESP32 queues the event's clips, same path
//   ESP32 --"saved" report--> Apps Script (emails, day folders, cleanup)
//
// Target: classic ESP32 (ESP-WROOM-32) without PSRAM. Files never live on the
// ESP32: they are streamed camera -> TLS socket in small pieces.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFiManager.h>
#include <PicoMQTT.h>
#include <time.h>
#include <esp_system.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

#include "root_certs.h"
#include "web_ui.h"

static const char *FW_VERSION = "0.7.1";
static const char *HOSTNAME = "yicam-bridge";
static const char *SETUP_AP_NAME = "YiCam-Setup";
// The setup network's password is generated per device (see setupApPassword()).
static const int BOOT_BUTTON_PIN = 0;             // "IO0"/BOOT button
static const int LED_PIN = 2;                     // blue LED on most DevKits
static const uint32_t MOTION_FAILSAFE_MS = 5UL * 60UL * 1000UL;
static const uint32_t UPLOAD_RETRY_MS = 30UL * 1000UL;

// ---------------------------------------------------------------------------
// Configuration (stored in NVS, edited in the setup portal)
// ---------------------------------------------------------------------------
struct Config {
  String camHost = "192.168.0.163";
  String camUser;  // optional HTTP basic auth of the yi-hack web UI
  String camPass;
  String res = "high";  // high | low
  String scriptUrl;     // https://script.google.com/macros/s/.../exec
  String token;         // shared secret checked by the Apps Script
  String mqttUser;      // credentials the camera uses to connect to us
  String mqttPass;
  String motionTopic = "yicam/motion_detection";
  uint32_t intervalSec = 2;
  uint32_t maxShots = 10;
  String tz = "<-03>3";  // POSIX TZ, UTC-3
  String otaPass;
  String dashHash;  // PBKDF2 hash of the dashboard password; empty = no login
  bool videoOn = true;      // upload the camera's motion video clips
  uint32_t videoCap = 50;   // clips per day at most
  bool remoteOn = true;     // check in with the Apps Script for control-page requests
  uint32_t pollSec = 30;    // seconds between check-ins
} cfg;

Preferences prefs;

void loadConfig() {
  prefs.begin("yicam", true);
  cfg.camHost = prefs.getString("camHost", cfg.camHost);
  cfg.camUser = prefs.getString("camUser", cfg.camUser);
  cfg.camPass = prefs.getString("camPass", cfg.camPass);
  cfg.res = prefs.getString("res", cfg.res);
  cfg.scriptUrl = prefs.getString("scriptUrl", cfg.scriptUrl);
  cfg.token = prefs.getString("token", cfg.token);
  cfg.mqttUser = prefs.getString("mqttUser", cfg.mqttUser);
  cfg.mqttPass = prefs.getString("mqttPass", cfg.mqttPass);
  cfg.motionTopic = prefs.getString("topic", cfg.motionTopic);
  cfg.intervalSec = prefs.getUInt("interval", cfg.intervalSec);
  cfg.maxShots = prefs.getUInt("maxShots", cfg.maxShots);
  cfg.tz = prefs.getString("tz", cfg.tz);
  cfg.otaPass = prefs.getString("otaPass", cfg.otaPass);
  cfg.dashHash = prefs.getString("dashHash", cfg.dashHash);
  cfg.videoOn = prefs.getBool("videoOn", cfg.videoOn);
  cfg.videoCap = prefs.getUInt("videoCap", cfg.videoCap);
  cfg.remoteOn = prefs.getBool("remoteOn", cfg.remoteOn);
  cfg.pollSec = prefs.getUInt("pollSec", cfg.pollSec);
  prefs.end();
  if (cfg.pollSec < 15) cfg.pollSec = 15;
  if (cfg.pollSec > 300) cfg.pollSec = 300;
  if (cfg.intervalSec < 1) cfg.intervalSec = 1;
  if (cfg.maxShots < 1) cfg.maxShots = 1;
  if (cfg.res != "low") cfg.res = "high";
}

// Writes a configuration to NVS. The web settings page saves a modified copy
// and restarts, so the running cfg is never changed while the worker task
// may be reading it.
void saveConfig(const Config &c = cfg) {
  prefs.begin("yicam", false);
  prefs.putString("camHost", c.camHost);
  prefs.putString("camUser", c.camUser);
  prefs.putString("camPass", c.camPass);
  prefs.putString("res", c.res);
  prefs.putString("scriptUrl", c.scriptUrl);
  prefs.putString("token", c.token);
  prefs.putString("mqttUser", c.mqttUser);
  prefs.putString("mqttPass", c.mqttPass);
  prefs.putString("topic", c.motionTopic);
  prefs.putUInt("interval", c.intervalSec);
  prefs.putUInt("maxShots", c.maxShots);
  prefs.putString("tz", c.tz);
  prefs.putString("otaPass", c.otaPass);
  prefs.putString("dashHash", c.dashHash);
  prefs.putBool("videoOn", c.videoOn);
  prefs.putUInt("videoCap", c.videoCap);
  prefs.putBool("remoteOn", c.remoteOn);
  prefs.putUInt("pollSec", c.pollSec);
  prefs.end();
}

// ---------------------------------------------------------------------------
// Dashboard password and sign-in sessions
// ---------------------------------------------------------------------------
// The password itself is never stored: only a salted PBKDF2-HMAC-SHA256 hash.
// Sessions are random 128-bit tokens kept in NVS so a restart doesn't sign
// everyone out.
static const uint32_t PBKDF2_ITERS = 10000;
static const size_t MIN_DASH_PASS = 12;
static const uint32_t SESSION_SHORT_S = 12UL * 3600UL;      // browser session
static const uint32_t SESSION_LONG_S = 30UL * 86400UL;      // "keep me signed in"
static const int MAX_SESSIONS = 8;

struct Session {
  uint8_t tok[16];
  uint32_t exp;  // epoch seconds; 0 = free slot
};
Session sessions[MAX_SESSIONS];

String toHex(const uint8_t *b, size_t n) {
  static const char *h = "0123456789abcdef";
  String s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; i++) {
    s += h[b[i] >> 4];
    s += h[b[i] & 15];
  }
  return s;
}

bool fromHex(const String &s, uint8_t *out, size_t n) {
  if (s.length() != n * 2) return false;
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < n; i++) {
    int hi = nib(s[2 * i]), lo = nib(s[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (hi << 4) | lo;
  }
  return true;
}

bool equalCT(const uint8_t *a, const uint8_t *b, size_t n) {
  uint8_t d = 0;
  for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
  return d == 0;
}

bool pbkdf2(const String &pass, const uint8_t *salt, size_t saltLen,
            uint32_t iters, uint8_t out[32]) {
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  bool ok = mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1) == 0 &&
            mbedtls_pkcs5_pbkdf2_hmac(&ctx, (const unsigned char *)pass.c_str(),
                                      pass.length(), salt, saltLen, iters, 32, out) == 0;
  mbedtls_md_free(&ctx);
  return ok;
}

// Format: pbkdf2-sha256$<iterations>$<salt hex>$<hash hex>
String hashPassword(const String &pass) {
  uint8_t salt[16], out[32];
  esp_fill_random(salt, sizeof(salt));
  if (!pbkdf2(pass, salt, sizeof(salt), PBKDF2_ITERS, out)) return "";
  return "pbkdf2-sha256$" + String(PBKDF2_ITERS) + "$" + toHex(salt, 16) + "$" + toHex(out, 32);
}

bool checkPassword(const String &pass) {
  const String &h = cfg.dashHash;
  int a = h.indexOf('$'), b = h.indexOf('$', a + 1), c = h.indexOf('$', b + 1);
  if (a < 0 || b < 0 || c < 0) return false;
  uint32_t iters = h.substring(a + 1, b).toInt();
  uint8_t salt[16], want[32], got[32];
  if (iters < 1000 || !fromHex(h.substring(b + 1, c), salt, 16) ||
      !fromHex(h.substring(c + 1), want, 32))
    return false;
  return pbkdf2(pass, salt, 16, iters, got) && equalCT(got, want, 32);
}

bool authEnabled() { return cfg.dashHash.length() > 0; }

uint32_t nowEpoch() {
  time_t t = time(nullptr);
  return t > 1700000000 ? (uint32_t)t : 0;
}

void loadSessions() {
  prefs.begin("yicam", true);
  size_t n = prefs.getBytes("sess", sessions, sizeof(sessions));
  prefs.end();
  if (n != sizeof(sessions)) memset(sessions, 0, sizeof(sessions));
}

void saveSessions() {
  prefs.begin("yicam", false);
  prefs.putBytes("sess", sessions, sizeof(sessions));
  prefs.end();
}

void clearSessions() {
  memset(sessions, 0, sizeof(sessions));
  saveSessions();
}

// ---------------------------------------------------------------------------
// Shared state between the MQTT/web loop and the worker task
// ---------------------------------------------------------------------------
volatile bool motionActive = false;
volatile bool newMotionEvent = false;
volatile bool manualShot = false;
volatile bool captureBusy = false;  // worker is fetching a snapshot from the camera
volatile bool remoteActive = false;   // a "capture for a while" request is running
volatile uint8_t remoteShotFlags = 0;  // flags for the pending manual shot (SLOT_REMOTE / SLOT_LIVE)
volatile bool remoteLive = false;      // the running remote capture is the page's live view
volatile uint32_t remoteEveryMs = 0;   // its interval (0 = the Settings interval)
volatile uint32_t remoteUntilMs = 0;
volatile time_t lastPollOkT = 0;      // last successful check-in with the Apps Script
volatile uint32_t motionSinceMs = 0;

volatile uint32_t statEvents = 0, statCaptured = 0, statCaptureFail = 0,
                  statUploaded = 0, statUploadFail = 0, statDropped = 0;
volatile int mqttClients = 0;
volatile time_t lastUploadT = 0;
// Written by the worker task, read by the status page. Guarded by a mutex
// (not a critical section) because assigning a String allocates.
String lastError, lastUploaded;
SemaphoreHandle_t textMutex;

void setText(String &dst, const String &v) {
  xSemaphoreTake(textMutex, portMAX_DELAY);
  dst = v;
  xSemaphoreGive(textMutex);
}

String getText(const String &src) {
  xSemaphoreTake(textMutex, portMAX_DELAY);
  String copy = src;
  xSemaphoreGive(textMutex);
  return copy;
}

void setError(const String &e) {
  Serial.println("[err] " + e);
  setText(lastError, e);
}

// ---------------------------------------------------------------------------
// MQTT broker
// ---------------------------------------------------------------------------
class Broker : public PicoMQTT::Server {
 protected:
  PicoMQTT::ConnectReturnCode auth(const char *client_id, const char *username,
                                   const char *password) override {
    if (cfg.mqttUser.isEmpty()) return PicoMQTT::CRC_ACCEPTED;  // open (LAN)
    if (username && password && cfg.mqttUser == username &&
        cfg.mqttPass == password)
      return PicoMQTT::CRC_ACCEPTED;
    Serial.printf("[mqtt] rejected client %s\n", client_id);
    return PicoMQTT::CRC_BAD_USERNAME_OR_PASSWORD;
  }
  void on_connected(const char *client_id) override {
    mqttClients++;
    Serial.printf("[mqtt] client connected: %s\n", client_id);
  }
  void on_disconnected(const char *client_id) override {
    if (mqttClients > 0) mqttClients--;
    Serial.printf("[mqtt] client disconnected: %s\n", client_id);
  }
} mqtt;

void onMotionMessage(const char *payload) {
  String p(payload);
  p.trim();
  Serial.printf("[mqtt] %s = %s\n", cfg.motionTopic.c_str(), p.c_str());
  if (p == "motion_stop") {
    motionActive = false;
  } else if (p.length()) {
    // motion_start, or AI labels (human / vehicle / animal) on the same topic
    if (!motionActive) {
      newMotionEvent = true;
      statEvents++;
    }
    motionSinceMs = millis();
    motionActive = true;
  }
}

// ---------------------------------------------------------------------------
// Photo slots on the camera's SD card
// ---------------------------------------------------------------------------
// snapshot.sh?file=NAME makes the camera write the JPEG to /tmp/sd/record/NAME,
// which its web server then serves at /record/NAME with a Content-Length. So a
// capture is one quick request, and photos are streamed one at a time from the
// camera to Drive; nothing is stored on the ESP32. The camera's web UI can only
// delete recordings, not these files, so a fixed ring of names is reused, and a
// slot is overwritten only once its photo is in Drive.
static const int SLOTS = 30;
static const uint8_t SLOT_REMOTE = 1;  // requested from the control page (…-r.jpg)
static const uint8_t SLOT_LIVE = 2;    // live-view frame from the control page (…-l.jpg)
static const uint8_t SLOT_MAX_TRIES = 10;
static const uint8_t VIDEO_MAX_TRIES = 5;
static const uint32_t VIDEO_RETRY_MS = 60UL * 1000UL;

struct Slot {
  uint32_t t;      // capture time (epoch seconds; 0 if the clock wasn't set yet)
  uint8_t state;   // 0 free, 1 waiting for upload
  uint8_t tries;
  uint8_t seq;     // shot number, for the file name
  uint8_t flags;   // SLOT_REMOTE: requested from the control page
};
struct SlotStore {
  Slot s[SLOTS];
  uint8_t next;    // slot the next capture uses
};
SlotStore slotStore;  // only the worker task touches it

volatile uint32_t statPending = 0;
volatile uint32_t statVideoUp = 0, statVideoFail = 0, statVideoSkipped = 0, statVideoQueued = 0;
volatile int videoProgress = -1;  // percent of the clip being uploaded, -1 = none

String jsonEscape(const String &s);  // defined with the web server

void loadSlots() {
  prefs.begin("yicam", true);
  size_t n = prefs.getBytes("slots", &slotStore, sizeof(slotStore));
  prefs.end();
  if (n != sizeof(slotStore)) memset(&slotStore, 0, sizeof(slotStore));
  if (slotStore.next >= SLOTS) slotStore.next = 0;
}

void saveSlots() {
  prefs.begin("yicam", false);
  prefs.putBytes("slots", &slotStore, sizeof(slotStore));
  prefs.end();
}

String slotFile(int i) {
  char b[24];
  snprintf(b, sizeof(b), "yicam-bridge-%02d.jpg", i);
  return String(b);
}

int pendingSlots() {
  int n = 0;
  for (auto &s : slotStore.s) n += s.state == 1;
  return n;
}

bool liveFramePending() {
  for (auto &s : slotStore.s)
    if (s.state == 1 && (s.flags & SLOT_LIVE)) return true;
  return false;
}

// Which photo to upload next. A live-view frame goes first, and only the
// newest one: older waiting frames are already out of date, so their slots
// are freed without uploading. Everything else goes oldest first.
int nextSlotToUpload() {
  int newestLive = -1;
  for (int k = 0; k < SLOTS; k++) {  // ring order: later k = newer
    int i = (slotStore.next + k) % SLOTS;
    if (slotStore.s[i].state == 1 && (slotStore.s[i].flags & SLOT_LIVE)) newestLive = i;
  }
  if (newestLive >= 0) {
    bool freed = false;
    for (auto &s : slotStore.s)
      if (s.state == 1 && (s.flags & SLOT_LIVE) && &s != &slotStore.s[newestLive]) {
        s.state = 0;
        freed = true;
      }
    if (freed) saveSlots();
    return newestLive;
  }
  for (int k = 0; k < SLOTS; k++) {
    int i = (slotStore.next + k) % SLOTS;
    if (slotStore.s[i].state == 1) return i;
  }
  return -1;
}

int oldestPendingSlot() {  // slots are filled in ring order
  for (int k = 0; k < SLOTS; k++) {
    int i = (slotStore.next + k) % SLOTS;
    if (slotStore.s[i].state == 1) return i;
  }
  return -1;
}

void cameraAuth(HTTPClient &http) {
  if (cfg.camUser.length()) http.setAuthorization(cfg.camUser.c_str(), cfg.camPass.c_str());
}

bool captureToCamera(uint8_t seq, uint8_t flags) {
  int i = slotStore.next;
  if (slotStore.s[i].state == 1) {
    statDropped++;
    setError("capture: all " + String(SLOTS) + " camera slots are waiting for upload, shot skipped");
    return false;
  }
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(15000);
  http.setConnectTimeout(5000);
  http.setReuse(false);
  // Live-view frames are low resolution: quick to upload, light on mobile data.
  String res = (flags & SLOT_LIVE) ? String("low") : cfg.res;
  String url = "http://" + cfg.camHost + "/cgi-bin/snapshot.sh?res=" + res +
               "&watermark=yes&file=" + slotFile(i);
  if (!http.begin(client, url)) {
    setError("capture: bad camera address " + cfg.camHost);
    return false;
  }
  cameraAuth(http);
  const char *hdrs[] = {"Content-Type"};
  http.collectHeaders(hdrs, 1);
  int code = http.GET();
  if (code == HTTP_CODE_UNAUTHORIZED) {
    http.end();
    setError("capture: the camera rejected the bridge's login (HTTP 401); check the camera user and password");
    return false;
  }
  // With file= the camera answers a tiny JSON. Anything else (e.g. a JPEG)
  // means it didn't save the file; don't read a whole image into RAM.
  if (code != HTTP_CODE_OK || !http.header("Content-Type").startsWith("application/json")) {
    http.end();
    setError("capture: the camera didn't save the snapshot (HTTP " + String(code) + ")");
    return false;
  }
  String body = http.getString();
  http.end();
  if (body.indexOf("\"false\"") < 0) {
    setError("capture: the camera reported an error saving the snapshot");
    return false;
  }
  time_t now = time(nullptr);
  slotStore.s[i] = {now > 1700000000 ? (uint32_t)now : 0, 1, 0, seq, flags};
  slotStore.next = (i + 1) % SLOTS;
  saveSlots();
  Serial.printf("[capture] slot %d (%s)\n", i, slotFile(i).c_str());
  return true;
}

// ---------------------------------------------------------------------------
// Apps Script calls: Drive token, day folders, "saved" reports
// ---------------------------------------------------------------------------
String urlEncode(const String &s) {
  String out;
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      out += '%';
      out += hex[(c >> 4) & 15];
      out += hex[c & 15];
    }
  }
  return out;
}

bool splitHttpsUrl(const String &url, String &host, String &path) {
  if (!url.startsWith("https://")) return false;
  int slash = url.indexOf('/', 8);
  host = slash < 0 ? url.substring(8) : url.substring(8, slash);
  path = slash < 0 ? "/" : url.substring(slash);
  return host.length() > 0;
}

// Reads the status line and headers; returns the status code and fills in
// the Location and Range headers when present.
int readResponseHead(WiFiClientSecure &c, String &location, String *range = nullptr) {
  uint32_t deadline = millis() + 30000;
  while (!c.available() && c.connected() && millis() < deadline) delay(10);
  String status = c.readStringUntil('\n');
  int code = 0;
  int sp = status.indexOf(' ');
  if (sp > 0) code = status.substring(sp + 1, sp + 4).toInt();
  while (c.connected() || c.available()) {
    String line = c.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) break;
    if (line.substring(0, 9).equalsIgnoreCase("location:")) {
      location = line.substring(9);
      location.trim();
    } else if (range && line.substring(0, 6).equalsIgnoreCase("range:")) {
      *range = line.substring(6);
      range->trim();
    }
  }
  return code;
}

// Value of a string field in a small, flat JSON reply.
String jsonStr(const String &j, const char *key) {
  String k = String("\"") + key + "\"";
  int i = j.indexOf(k);
  if (i < 0) return "";
  i = j.indexOf('"', j.indexOf(':', i + k.length()) + 1);
  if (i < 0) return "";
  int e = j.indexOf('"', i + 1);
  return e < 0 ? "" : j.substring(i + 1, e);
}

// POSTs to the Apps Script web app and returns its JSON reply ("" on network
// failure). Apps Script answers with a 302 to script.googleusercontent.com,
// where the result is read with a GET.
String scriptCall(const String &action, const String &extraQuery, const String &body,
                  bool quiet = false) {
  String host, basePath;
  if (!splitHttpsUrl(cfg.scriptUrl, host, basePath)) {
    if (!quiet) setError("upload: the Apps Script URL is missing or not https://");
    return "";
  }
  String path = basePath + (basePath.indexOf('?') >= 0 ? "&" : "?") + "token=" + urlEncode(cfg.token) +
                "&action=" + action + extraQuery;
  WiFiClientSecure tls;
  tls.setCACert(GOOGLE_ROOT_CAS);
  tls.setTimeout(30);  // seconds
  if (!tls.connect(host.c_str(), 443)) {
    if (!quiet) setError("upload: can't reach " + host);
    return "";
  }
  tls.print("POST " + path + " HTTP/1.1\r\nHost: " + host + "\r\nUser-Agent: yicam-bridge/" +
            String(FW_VERSION) + "\r\nContent-Type: application/json\r\nContent-Length: " +
            String(body.length()) + "\r\nConnection: close\r\n\r\n");
  if (body.length()) tls.print(body);
  String location;
  int code = readResponseHead(tls, location);
  String reply;
  if (code == 200) reply = tls.readString();
  tls.stop();
  if (code == 302 && location.startsWith("https://")) {
    WiFiClientSecure tls2;
    tls2.setCACert(GOOGLE_ROOT_CAS);
    HTTPClient http;
    http.setTimeout(20000);
    if (http.begin(tls2, location)) {
      if (http.GET() == HTTP_CODE_OK) reply = http.getString();
      http.end();
    }
  }
  if (reply.isEmpty() && !quiet) setError("upload: no answer from the Apps Script (HTTP " + String(code) + ")");
  return reply;
}

String accessToken;
uint32_t tokenExpMs = 0;
String folderDay[2], folderIdCache[2];

bool ensureToken() {
  if (accessToken.length() && (int32_t)(tokenExpMs - millis()) > 5L * 60 * 1000) return true;
  String r = scriptCall("token", "", "");
  String t = jsonStr(r, "access_token");
  if (t.isEmpty()) {
    if (r.indexOf("empty body") >= 0)
      setError("upload: the Apps Script is out of date. Paste the new Code.gs and deploy a new version.");
    else if (r.indexOf("unauthorized") >= 0)
      setError("upload: the Apps Script rejected the token; check it in Settings");
    else if (r.length())
      setError("upload: no Drive token from the Apps Script: " + r.substring(0, 120));
    return false;
  }
  accessToken = t;
  long ttl = jsonStr(r, "expires_in").toInt();
  if (ttl <= 0) {  // expires_in is a number, not a string
    int i = r.indexOf("\"expires_in\":");
    ttl = i >= 0 ? r.substring(i + 13).toInt() : 3000;
  }
  tokenExpMs = millis() + (uint32_t)(ttl > 600 ? ttl : 600) * 1000UL;
  return true;
}

String dayFolder(const String &day) {
  for (int i = 0; i < 2; i++)
    if (folderDay[i] == day && folderIdCache[i].length()) return folderIdCache[i];
  String r = scriptCall("folder", "&day=" + day, "");
  String id = jsonStr(r, "id");
  if (id.isEmpty()) {
    if (r.length()) setError("upload: no Drive folder for " + day + ": " + r.substring(0, 100));
    return "";
  }
  folderDay[1] = folderDay[0];
  folderIdCache[1] = folderIdCache[0];
  folderDay[0] = day;
  folderIdCache[0] = id;
  return id;
}

// "saved" reports are batched: one call to the Apps Script for several files.
String savedBatch;
int savedCount = 0;
uint32_t savedSinceMs = 0;

void reportSaved(const String &id, const String &name, const String &folder, const char *kind) {
  if (savedCount >= 40) return;  // the Apps Script is unreachable; uploads matter more
  if (savedCount) savedBatch += ",";
  else savedSinceMs = millis();
  savedBatch += "{\"id\":\"" + id + "\",\"name\":\"" + jsonEscape(name) + "\",\"folder\":\"" + folder +
                "\",\"kind\":\"" + kind + "\"}";
  savedCount++;
}

void flushSaved(bool force) {
  if (!savedCount) return;
  if (!force && savedCount < 10 && millis() - savedSinceMs < 20000) return;
  String r = scriptCall("saved", "", "[" + savedBatch + "]");
  if (r.indexOf("\"ok\":true") >= 0 || millis() - savedSinceMs > 10UL * 60 * 1000) {
    savedBatch = "";
    savedCount = 0;
  }
}

// ---------------------------------------------------------------------------
// Direct uploads to the Drive API
// ---------------------------------------------------------------------------
static const char *DRIVE_HOST = "www.googleapis.com";

enum PumpResult { PUMP_OK, PUMP_SRC_FAIL, PUMP_DST_FAIL, PUMP_ABORTED };

// Copies len bytes from the camera to Drive in small pieces (never a whole file).
PumpResult pump(WiFiClient &in, WiFiClientSecure &out, size_t len, bool abortOnMotion,
                size_t *copied, size_t progressBase = 0, size_t progressTotal = 0) {
  static uint8_t buf[1460];
  *copied = 0;
  uint32_t lastData = millis();
  while (*copied < len) {
    if (abortOnMotion && (motionActive || manualShot || remoteActive)) return PUMP_ABORTED;
    int n = in.available();
    if (n <= 0) {
      if (!in.connected() || millis() - lastData > 15000) return PUMP_SRC_FAIL;
      delay(2);
      continue;
    }
    size_t want = len - *copied;
    if (want > sizeof(buf)) want = sizeof(buf);
    if ((size_t)n < want) want = n;
    int r = in.read(buf, want);
    if (r <= 0) continue;
    if (out.write(buf, r) != (size_t)r) return PUMP_DST_FAIL;
    *copied += r;
    lastData = millis();
    if (progressTotal) videoProgress = (int)(100.0 * (progressBase + *copied) / progressTotal);
  }
  return PUMP_OK;
}

bool driveConnect(WiFiClientSecure &tls) {
  tls.setCACert(GOOGLE_ROOT_CAS);
  tls.setTimeout(30);
  if (tls.connect(DRIVE_HOST, 443)) return true;
  setError("upload: can't reach " + String(DRIVE_HOST));
  return false;
}

// Opens a camera file (optionally from an offset). Returns its remaining size, or -1.
long openCameraFile(HTTPClient &http, WiFiClient &client, const String &path, size_t from, int *codeOut) {
  http.setTimeout(15000);
  http.setConnectTimeout(5000);
  http.setReuse(false);
  if (!http.begin(client, "http://" + cfg.camHost + path)) return -1;
  cameraAuth(http);
  if (from) http.addHeader("Range", "bytes=" + String(from) + "-");
  int code = http.GET();
  if (codeOut) *codeOut = code;
  if (code != (from ? 206 : 200)) return -1;
  return http.getSize();
}

// One-request upload (photos): metadata + bytes, streamed. Returns the file id.
String driveUploadSmall(const String &name, const String &folder, const char *mime, WiFiClient &src,
                        size_t len) {
  WiFiClientSecure tls;
  if (!driveConnect(tls)) return "";
  const char *B = "yicam-bridge-boundary";
  String meta = "{\"name\":\"" + jsonEscape(name) + "\",\"parents\":[\"" + folder + "\"]}";
  String head = String("--") + B + "\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n" + meta +
                "\r\n--" + B + "\r\nContent-Type: " + mime + "\r\n\r\n";
  String tail = String("\r\n--") + B + "--\r\n";
  tls.print(String("POST /upload/drive/v3/files?uploadType=multipart&fields=id HTTP/1.1\r\nHost: ") +
            DRIVE_HOST + "\r\nAuthorization: Bearer " + accessToken +
            "\r\nContent-Type: multipart/related; boundary=" + B +
            "\r\nContent-Length: " + String(head.length() + len + tail.length()) +
            "\r\nConnection: close\r\n\r\n");
  tls.print(head);
  size_t copied;
  PumpResult pr = pump(src, tls, len, false, &copied);
  if (pr != PUMP_OK) {
    tls.stop();
    setError(pr == PUMP_SRC_FAIL ? "upload: the camera stopped sending the photo"
                                 : "upload: the connection to Drive dropped");
    return "";
  }
  tls.print(tail);
  String loc;
  int code = readResponseHead(tls, loc);
  String body = tls.readString();
  tls.stop();
  String id = jsonStr(body, "id");
  if (code == 401) accessToken = "";  // expired early: get a new one next time
  if (code != 200 || id.isEmpty()) setError("upload: Drive answered HTTP " + String(code) + " " + body.substring(0, 120));
  return code == 200 ? id : "";
}

void localNames(time_t t, String &day, String &stamp) {
  struct tm lt;
  localtime_r(&t, &lt);
  char d[12], s[20];
  strftime(d, sizeof(d), "%Y-%m-%d", &lt);
  strftime(s, sizeof(s), "%Y%m%d-%H%M%S", &lt);
  day = d;
  stamp = s;
}

bool uploadSlot(int i) {
  Slot &sl = slotStore.s[i];
  if (!ensureToken()) return false;
  time_t t = sl.t ? (time_t)sl.t : time(nullptr);
  String day, stamp;
  localNames(t, day, stamp);
  char seq[4];
  snprintf(seq, sizeof(seq), "%02u", (unsigned)(sl.seq % 100));
  String name = (sl.t ? stamp : "notime-" + stamp) + "-" + seq +
                ((sl.flags & SLOT_LIVE) ? "-l" : (sl.flags & SLOT_REMOTE) ? "-r" : "") + ".jpg";
  String folder = dayFolder(day);
  if (folder.isEmpty()) return false;

  WiFiClient client;
  HTTPClient http;
  int code = 0;
  long len = openCameraFile(http, client, "/record/" + slotFile(i), 0, &code);
  if (len <= 2048) {
    http.end();
    if (code == 404) {  // gone from the camera (card swapped?): nothing to upload
      sl.state = 0;
      saveSlots();
    }
    setError("upload: can't read " + slotFile(i) + " from the camera (HTTP " + String(code) + ")");
    return false;
  }
  String id = driveUploadSmall(name, folder, "image/jpeg", *http.getStreamPtr(), (size_t)len);
  http.end();
  if (id.isEmpty()) return false;
  sl.state = 0;
  saveSlots();
  // Live frames aren't reported: the script finds the newest one itself when
  // the page asks, which saves the bridge two connections per frame.
  if (!(sl.flags & SLOT_LIVE)) reportSaved(id, name, folder, "photo");
  setText(lastUploaded, name);
  Serial.printf("[upload] %s -> Drive (%ld bytes)\n", name.c_str(), len);
  return true;
}

// ---------------------------------------------------------------------------
// Video clips: announced by the camera on <prefix>/motion_files about 70 s
// after motion stops, uploaded with a resumable Drive upload
// ---------------------------------------------------------------------------
static const int VIDEO_QUEUE_MAX = 60;
struct VideoItem {
  char path[32];  // e.g. 2026Y11M15D02H/16M06S54.mp4 (UTC folder)
  uint8_t tries;
};
struct VideoStore {
  VideoItem q[VIDEO_QUEUE_MAX];
  uint8_t count;
  char day[11];      // local day of the daily counter
  uint16_t today;    // clips accepted today
} videoStore;
SemaphoreHandle_t videoMutex;  // MQTT callback (loop task) and worker task

// Resumable session of the clip at the head of the queue (RAM only).
String videoSession;
long videoSize = 0;
uint32_t nextVideoMs = 0;

void loadVideos() {
  prefs.begin("yicam", true);
  size_t n = prefs.getBytes("videos", &videoStore, sizeof(videoStore));
  prefs.end();
  if (n != sizeof(videoStore) || videoStore.count > VIDEO_QUEUE_MAX) memset(&videoStore, 0, sizeof(videoStore));
  statVideoQueued = videoStore.count;
}

void saveVideos() {  // caller holds videoMutex
  prefs.begin("yicam", false);
  prefs.putBytes("videos", &videoStore, sizeof(videoStore));
  prefs.end();
  statVideoQueued = videoStore.count;
}

bool validClipPath(const String &p) {  // 2026Y11M15D02H/16M06S54.mp4
  if (p.length() != 27) return false;
  const char *s = p.c_str();
  return s[4] == 'Y' && s[7] == 'M' && s[10] == 'D' && s[13] == 'H' && s[14] == '/' &&
         s[17] == 'M' && s[20] == 'S' && p.endsWith(".mp4");
}

String videoTopic() {
  int slash = cfg.motionTopic.lastIndexOf('/');
  return (slash > 0 ? cfg.motionTopic.substring(0, slash) : String("yicam")) + "/motion_files";
}

String localDayNow() {
  time_t now = time(nullptr);
  if (now < 1700000000) return "";
  String day, stamp;
  localNames(now, day, stamp);
  return day;
}

String lastVideoMsg;  // last motion_files payload (start), for the status page

void onMotionFiles(const char *payload) {
  String p(payload), today = localDayNow();
  String brief = p.substring(0, 160);
  brief.replace("\n", " ");
  Serial.printf("[video] %s: %s\n", videoTopic().c_str(), brief.c_str());
  setText(lastVideoMsg, brief);
  if (!cfg.videoOn) {
    Serial.println("[video] clips are turned off in Settings; ignored");
    return;
  }
  // "files":[ "a.mp4", "b.mp4" ] lists newest first; queue oldest first.
  int f = p.indexOf("\"files\"");
  if (f < 0) {
    Serial.println("[video] no \"files\" list in the message; ignored");
    return;
  }
  String found[50];
  int n = 0;
  for (int i = p.indexOf('[', f); i >= 0 && n < 50;) {
    int a = p.indexOf('"', i + 1);
    if (a < 0) break;
    int b = p.indexOf('"', a + 1);
    if (b < 0) break;
    String s = p.substring(a + 1, b);
    if (validClipPath(s)) found[n++] = s;
    i = b;
  }
  xSemaphoreTake(videoMutex, portMAX_DELAY);
  if (today.length() && today != videoStore.day) {
    strncpy(videoStore.day, today.c_str(), sizeof(videoStore.day) - 1);
    videoStore.today = 0;
  }
  int added = 0;
  for (int k = n - 1; k >= 0; k--) {
    bool dup = false;
    for (int q = 0; q < videoStore.count && !dup; q++) dup = found[k] == videoStore.q[q].path;
    if (dup) continue;
    if (videoStore.today >= cfg.videoCap || videoStore.count >= VIDEO_QUEUE_MAX) {
      statVideoSkipped++;
      continue;
    }
    VideoItem &v = videoStore.q[videoStore.count++];
    memset(&v, 0, sizeof(v));
    strncpy(v.path, found[k].c_str(), sizeof(v.path) - 1);
    videoStore.today++;
    added++;
  }
  if (added) saveVideos();
  xSemaphoreGive(videoMutex);
  Serial.printf("[video] %d clip(s) announced, %d queued\n", n, added);
}

void popVideo() {  // worker task: drop the head of the queue
  xSemaphoreTake(videoMutex, portMAX_DELAY);
  if (videoStore.count) {
    memmove(&videoStore.q[0], &videoStore.q[1], sizeof(VideoItem) * (videoStore.count - 1));
    videoStore.count--;
    saveVideos();
  }
  xSemaphoreGive(videoMutex);
  videoSession = "";
  videoSize = 0;
}

// Days since 1970-01-01 for a UTC civil date (the camera's folders are UTC).
long daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468;
}

time_t clipTime(const String &p) {
  int y = p.substring(0, 4).toInt(), mo = p.substring(5, 7).toInt(), d = p.substring(8, 10).toInt();
  int h = p.substring(11, 13).toInt(), mi = p.substring(15, 17).toInt(), s = p.substring(18, 20).toInt();
  return (time_t)(daysFromCivil(y, mo, d) * 86400L + h * 3600L + mi * 60L + s);
}

// Asks Drive how much of the clip it already has. Returns the next offset, or -1.
long resumeOffset(const String &sessionPath) {
  WiFiClientSecure tls;
  if (!driveConnect(tls)) return -1;
  tls.print("PUT " + sessionPath + " HTTP/1.1\r\nHost: " + DRIVE_HOST + "\r\nAuthorization: Bearer " +
            accessToken + "\r\nContent-Length: 0\r\nContent-Range: bytes */" + String(videoSize) +
            "\r\nConnection: close\r\n\r\n");
  String loc, range;
  int code = readResponseHead(tls, loc, &range);
  tls.stop();
  if (code == 308) {
    int dash = range.indexOf('-');
    return dash < 0 ? 0 : range.substring(dash + 1).toInt() + 1;
  }
  if (code == 200 || code == 201) return videoSize;  // already complete
  return -1;  // session expired or gone: start over
}

enum VideoResult { VID_DONE, VID_FAILED, VID_PAUSED, VID_DROP };

VideoResult uploadVideo(const char *clip) {
  String path(clip);
  if (!ensureToken()) return VID_FAILED;
  String day, stamp;
  localNames(clipTime(path), day, stamp);
  String name = stamp + ".mp4";
  String folder = dayFolder(day);
  if (folder.isEmpty()) return VID_FAILED;

  long offset = 0;
  String sessionPath;
  if (videoSession.length()) {
    String h;
    splitHttpsUrl(videoSession, h, sessionPath);
    offset = resumeOffset(sessionPath);
    if (offset < 0) {
      videoSession = "";
      offset = 0;
    }
  }

  if (videoSession.isEmpty()) {  // start a resumable session
    WiFiClient c0;
    HTTPClient h0;
    int code = 0;
    long len = openCameraFile(h0, c0, "/record/" + path, 0, &code);
    h0.end();
    if (len <= 0) {
      setError("video: can't read " + path + " from the camera (HTTP " + String(code) + ")");
      return code == 404 ? VID_DROP : VID_FAILED;  // 404: already deleted by the camera
    }
    videoSize = len;
    WiFiClientSecure tls;
    if (!driveConnect(tls)) return VID_FAILED;
    String meta = "{\"name\":\"" + name + "\",\"parents\":[\"" + folder + "\"]}";
    tls.print(String("POST /upload/drive/v3/files?uploadType=resumable&fields=id HTTP/1.1\r\nHost: ") +
              DRIVE_HOST + "\r\nAuthorization: Bearer " + accessToken +
              "\r\nContent-Type: application/json; charset=UTF-8\r\nX-Upload-Content-Type: video/mp4" +
              "\r\nX-Upload-Content-Length: " + String(videoSize) + "\r\nContent-Length: " +
              String(meta.length()) + "\r\nConnection: close\r\n\r\n" + meta);
    String loc;
    int sc = readResponseHead(tls, loc);
    tls.stop();
    if (sc != 200 || !loc.startsWith("https://")) {
      if (sc == 401) accessToken = "";
      setError("video: Drive didn't start the upload (HTTP " + String(sc) + ")");
      return VID_FAILED;
    }
    videoSession = loc;
    String h;
    splitHttpsUrl(videoSession, h, sessionPath);
    offset = 0;
  }

  if (offset < videoSize) {  // send the rest of the clip in one request
    WiFiClient client;
    HTTPClient http;
    int code = 0;
    long rest = openCameraFile(http, client, "/record/" + path, offset, &code);
    if (rest != videoSize - offset) {
      http.end();
      setError("video: the camera didn't resume " + path + " (HTTP " + String(code) + ")");
      return VID_FAILED;
    }
    WiFiClientSecure tls;
    if (!driveConnect(tls)) {
      http.end();
      return VID_FAILED;
    }
    tls.print("PUT " + sessionPath + " HTTP/1.1\r\nHost: " + DRIVE_HOST + "\r\nAuthorization: Bearer " +
              accessToken + "\r\nContent-Type: video/mp4\r\nContent-Length: " + String(rest) +
              "\r\nContent-Range: bytes " + String(offset) + "-" + String(videoSize - 1) + "/" +
              String(videoSize) + "\r\nConnection: close\r\n\r\n");
    size_t copied;
    PumpResult pr = pump(*http.getStreamPtr(), tls, rest, true, &copied, offset, videoSize);
    http.end();
    if (pr != PUMP_OK) {
      tls.stop();
      videoProgress = -1;
      if (pr == PUMP_ABORTED) {
        Serial.println("[video] paused for motion; will resume");
        return VID_PAUSED;
      }
      setError(pr == PUMP_SRC_FAIL ? "video: the camera stopped sending " + path
                                   : String("video: the connection to Drive dropped; will resume"));
      return VID_FAILED;
    }
    String loc;
    int code2 = readResponseHead(tls, loc);
    String body = tls.readString();
    tls.stop();
    videoProgress = -1;
    if (code2 != 200 && code2 != 201) {
      setError("video: Drive answered HTTP " + String(code2) + " " + body.substring(0, 100));
      return VID_FAILED;
    }
    String id = jsonStr(body, "id");
    reportSaved(id, name, folder, "video");
    Serial.printf("[video] %s -> Drive (%ld bytes)\n", name.c_str(), videoSize);
    return VID_DONE;
  }
  return VID_DONE;  // Drive already had all of it
}

// ---------------------------------------------------------------------------
// Remote control: check in with the Apps Script every pollSec seconds
// ---------------------------------------------------------------------------
// Nothing outside the LAN can reach the bridge, so it asks. Each check-in
// reports a short status (the control page shows it) and returns any capture
// requests from the page. Requests have ids: the bridge acknowledges them on
// the next check-in (sent 2 s later) and ignores ids it has already run, so a
// lost reply can't run a request twice. The script drops requests older than
// 2 minutes, so an offline bridge never takes surprise photos later.
uint32_t nextPollMs = 0;
uint32_t pollBackoffS = 0;
String pendingAcks;        // "id","id" acknowledged on the next check-in
String recentCmds[8];
int recentCmdIdx = 0;

bool seenCmd(const String &id) {
  for (auto &c : recentCmds)
    if (c == id) return true;
  recentCmds[recentCmdIdx] = id;
  recentCmdIdx = (recentCmdIdx + 1) % 8;
  return false;
}

String pollBody() {
  String err = getText(lastError);
  String j;
  j.reserve(400);
  j += "{\"version\":\"" + String(FW_VERSION) + "\",";
  j += "\"uptime_s\":" + String(millis() / 1000) + ",";
  j += "\"poll_s\":" + String(cfg.pollSec) + ",";
  j += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  j += "\"motion\":" + String(motionActive ? "true" : "false") + ",";
  j += "\"remote\":" + String(remoteActive ? "true" : "false") + ",";
  j += "\"live\":" + String(remoteActive && remoteLive ? "true" : "false") + ",";
  j += "\"events\":" + String(statEvents) + ",";
  j += "\"captured\":" + String(statCaptured) + ",";
  j += "\"uploaded\":" + String(statUploaded) + ",";
  j += "\"queued\":" + String(statPending) + ",";
  j += "\"videos_uploaded\":" + String(statVideoUp) + ",";
  j += "\"videos_queued\":" + String(statVideoQueued) + ",";
  j += "\"camera_connected\":" + String(mqttClients > 0 ? "true" : "false") + ",";
  j += "\"last_error\":\"" + jsonEscape(err.substring(0, 160)) + "\",";
  j += "\"ack\":[" + pendingAcks + "]}";
  return j;
}

long jsonNum(const String &j, const char *key) {
  String k = String("\"") + key + "\":";
  int i = j.indexOf(k);
  return i < 0 ? 0 : j.substring(i + k.length()).toInt();
}

void pollRemote() {
  String r = scriptCall("poll", "", pollBody(), true);
  if (r.indexOf("\"ok\":true") < 0) {
    // Network trouble: back off 1, 2, 4… up to 10 min. A script without the
    // poll request (older Code.gs) answers with an error: check rarely.
    pollBackoffS = r.length() ? 600 : (pollBackoffS ? (pollBackoffS * 2 > 600 ? 600 : pollBackoffS * 2) : 60);
    nextPollMs = millis() + pollBackoffS * 1000UL;
    return;
  }
  pollBackoffS = 0;
  lastPollOkT = time(nullptr);
  pendingAcks = "";  // delivered

  bool got = false;
  int c = r.indexOf("\"commands\"");
  for (int i = c < 0 ? -1 : r.indexOf("{", c); i >= 0; i = r.indexOf("{", i + 1)) {
    int end = r.indexOf('}', i);
    if (end < 0) break;
    String obj = r.substring(i, end + 1);
    String id = jsonStr(obj, "id"), type = jsonStr(obj, "type");
    if (id.isEmpty() || id.length() > 40) continue;
    got = true;
    if (pendingAcks.length()) pendingAcks += ",";
    pendingAcks += "\"" + id + "\"";
    if (seenCmd(id)) continue;  // already ran it; just acknowledge again
    long secs = jsonNum(obj, "secs");
    if (type == "snap") {
      manualShot = true;
      remoteShotFlags = SLOT_REMOTE;
      Serial.println("[remote] snapshot requested");
    } else if (type == "burst" || type == "live") {
      bool live = type == "live";
      if (secs < 5) secs = 5;
      if (secs > 300) secs = 300;
      long every = jsonNum(obj, "every");
      if (every && every < 3) every = 3;
      if (every > 60) every = 60;
      bool wasLive = remoteActive && remoteLive;
      remoteUntilMs = millis() + (uint32_t)secs * 1000UL;
      remoteEveryMs = live ? (uint32_t)every * 1000UL : 0;
      remoteLive = live;
      remoteActive = true;
      if (!wasLive) {  // first shot right away (a live renewal keeps its rhythm)
        manualShot = true;
        remoteShotFlags = live ? SLOT_LIVE : SLOT_REMOTE;
      }
      Serial.printf("[remote] %s for %ld s\n", live ? "live view" : "capturing", secs);
    } else if (type == "stop") {
      remoteActive = false;
      remoteLive = false;
      Serial.println("[remote] stopped");
    }
  }
  // Acknowledge new requests quickly so the page can show "picked up".
  // While the live view runs, check in every 10 s so frequency changes and
  // Pause take effect quickly.
  uint32_t interval = (remoteActive && remoteLive && cfg.pollSec > 10) ? 10 : cfg.pollSec;
  nextPollMs = millis() + (got ? 2000UL : interval * 1000UL);
}

// ---------------------------------------------------------------------------
// Worker task: photos first (capture on time, then upload one at a time),
// video clips when there's nothing else to do
// ---------------------------------------------------------------------------
void workerTask(void *) {
  uint32_t shotsThisEvent = 0, seq = 0;
  uint32_t lastShotMs = 0, nextUploadMs = 0;

  for (;;) {
    uint32_t now = millis();

    if (newMotionEvent) {
      newMotionEvent = false;
      shotsThisEvent = 0;
      lastShotMs = 0;
    }
    if (motionActive && now - motionSinceMs > MOTION_FAILSAFE_MS) {
      Serial.println("[motion] no motion_stop received, ending event");
      motionActive = false;
    }

    if (remoteActive && (int32_t)(now - remoteUntilMs) >= 0) {
      remoteActive = false;
      remoteLive = false;
    }
    uint32_t remoteInterval = remoteEveryMs ? remoteEveryMs : cfg.intervalSec * 1000UL;

    bool dueMotion = motionActive && shotsThisEvent < cfg.maxShots &&
                     (lastShotMs == 0 || now - lastShotMs >= cfg.intervalSec * 1000UL);
    // A "capture for a while" request from the control page uses the same
    // interval and shares the schedule with motion, so shots never double up.
    // Live view: no new frame while the previous one is still uploading, so a
    // backlog can't build up; the rate becomes "every N s or as fast as uploads go".
    bool dueRemote = remoteActive && now - lastShotMs >= remoteInterval &&
                     !(remoteLive && liveFramePending());
    if (manualShot || dueMotion || dueRemote) {
      if (dueMotion) shotsThisEvent++;
      lastShotMs = now;
      // Motion shots stay plain; remote ones are marked -r, live frames -l.
      uint8_t flags = remoteShotFlags;
      if (!flags && dueRemote && !dueMotion) flags = remoteLive ? SLOT_LIVE : SLOT_REMOTE;
      if (dueMotion) flags = 0;
      manualShot = false;
      remoteShotFlags = 0;
      captureBusy = true;
      bool ok = captureToCamera((uint8_t)(seq++ % 100), flags);
      captureBusy = false;
      if (ok) {
        statCaptured++;
        nextUploadMs = 0;  // try uploading right away
      } else {
        statCaptureFail++;
      }
    }
    statPending = pendingSlots();

    // Don't start an upload right before the next shot is due.
    bool capturing = (motionActive && shotsThisEvent < cfg.maxShots) || remoteActive;
    uint32_t nextInterval = (motionActive && shotsThisEvent < cfg.maxShots) ? cfg.intervalSec * 1000UL : remoteInterval;
    bool captureSoon = capturing && millis() - lastShotMs + 1500 >= nextInterval;
    if (!captureSoon && WiFi.isConnected()) {
      int slot = nextSlotToUpload();
      if (cfg.remoteOn && cfg.scriptUrl.length() && (int32_t)(millis() - nextPollMs) >= 0) {
        pollRemote();  // short; uploads continue on the next pass
      } else if (slot >= 0 && (int32_t)(millis() - nextUploadMs) >= 0) {
        digitalWrite(LED_PIN, HIGH);
        if (uploadSlot(slot)) {
          statUploaded++;
          lastUploadT = time(nullptr);
        } else {
          statUploadFail++;
          Slot &sl = slotStore.s[slot];
          if (sl.state == 1 && ++sl.tries >= SLOT_MAX_TRIES) {
            sl.state = 0;  // give up on this one so the slot can be reused
            statDropped++;
          }
          saveSlots();
          nextUploadMs = millis() + UPLOAD_RETRY_MS;
        }
        digitalWrite(LED_PIN, LOW);
      } else if (slot < 0 && savedCount && (motionActive || savedCount >= 10 ||
                                             millis() - savedSinceMs >= 3000)) {
        flushSaved(!motionActive);  // report uploads before a long video starts
      } else if (slot < 0 && !motionActive && !remoteActive && videoStore.count &&
                 (int32_t)(millis() - nextVideoMs) >= 0) {
        char clip[32];
        xSemaphoreTake(videoMutex, portMAX_DELAY);
        memcpy(clip, videoStore.q[0].path, sizeof(clip));
        xSemaphoreGive(videoMutex);
        digitalWrite(LED_PIN, HIGH);
        VideoResult r = uploadVideo(clip);
        digitalWrite(LED_PIN, LOW);
        if (r == VID_DONE) {
          statVideoUp++;
          popVideo();
        } else if (r == VID_DROP) {
          statVideoFail++;
          popVideo();
        } else if (r == VID_FAILED) {
          xSemaphoreTake(videoMutex, portMAX_DELAY);
          bool giveUp = videoStore.count && ++videoStore.q[0].tries >= VIDEO_MAX_TRIES;
          if (!giveUp) saveVideos();
          xSemaphoreGive(videoMutex);
          if (giveUp) {
            statVideoFail++;
            popVideo();
          }
          nextVideoMs = millis() + VIDEO_RETRY_MS;
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ---------------------------------------------------------------------------
// Setup portal (WiFiManager)
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Wi-Fi change requested from the web settings page
// ---------------------------------------------------------------------------
// The ESP32 can only join one network at a time, so a change is stored as
// "pending" together with the network it replaces, and applied at boot:
// try the new network, fall back to the old one, and only if both fail open
// the setup portal. The pending record is cleared only once resolved, so a
// power cut half-way just repeats the attempt.
static const int WIFI_TRIES = 3;
static const uint32_t WIFI_TRY_MS = 20000;

enum WifiSwitch { WS_NONE, WS_JOINED_NEW, WS_REVERTED, WS_FAILED };
String wifiResult;  // shown on the dashboard after a switch

bool tryJoin(const String &ssid, const String &pass) {
  for (int a = 1; a <= WIFI_TRIES; a++) {
    Serial.printf("[wifi] joining \"%s\" (try %d/%d)\n", ssid.c_str(), a, WIFI_TRIES);
    WiFi.disconnect();
    delay(200);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());  // also stores it as the saved network
    uint32_t t0 = millis();
    while (millis() - t0 < WIFI_TRY_MS) {
      if (WiFi.status() == WL_CONNECTED) return true;
      digitalWrite(LED_PIN, (millis() / 500) % 2);
      delay(100);
    }
  }
  digitalWrite(LED_PIN, LOW);
  return false;
}

void clearPendingWifi() {
  prefs.begin("yicam", false);
  prefs.remove("pSsid");
  prefs.remove("pPass");
  prefs.remove("oSsid");
  prefs.remove("oPass");
  prefs.end();
}

WifiSwitch applyPendingWifi() {
  prefs.begin("yicam", true);
  String ps = prefs.getString("pSsid", ""), pp = prefs.getString("pPass", "");
  String os = prefs.getString("oSsid", ""), op = prefs.getString("oPass", "");
  prefs.end();
  if (ps.isEmpty()) return WS_NONE;

  WifiSwitch r;
  if (tryJoin(ps, pp)) {
    r = WS_JOINED_NEW;
    wifiResult = "joined:" + ps;
  } else if (os.length() && tryJoin(os, op)) {
    r = WS_REVERTED;
    wifiResult = "reverted:" + ps;
  } else {
    r = WS_FAILED;
  }
  Serial.printf("[wifi] switch to \"%s\": %s\n", ps.c_str(),
                r == WS_JOINED_NEW ? "joined" : r == WS_REVERTED ? "failed, back on the old network"
                                                                 : "failed, opening setup portal");
  clearPendingWifi();
  return r;
}

// ---------------------------------------------------------------------------
// Setup network password, unique per device
// ---------------------------------------------------------------------------
// Random on first boot and kept in NVS, so there's no shared default that
// anyone reading the source would know. It is deliberately NOT derived from
// the chip ID: the MAC address is broadcast by the setup network itself.
// Shown on the serial log at every boot (for a new board) and on the signed-in
// Settings page. Erasing the flash generates a new one.
String setupApPass;

String setupApPassword() {
  if (setupApPass.length()) return setupApPass;
  prefs.begin("yicam", true);
  setupApPass = prefs.getString("apPass", "");
  prefs.end();
  if (setupApPass.length() >= 8) return setupApPass;

  // 12 characters from an alphabet without look-alikes (no 0/O, 1/l/I),
  // shown as xxxx-xxxx-xxxx: about 60 bits, easy to type on a phone.
  static const char *A = "abcdefghjkmnpqrstuvwxyz23456789";
  uint8_t rnd[12];
  esp_fill_random(rnd, sizeof(rnd));
  setupApPass = "";
  for (int i = 0; i < 12; i++) {
    if (i && i % 4 == 0) setupApPass += '-';
    setupApPass += A[rnd[i] % 31];
  }
  prefs.begin("yicam", false);
  prefs.putString("apPass", setupApPass);
  prefs.end();
  return setupApPass;
}

bool portalSaved = false;

void runWiFiSetup(bool forcePortal) {
  WiFiManager wm;
  char interval[8], maxShots[8];
  snprintf(interval, sizeof(interval), "%u", (unsigned)cfg.intervalSec);
  snprintf(maxShots, sizeof(maxShots), "%u", (unsigned)cfg.maxShots);

  // Secrets are never pre-filled; leave blank to keep the stored value.
  WiFiManagerParameter pHdr("<h3>YiCam bridge</h3><p>Leave secret fields blank to keep current values.</p>");
  WiFiManagerParameter pCam("camHost", "Camera IP / host", cfg.camHost.c_str(), 64);
  WiFiManagerParameter pCamUser("camUser", "Camera web user (optional)", cfg.camUser.c_str(), 32);
  WiFiManagerParameter pCamPass("camPass", "Camera web password (secret)", "", 64, "type='password'");
  WiFiManagerParameter pRes("res", "Snapshot resolution (high/low)", cfg.res.c_str(), 5);
  WiFiManagerParameter pUrl("scriptUrl", "Apps Script web app URL", cfg.scriptUrl.c_str(), 200);
  WiFiManagerParameter pToken("token", "Apps Script token (secret)", "", 64, "type='password'");
  WiFiManagerParameter pMUser("mqttUser", "MQTT user for the camera (blank = open)", cfg.mqttUser.c_str(), 32);
  WiFiManagerParameter pMPass("mqttPass", "MQTT password (secret)", "", 64, "type='password'");
  WiFiManagerParameter pTopic("topic", "Motion topic", cfg.motionTopic.c_str(), 96);
  WiFiManagerParameter pInt("interval", "Seconds between shots", interval, 6);
  WiFiManagerParameter pMax("maxShots", "Max shots per motion event", maxShots, 6);
  WiFiManagerParameter pTz("tz", "POSIX timezone", cfg.tz.c_str(), 48);
  WiFiManagerParameter pOta("otaPass", "OTA update password (secret)", "", 64, "type='password'");
  WiFiManagerParameter pDash("dashPass", "Dashboard password (secret, at least 12 characters)", "", 128,
                             "type='password' autocomplete='new-password'");

  WiFiManagerParameter *params[] = {&pHdr, &pCam, &pCamUser, &pCamPass, &pRes,
                                    &pUrl, &pToken, &pMUser, &pMPass, &pTopic,
                                    &pInt, &pMax, &pTz, &pOta, &pDash};
  for (auto *p : params) wm.addParameter(p);

  wm.setSaveConfigCallback([] { portalSaved = true; });
  wm.setSaveParamsCallback([] { portalSaved = true; });
  wm.setConfigPortalTimeout(600);
  wm.setHostname(HOSTNAME);

  String apPass = setupApPassword();
  bool connected = forcePortal ? wm.startConfigPortal(SETUP_AP_NAME, apPass.c_str())
                               : wm.autoConnect(SETUP_AP_NAME, apPass.c_str());

  if (portalSaved) {
    auto keepIfBlank = [](String &dst, const char *v) {
      if (v && strlen(v)) dst = v;
    };
    cfg.camHost = pCam.getValue();
    cfg.camUser = pCamUser.getValue();
    keepIfBlank(cfg.camPass, pCamPass.getValue());
    cfg.res = pRes.getValue();
    cfg.scriptUrl = pUrl.getValue();
    keepIfBlank(cfg.token, pToken.getValue());
    cfg.mqttUser = pMUser.getValue();
    keepIfBlank(cfg.mqttPass, pMPass.getValue());
    cfg.motionTopic = pTopic.getValue();
    cfg.intervalSec = String(pInt.getValue()).toInt();
    cfg.maxShots = String(pMax.getValue()).toInt();
    cfg.tz = pTz.getValue();
    keepIfBlank(cfg.otaPass, pOta.getValue());
    String dash = pDash.getValue();
    if (dash.length() >= MIN_DASH_PASS) {
      String h = hashPassword(dash);
      if (h.length()) {
        cfg.dashHash = h;
        clearSessions();  // a new password signs every device out
        Serial.println("[setup] dashboard password set");
      }
    } else if (dash.length()) {
      Serial.printf("[setup] dashboard password too short (min %u), not changed\n",
                    (unsigned)MIN_DASH_PASS);
    }
    cfg.camHost.trim();
    cfg.scriptUrl.trim();
    saveConfig();
    loadConfig();  // re-apply validation
    Serial.println("[setup] configuration saved");
  }

  if (!connected) {
    Serial.println("[setup] no WiFi, restarting");
    delay(2000);
    ESP.restart();
  }
}

// ---------------------------------------------------------------------------
// Status web server
// ---------------------------------------------------------------------------
WebServer web(80);

String jsonEscape(const String &s) {
  String o;
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((unsigned char)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

void handleStatus() {
  String err = getText(lastError);
  String uploadedName = getText(lastUploaded);
  time_t now = time(nullptr);
  bool synced = now > 1700000000;
  char localTime[24] = "";
  if (synced) {
    struct tm t;
    localtime_r(&now, &t);
    strftime(localTime, sizeof(localTime), "%Y-%m-%d %H:%M:%S", &t);
  }
  String j;
  j.reserve(1200);
  j += "{";
  j += "\"version\":\"" + String(FW_VERSION) + "\",";
  j += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  j += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  j += "\"uptime_s\":" + String(millis() / 1000) + ",";
  j += "\"time_synced\":" + String(synced ? "true" : "false") + ",";
  j += "\"now\":" + String((uint32_t)(synced ? now : 0)) + ",";
  j += "\"local_time\":\"" + String(localTime) + "\",";
  j += "\"camera\":\"" + jsonEscape(cfg.camHost) + "\",";
  j += "\"script_configured\":" + String(cfg.scriptUrl.length() ? "true" : "false") + ",";
  j += "\"res\":\"" + jsonEscape(cfg.res) + "\",";
  j += "\"interval_s\":" + String(cfg.intervalSec) + ",";
  j += "\"max_shots\":" + String(cfg.maxShots) + ",";
  j += "\"topic\":\"" + jsonEscape(cfg.motionTopic) + "\",";
  j += "\"mqtt_auth\":" + String(cfg.mqttUser.length() ? "true" : "false") + ",";
  j += "\"mqtt_clients\":" + String(mqttClients) + ",";
  j += "\"last_upload\":\"" + jsonEscape(uploadedName) + "\",";
  j += "\"last_upload_t\":" + String((uint32_t)lastUploadT) + ",";
  j += "\"motion_active\":" + String(motionActive ? "true" : "false") + ",";
  j += "\"events\":" + String(statEvents) + ",";
  j += "\"captured\":" + String(statCaptured) + ",";
  j += "\"capture_failed\":" + String(statCaptureFail) + ",";
  j += "\"uploaded\":" + String(statUploaded) + ",";
  j += "\"upload_failed\":" + String(statUploadFail) + ",";
  j += "\"dropped\":" + String(statDropped) + ",";
  j += "\"queued\":" + String(statPending) + ",";
  j += "\"slots\":" + String(SLOTS) + ",";
  j += "\"video_on\":" + String(cfg.videoOn ? "true" : "false") + ",";
  j += "\"video_cap\":" + String(cfg.videoCap) + ",";
  j += "\"video_today\":" + String(videoStore.today) + ",";
  j += "\"videos_uploaded\":" + String(statVideoUp) + ",";
  j += "\"videos_queued\":" + String(statVideoQueued) + ",";
  j += "\"videos_failed\":" + String(statVideoFail) + ",";
  j += "\"videos_skipped\":" + String(statVideoSkipped) + ",";
  j += "\"video_progress\":" + String(videoProgress) + ",";
  j += "\"video_topic\":\"" + jsonEscape(videoTopic()) + "\",";
  j += "\"remote_on\":" + String(cfg.remoteOn ? "true" : "false") + ",";
  j += "\"remote_active\":" + String(remoteActive ? "true" : "false") + ",";
  j += "\"poll_s\":" + String(cfg.pollSec) + ",";
  j += "\"last_poll_t\":" + String((uint32_t)lastPollOkT) + ",";
  j += "\"video_last_msg\":\"" + jsonEscape(getText(lastVideoMsg)) + "\",";
  j += "\"heap_free\":" + String(ESP.getFreeHeap()) + ",";
  j += "\"heap_max_block\":" + String(ESP.getMaxAllocHeap()) + ",";
  j += "\"auth\":" + String(authEnabled() ? "true" : "false") + ",";
  j += "\"camera_login\":" + String(cfg.camUser.length() ? "true" : "false") + ",";
  j += "\"busy\":" + String(motionActive || remoteActive || captureBusy || manualShot ? "true" : "false") + ",";
  j += "\"wifi_ssid\":\"" + jsonEscape(WiFi.SSID()) + "\",";
  j += "\"wifi_result\":\"" + jsonEscape(wifiResult) + "\",";
  j += "\"last_error\":\"" + jsonEscape(err) + "\"";
  j += "}";
  web.send(200, "application/json", j);
}

// --- Sign-in -----------------------------------------------------------------
uint8_t loginFails = 0;
uint32_t lockUntilMs = 0;

void redirect(const char *to) {
  web.sendHeader("Location", to);
  web.send(303, "text/plain", "");
}

Session *currentSession() {
  if (!web.hasHeader("Cookie")) return nullptr;
  String c = web.header("Cookie");
  int i = c.indexOf("yb_s=");
  if (i < 0) return nullptr;
  uint8_t tok[16];
  if (!fromHex(c.substring(i + 5, i + 5 + 32), tok, 16)) return nullptr;
  uint32_t now = nowEpoch();
  for (auto &s : sessions) {
    if (s.exp && equalCT(s.tok, tok, 16)) {
      if (now && now > s.exp) {  // expired
        s.exp = 0;
        saveSessions();
        return nullptr;
      }
      return &s;
    }
  }
  return nullptr;
}

bool signedIn() { return !authEnabled() || currentSession() != nullptr; }

// For JSON endpoints: answers 401 (the page then shows the sign-in form).
bool requireSignIn() {
  if (signedIn()) return true;
  web.send(401, "application/json", "{\"error\":\"login\"}");
  return false;
}

// Password check shared by sign-in and the settings page, with one lockout:
// after 5 misses, 30 s, doubling up to 16 min.
enum PwCheck { PW_OK, PW_BAD, PW_LOCKED };

PwCheck verifyPassword(const String &pw) {
  if ((int32_t)(lockUntilMs - millis()) > 0) return PW_LOCKED;
  if (checkPassword(pw)) {
    loginFails = 0;
    return PW_OK;
  }
  loginFails++;
  if (loginFails >= 5) {
    uint8_t shift = loginFails - 5 > 5 ? 5 : loginFails - 5;
    lockUntilMs = millis() + (30000UL << shift);
  }
  Serial.println("[auth] wrong dashboard password");
  return PW_BAD;
}

void startSession(bool remember);
void sendJsonError(int code, const char *err);

void handleLogin() {
  if (!authEnabled()) return redirect("/");
  if ((int32_t)(lockUntilMs - millis()) > 0) return redirect("/?e=locked");
  if (!nowEpoch()) return redirect("/?e=clock");  // need the clock for expiry

  PwCheck pc = verifyPassword(web.arg("password"));
  if (pc == PW_LOCKED) return redirect("/?e=locked");
  if (pc == PW_BAD) return redirect("/?e=bad");

  startSession(web.hasArg("remember"));
  Serial.println("[auth] signed in");
  redirect("/");
}

// Creates a session and sets its cookie on the current response.
void startSession(bool remember) {
  Session *slot = &sessions[0];  // a free slot, or else the oldest one
  for (auto &s : sessions) {
    if (!s.exp) { slot = &s; break; }
    if (s.exp < slot->exp) slot = &s;
  }
  esp_fill_random(slot->tok, sizeof(slot->tok));
  slot->exp = nowEpoch() + (remember ? SESSION_LONG_S : SESSION_SHORT_S);
  saveSessions();

  // HttpOnly: scripts can't read it. SameSite=Strict: other sites can't use it
  // to trigger /snap or /restart.
  String cookie = "yb_s=" + toHex(slot->tok, 16) + "; Path=/; HttpOnly; SameSite=Strict";
  if (remember) cookie += "; Max-Age=" + String(SESSION_LONG_S);
  web.sendHeader("Set-Cookie", cookie);
}

// First dashboard password, set from the browser. Only while none exists:
// until then the dashboard is open to the whole LAN anyway, so letting the
// first visitor set one only closes that gap. Afterwards, changing it needs
// the current password (settings page) or the physical setup portal.
void handleFirstPassword() {
  if (authEnabled()) return sendJsonError(409, "already_set");
  if (!nowEpoch()) return sendJsonError(503, "clock");
  String p = web.arg("newDash");
  if (p.length() < MIN_DASH_PASS) {
    web.send(400, "application/json",
             "{\"error\":\"invalid\",\"field\":\"first\",\"msg\":\"Use at least " + String(MIN_DASH_PASS) +
                 " characters.\"}");
    return;
  }
  String h = hashPassword(p);
  if (h.isEmpty()) return sendJsonError(500, "hash_failed");
  cfg.dashHash = h;  // only the web handlers (this task) read it
  saveConfig();
  clearSessions();
  startSession(true);
  Serial.println("[auth] first dashboard password set from the web");
  web.send(200, "application/json", "{\"ok\":true}");
}

void handleLogout() {
  Session *s = currentSession();
  if (s) {
    memset(s, 0, sizeof(*s));
    saveSessions();
  }
  web.sendHeader("Set-Cookie", "yb_s=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
  redirect("/");
}

// --- Camera preview relay ------------------------------------------------------
// The bridge signs in to the camera with its stored login, so the camera
// password never reaches the browser. One low-res frame per request, streamed
// straight through (never stored). Off until a dashboard password is set,
// so the relay can't bypass the camera's own login.
uint32_t lastPreviewMs = 0;

void sendJsonError(int code, const char *err) {
  web.send(code, "application/json", String("{\"error\":\"") + err + "\"}");
}

void handlePreview() {
  if (!requireSignIn()) return;
  if (!authEnabled()) return sendJsonError(403, "no_password");
  if (motionActive || remoteActive || captureBusy || manualShot) return sendJsonError(503, "busy");
  if (lastPreviewMs && millis() - lastPreviewMs < 2000) return sendJsonError(429, "too_fast");
  lastPreviewMs = millis();

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(8000);
  http.setConnectTimeout(3000);
  http.setReuse(false);
  String url = "http://" + cfg.camHost + "/cgi-bin/snapshot.sh?res=low&watermark=yes";
  if (!http.begin(client, url)) return sendJsonError(502, "camera");
  if (cfg.camUser.length()) http.setAuthorization(cfg.camUser.c_str(), cfg.camPass.c_str());
  int code = http.GET();
  if (code == HTTP_CODE_UNAUTHORIZED) {
    http.end();
    return sendJsonError(502, "camera_login");
  }
  if (code != HTTP_CODE_OK) {
    http.end();
    return sendJsonError(502, "camera");
  }

  web.sendHeader("Cache-Control", "no-store");
  web.setContentLength(CONTENT_LENGTH_UNKNOWN);  // the CGI sends no length
  web.send(200, "image/jpeg", "");
  WiFiClient *in = http.getStreamPtr();
  static uint8_t buf[1460];
  uint32_t deadline = millis() + 8000;
  while ((http.connected() || in->available()) && millis() < deadline) {
    int n = in->available();
    if (n <= 0) {
      delay(2);
      continue;
    }
    n = in->read(buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf));
    if (n > 0) web.sendContent((const char *)buf, n);
  }
  web.sendContent("");  // end of chunked response
  http.end();
}

// --- Settings page API -----------------------------------------------------------
// Only available with a dashboard password set (otherwise anyone on the LAN
// could rewrite the configuration). Saving needs the current password again,
// so a stolen session cookie alone can't change the password or redirect
// uploads. Secrets are never sent to the browser.
uint32_t restartAtMs = 0;  // > 0: restart once the response has gone out

bool requireSettingsAccess() {
  if (!requireSignIn()) return false;
  if (!authEnabled()) {
    sendJsonError(403, "no_password");
    return false;
  }
  return true;
}

void handleSettingsGet() {
  if (!requireSettingsAccess()) return;
  auto has = [](const String &s) { return s.length() ? "true" : "false"; };
  String j;
  j.reserve(700);
  j += "{\"camHost\":\"" + jsonEscape(cfg.camHost) + "\",";
  j += "\"camUser\":\"" + jsonEscape(cfg.camUser) + "\",";
  j += "\"has_camPass\":" + String(has(cfg.camPass)) + ",";
  j += "\"res\":\"" + jsonEscape(cfg.res) + "\",";
  j += "\"scriptUrl\":\"" + jsonEscape(cfg.scriptUrl) + "\",";
  j += "\"has_token\":" + String(has(cfg.token)) + ",";
  j += "\"mqttUser\":\"" + jsonEscape(cfg.mqttUser) + "\",";
  j += "\"has_mqttPass\":" + String(has(cfg.mqttPass)) + ",";
  j += "\"topic\":\"" + jsonEscape(cfg.motionTopic) + "\",";
  j += "\"interval\":" + String(cfg.intervalSec) + ",";
  j += "\"maxShots\":" + String(cfg.maxShots) + ",";
  j += "\"tz\":\"" + jsonEscape(cfg.tz) + "\",";
  j += "\"has_otaPass\":" + String(has(cfg.otaPass)) + ",";
  j += "\"videoOn\":\"" + String(cfg.videoOn ? "on" : "off") + "\",";
  j += "\"videoCap\":" + String(cfg.videoCap) + ",";
  j += "\"remoteOn\":\"" + String(cfg.remoteOn ? "on" : "off") + "\",";
  j += "\"pollSec\":" + String(cfg.pollSec) + ",";
  j += "\"wifi_ssid\":\"" + jsonEscape(WiFi.SSID()) + "\",";
  j += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
  j += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  j += "\"hostname\":\"" + String(HOSTNAME) + ".local\",";
  j += "\"ap_name\":\"" + String(SETUP_AP_NAME) + "\",";
  j += "\"ap_pass\":\"" + setupApPassword() + "\"}";
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json", j);
}

void fieldError(const char *field, const String &msg) {
  web.send(400, "application/json",
           "{\"error\":\"invalid\",\"field\":\"" + String(field) + "\",\"msg\":\"" + jsonEscape(msg) + "\"}");
}

void handleSettingsSave() {
  if (!requireSettingsAccess()) return;
  PwCheck pc = verifyPassword(web.arg("current"));
  if (pc == PW_LOCKED) return sendJsonError(429, "locked");
  if (pc == PW_BAD) return sendJsonError(403, "bad_password");

  auto val = [](const char *k) {
    String v = web.arg(k);
    v.trim();
    return v;
  };
  Config n = cfg;  // copy; the running config stays untouched until restart

  n.camHost = val("camHost");
  if (n.camHost.isEmpty()) return fieldError("camHost", "Enter the camera's IP address or host name.");
  n.camUser = val("camUser");
  if (web.arg("camPass").length()) n.camPass = web.arg("camPass");
  n.res = val("res");
  if (n.res != "high" && n.res != "low") return fieldError("res", "Choose high or low.");

  n.scriptUrl = val("scriptUrl");
  if (n.scriptUrl.length() && !n.scriptUrl.startsWith("https://script.google.com/"))
    return fieldError("scriptUrl", "Use the Apps Script web app URL (https://script.google.com/.../exec).");
  if (web.arg("token").length()) n.token = web.arg("token");

  n.mqttUser = val("mqttUser");
  if (web.arg("mqttPass").length()) n.mqttPass = web.arg("mqttPass");
  n.motionTopic = val("topic");
  if (n.motionTopic.isEmpty() || n.motionTopic.indexOf('#') >= 0 || n.motionTopic.indexOf('+') >= 0)
    return fieldError("topic", "Enter a topic without wildcards, e.g. yicam/motion_detection.");

  long iv = val("interval").toInt(), mx = val("maxShots").toInt();
  if (iv < 1 || iv > 3600) return fieldError("interval", "Use 1 to 3600 seconds.");
  if (mx < 1 || mx > 100) return fieldError("maxShots", "Use 1 to 100 shots.");
  n.intervalSec = iv;
  n.maxShots = mx;

  String von = val("videoOn");
  if (von.length()) {  // older settings pages don't send it
    if (von != "on" && von != "off") return fieldError("videoOn", "Choose on or off.");
    long cap = val("videoCap").toInt();
    if (cap < 1 || cap > 500) return fieldError("videoCap", "Use 1 to 500 clips per day.");
    n.videoOn = von == "on";
    n.videoCap = cap;
  }
  String ron = val("remoteOn");
  if (ron.length()) {  // older settings pages don't send it
    if (ron != "on" && ron != "off") return fieldError("remoteOn", "Choose on or off.");
    long ps = val("pollSec").toInt();
    if (ps < 15 || ps > 300) return fieldError("pollSec", "Use 15 to 300 seconds.");
    n.remoteOn = ron == "on";
    n.pollSec = ps;
  }

  n.tz = val("tz");
  if (n.tz.isEmpty()) return fieldError("tz", "Enter a POSIX timezone, e.g. <-03>3.");
  if (web.arg("otaPass").length()) n.otaPass = web.arg("otaPass");

  String newDash = web.arg("newDash");
  if (newDash.length()) {
    if (newDash.length() < MIN_DASH_PASS)
      return fieldError("newDash", "Use at least " + String(MIN_DASH_PASS) + " characters.");
    n.dashHash = hashPassword(newDash);
    if (n.dashHash.isEmpty()) return sendJsonError(500, "hash_failed");
  }

  String ssid = val("wifiSsid"), wpass = web.arg("wifiPass");
  bool wifiChange = ssid.length() > 0;
  if (wifiChange) {
    if (ssid.length() > 32) return fieldError("wifiSsid", "Network names are at most 32 characters.");
    if (wpass.length() && (wpass.length() < 8 || wpass.length() > 63))
      return fieldError("wifiPass", "Wi-Fi passwords are 8 to 63 characters (leave blank for an open network).");
  }

  saveConfig(n);
  if (newDash.length()) clearSessions();  // sign every device out
  if (wifiChange) {
    prefs.begin("yicam", false);
    prefs.putString("pSsid", ssid);
    prefs.putString("pPass", wpass);
    prefs.putString("oSsid", WiFi.SSID());
    prefs.putString("oPass", WiFi.psk());
    prefs.end();
  }
  Serial.printf("[settings] saved%s%s, restarting\n", wifiChange ? ", Wi-Fi change pending" : "",
                newDash.length() ? ", new dashboard password" : "");
  web.send(200, "application/json",
           String("{\"ok\":true,\"wifi\":") + (wifiChange ? "true" : "false") +
               ",\"signed_out\":" + (newDash.length() ? "true" : "false") + "}");
  restartAtMs = millis() + 800;
}

void handleWifiScan() {
  if (!requireSettingsAccess()) return;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED) {  // none running: start one in the background
    WiFi.scanNetworks(true);
    return web.send(200, "application/json", "{\"scanning\":true}");
  }
  if (n == WIFI_SCAN_RUNNING) return web.send(200, "application/json", "{\"scanning\":true}");

  String j = "{\"networks\":[";
  bool first = true;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (s.isEmpty()) continue;
    bool dup = false;  // results come sorted by signal; keep the strongest
    for (int k = 0; k < i && !dup; k++) dup = WiFi.SSID(k) == s;
    if (dup) continue;
    if (!first) j += ",";
    first = false;
    j += "{\"ssid\":\"" + jsonEscape(s) + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
         ",\"open\":" + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + "}";
  }
  j += "]}";
  WiFi.scanDelete();
  web.send(200, "application/json", j);
}

void setupWeb() {
  const char *headers[] = {"Cookie"};
  web.collectHeaders(headers, 1);

  web.on("/", HTTP_GET, [] {
    web.sendHeader("Cache-Control", "no-cache");
    if (signedIn()) web.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
    else web.send_P(200, "text/html; charset=utf-8", LOGIN_HTML);
  });
  web.on("/login", HTTP_POST, handleLogin);
  web.on("/logout", HTTP_POST, handleLogout);
  web.on("/status", HTTP_GET, [] {
    if (requireSignIn()) handleStatus();
  });
  web.on("/preview", HTTP_GET, handlePreview);
  web.on("/snap", HTTP_POST, [] {
    if (!requireSignIn()) return;
    manualShot = true;
    web.send(202, "application/json", "{\"queued\":true}");
  });
  web.on("/restart", HTTP_POST, [] {
    if (!requireSignIn()) return;
    web.send(200, "application/json", "{\"restarting\":true}");
    restartAtMs = millis() + 500;
  });
  web.on("/settings", HTTP_GET, [] {
    web.sendHeader("Cache-Control", "no-cache");
    if (signedIn()) web.send_P(200, "text/html; charset=utf-8", SETTINGS_HTML);
    else redirect("/");
  });
  web.on("/api/settings", HTTP_GET, handleSettingsGet);
  web.on("/api/settings", HTTP_POST, handleSettingsSave);
  web.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
  web.on("/api/first-password", HTTP_POST, handleFirstPassword);
  web.begin();
  MDNS.addService("http", "tcp", 80);  // http://yicam-bridge.local
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n[boot] yicam-bridge %s\n", FW_VERSION);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  textMutex = xSemaphoreCreateMutex();
  videoMutex = xSemaphoreCreateMutex();

  loadConfig();
  loadSlots();
  loadVideos();
  Serial.printf("[boot] setup network \"%s\", password %s\n", SETUP_AP_NAME, setupApPassword().c_str());

  // Hold BOOT for ~2 s after reset to force the setup portal.
  bool forcePortal = false;
  uint32_t t0 = millis();
  while (millis() - t0 < 2000) {
    digitalWrite(LED_PIN, (millis() / 100) % 2);
    if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
      forcePortal = true;
      break;
    }
    delay(10);
  }
  digitalWrite(LED_PIN, LOW);
  if (forcePortal) Serial.println("[boot] BOOT held: opening setup portal");

  WiFi.setHostname(HOSTNAME);
  WifiSwitch ws = WS_NONE;
  if (forcePortal) clearPendingWifi();  // the portal replaces any pending change
  else ws = applyPendingWifi();
  if (ws != WS_JOINED_NEW && ws != WS_REVERTED)
    runWiFiSetup(forcePortal || ws == WS_FAILED || cfg.scriptUrl.isEmpty());
  loadSessions();
  WiFi.setSleep(false);  // lower latency for the broker
  WiFi.setAutoReconnect(true);
  Serial.printf("[wifi] connected, IP %s\n", WiFi.localIP().toString().c_str());

  configTzTime(cfg.tz.c_str(), "pool.ntp.org", "time.google.com");

  ArduinoOTA.setHostname(HOSTNAME);
  if (cfg.otaPass.length()) ArduinoOTA.setPassword(cfg.otaPass.c_str());
  ArduinoOTA.begin();

  mqtt.subscribe(cfg.motionTopic, [](char *topic, char *payload) {
    onMotionMessage(payload);
  });
  mqtt.subscribe(videoTopic(), [](char *topic, char *payload) {
    onMotionFiles(payload);
  });
  mqtt.begin();
  setupWeb();

  xTaskCreatePinnedToCore(workerTask, "worker", 16384, nullptr, 1, nullptr, 1);
  Serial.println("[boot] ready");
}

void loop() {
  mqtt.loop();
  web.handleClient();
  ArduinoOTA.handle();
  if (restartAtMs && (int32_t)(millis() - restartAtMs) >= 0) ESP.restart();
  delay(1);
}
