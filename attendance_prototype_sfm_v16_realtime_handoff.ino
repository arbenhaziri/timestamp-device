/* Fingerprint and RFID punch terminal, ESP32-C3 4 MB.
 * Partitions: partitions.csv beside this sketch (two 1.5 MB OTA app slots, 896 KB
 * LittleFS). The core uses it automatically; select Partition Scheme "Custom" so the
 * IDE size check matches. The default layout's 1.25 MB slots are too small.
 * Upgrading a unit that runs a default-layout build: send its queued punches first;
 * the filesystem moves and is formatted once on first boot. Going back to a
 * default-layout build requires Erase All Flash.
 * GPIO21 SDA / GPIO6 SCL: PN532 + DS3231; GPIO5 TX / GPIO10 RX / GPIO4 touch: SFM.
 * GPIO0 BCLK / GPIO1 LRC / GPIO7 DIN: MAX98357A voice feedback. GPIO20: relay, held off.
 * Punch API: cardRefCode (fingerprint ID or decimal RFID UID as a string), deviceCode,
 * eventType (1 IN, 0 OUT), eventTimestamp (UTC ISO8601 milliseconds).
 * Clock currently synchronizes over NTP; scans wait until the clock is valid.
 * HTTPS runs without certificate validation, as requested for staging.
 * Old /queue.jsonl records are preserved for download, never sent to the new API.
 * Secrets stay in the local, untracked deanex_prototype_secrets.h.
 */
#include <WiFi.h>
#include <NetworkClient.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h>
#include "fingerprint_protocol.h"
#include <time.h>
#include <sys/time.h>
#include <esp_sntp.h>
#include "punch_protocol.h"
#include "pn532_reader.h"
#include "queue_array_stream.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <esp_partition.h>
#include "VoiceFeedback.h"
#include "deanex_prototype_secrets.h"

// ─────────────────────────────────────────────────────────── configuration ──
const char* AP_SSID = DEANEX_SETUP_AP_SSID;
const char* AP_PASS = DEANEX_SETUP_AP_PASSWORD;

// Used only the very first time the device boots with no /config.json yet.
// After that, everything below is configured from the web page and lives
// on flash — change it there, not here, for any unit already in the field.
//
// Deliberately blank/placeholder: this unit ships to a network we don't
// control, so it must never assume OUR WiFi or OUR server. A blank SSID
// makes the device go straight to AP mode on first boot, forcing whoever
// receives it to configure their own network from the web page — that's
// the intended out-of-box experience, not a bug.
const char* WIFI_SSID_DEFAULT = "";
const char* WIFI_PASS_DEFAULT = "";
const char* SERVER_DEFAULT    = "";

const char* TOKEN_DEFAULT     = "";               // empty = no auth header sent

#define PROTOCOL_VERSION "2"

const char* PUNCHES_PATH_DEFAULT = "";

// No static IP here on purpose. A fixed address is only safe on a network
// you control — this unit is going to someone else's network, where a
// hardcoded 10.0.0.168/gateway 10.0.0.1 would silently associate to their
// WiFi and then be unreachable, because it's on the wrong subnet for their
// router. DHCP is slower to connect but always correct, on any network.
// If the receiving team wants a static IP back for THEIR network, that's a
// two-line change here — see WiFi.config() commented out in setup().

// ───────────────────────────────────────────────────────────────── pin map ──
#define PIN_SDA         21   // PN532 SDA   (also UART0 TX — hence USB CDC)
#define PIN_SCL          6   // PN532 + DS3231 SCL
#define PIN_BOOT         9   // Dev-board BOOT button, active LOW
#define PIN_LED          8   // status LED, active LOW per hardware diagnostic config
#define PIN_RELAY       20   // active HIGH; held off until door control exists
#define PIN_FP_RX       10   // wired to the SFM's TX
#define PIN_FP_TX        5   // wired to the SFM's RX
#define PIN_FP_TOUCH     4   // SFM TOUCH_OUT, active HIGH
#define PIN_I2S_BCLK     0   // MAX98357A BCLK
#define PIN_I2S_WS       1   // MAX98357A LRC
#define PIN_I2S_DIN      7   // MAX98357A DIN

// ──────────────────────────────────────────────────────────────── tuneable ──
#define SFM_CAPACITY      100        // hard limit of the module
#define DEBOUNCE_MS      5000UL
#define FLUSH_RETRY_MS   30000UL
#define LOG_LINES           60
#define MAX_QUEUE_BYTES  (512UL * 1024UL)
#define MAX_BAD_QUEUE_BYTES   32768UL
#define SERVICE_WINDOW_MS    900000UL
#define BOOT_DEBOUNCE_MS         60UL
#define BOOT_SERVICE_HOLD_MS    3000UL
#define BOOT_FACTORY_HOLD_MS   12000UL
#define BOOT_QUEUED_RESET_HOLD_MS 20000UL
#define VOICE_VOLUME          1.0f   // PCM amplitude 0..1; 8 ohm speaker tested clean at 1.0
// LittleFS address in the default layout (default.csv), used by every build
// that predates the partition address stored in NVS.
#define LEGACY_FS_ADDRESS 0x290000UL

#define QUEUE_FILE   "/punches-v2.jsonl"
#define LEGACY_QUEUE_FILE "/queue.jsonl"
#define USERS_FILE   "/users.json"
#define STATE_FILE   "/state.json"
#define CONFIG_FILE  "/config.json"
#define CACERT_FILE  "/ca.pem"   // stored raw, not JSON-wrapped — PEM already has its own newlines
#define BAD_QUEUE_FILE "/punches-v2.bad"

// ────────────────────────────────────────────────────────────────── globals ──

struct DeviceConfig {
  String server;
  String deviceId;
  String token;
  String wifiSsid;
  String wifiPass;
  String punchesPath;
  int eventType = -1;
};

// Runtime configuration — loaded from CONFIG_FILE at boot, editable from the
// web page, applied immediately (no reboot needed for server/id/token changes).
String cfgServer;
String cfgDeviceId;
String cfgToken;
String cfgWifiSsid;
String cfgWifiPass;
String cfgPunchesPath;
int cfgEventType = -1;
String deviceName;
bool haveRtc = false;
volatile bool ntpTimeUpdated = false;
void onNtpSync(struct timeval*) { ntpTimeUpdated = true; }

// Ring status: idle color reflects SERVER reachability (not just WiFi) —
// blue = last contact with the server succeeded, yellow = it didn't (or
// none has happened yet). Punch feedback (green/red/cyan) is a temporary
// override that reverts to this once its own timer elapses — the SFM ring
// hardware does NOT auto-revert on its own, so we track this ourselves.
bool serverOk = false;
// Saved: committed to the queue while WiFi is down; it uploads on reconnect.
enum class PunchFeedback : uint8_t { Unknown, Pending, Accepted, Failed, Saved };
volatile PunchFeedback punchFeedback = PunchFeedback::Unknown;
bool fsReady = false;
bool serviceMode = false;
bool apRunning = false;
uint32_t serviceUntil = 0;
String csrfToken;
uint32_t pendingRevertAt = 0;
bool bootButtonTracking = false;
bool bootButtonConfirmed = false;
bool bootButtonServiceCue = false;
bool bootButtonFactoryCue = false;
bool bootButtonQueuedResetCue = false;
uint32_t bootButtonLowSince = 0;
volatile bool idleRefreshRequested = false;

TaskHandle_t uploaderTaskHandle = nullptr;
QueueHandle_t voiceQueue = nullptr;   // one slot: the newest pending phrase
SemaphoreHandle_t queueMutex = nullptr;
SemaphoreHandle_t networkMutex = nullptr;
SemaphoreHandle_t logMutex = nullptr;
bool syncPrimitivesReady = false;

WebServer server(80);

// Kept as long-lived globals rather than constructed fresh per-call —
// WiFiClientSecure is a heavier object, and reusing one instance is the
// standard, well-tested pattern on ESP32.
WiFiClientSecure secureClient;

bool haveNfc = false, haveFp = false;
String lastPunchLine = "-";
String lastCardRef = "-";
Pn532Reader nfc(PIN_SDA, PIN_SCL);
bool cardArmed = true;

String logBuf[LOG_LINES];
int    logHead = 0;
uint32_t logSeq = 0;

class MutexGuard {
 public:
  MutexGuard(SemaphoreHandle_t mutex, TickType_t waitTicks)
      : mutex_(mutex), held_(mutex == nullptr || xSemaphoreTake(mutex, waitTicks) == pdTRUE) {}
  ~MutexGuard() { if (mutex_ != nullptr && held_) xSemaphoreGive(mutex_); }
  bool acquired() const { return held_; }
 private:
  SemaphoreHandle_t mutex_;
  bool held_;
};

// forward declarations — required because PAGE (below) is a raw string
// literal, which stops the Arduino auto-prototyper from seeing anything
// defined after it.
String utcNow();
int legacyQueueCount();
void handleLegacyQueue();
void recordPunch(const char* ref, const char* source = "finger");
void updateNfc();
void reject(const String& why);
bool readRtc(time_t& out);
bool writeRtc(time_t value);
void handleRoot();
void handleState();
void handleFlush();
void handleClear();
bool flushQueue(uint16_t timeoutMs = 5000);
void requestImmediateUpload();
void uploaderTask(void* parameter);
void announce(bool granted);
void voiceTask(void* parameter);
void handleConfigGet();
void handleConfigSet();
bool requireServiceAccess();
bool requireMutation();
NetworkClient* pickClient();
void loadConfig();
bool saveConfig(const DeviceConfig& cfg);
void i2cRecover();
void setSolid(uint8_t color);
void flash(uint8_t color, uint16_t ms);
void setIdle();
void refreshIdleIfNotBusy();
void makeUuid(char* out);
bool enqueue(const String& line);
bool atomicWrite(const char* path, const String& data);
bool atomicInstallTemp(const char* target, const char* tempPath);
void recoverAtomic(const char* path);
bool validateConfig(const DeviceConfig& cfg, String& error);
void applyConfig(const DeviceConfig& cfg);
void startServiceMode();
void updateServiceMode();
void updateBootButton();
void performPhysicalFactoryReset(bool allowQueuedPunches);
bool physicalPresence();
void addSecurityHeaders();

/* NETWORK
 * DHCP only. The setup AP is enabled for first configuration. After
 * provisioning it is available only for a bounded service window opened by
 * holding the running device's BOOT button until cyan at three seconds, then
 * releasing before red. Holding BOOT during reset is deliberately unsupported
 * because GPIO9 is a C3 strap pin.
 */

void logLine(const String& s);
#include "fingerprint_app.h"

// ═══════════════════════════════════════════════════════════════════ log ══
void logLine(const String& s) {
  MutexGuard guard(logMutex, pdMS_TO_TICKS(100));
  if (!guard.acquired()) return;
  String clock = utcNow();
  if (clock.length()) { clock = clock.substring(0, 19); clock.setCharAt(10, ' '); clock += " UTC"; }
  logBuf[logHead % LOG_LINES] = (clock.length() ? clock : String(millis() / 1000) + "s [clock waiting]") + "  " + s;
  Serial.println(logBuf[logHead % LOG_LINES]);
  logHead++;
  logSeq++;
}

String utcNow() {
  struct timeval now;
  gettimeofday(&now, nullptr);
  char out[25];
  return PunchProtocol::formatTimestamp(now.tv_sec, now.tv_usec, out, sizeof(out)) ? String(out) : String("");
}

static int fromBcd(uint8_t b) {
  return (b & 15) <= 9 && (b >> 4) <= 9 ? (b >> 4) * 10 + (b & 15) : -1;
}
static uint8_t toBcd(int v) { return ((v / 10) << 4) | (v % 10); }

bool readRtc(time_t& out) {
  if (digitalRead(PIN_SDA) == LOW || digitalRead(PIN_SCL) == LOW) return false;
  Wire.beginTransmission(0x68);
  Wire.write(0);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom((uint8_t)0x68, (uint8_t)7) != 7) return false;
  uint8_t r[7]; for (uint8_t& b : r) b = Wire.read();
  Wire.beginTransmission(0x68); Wire.write(0x0f);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom((uint8_t)0x68, (uint8_t)1) != 1) return false;
  if (Wire.read() & 0x80) return false; // oscillator stopped: time is not trustworthy
  tm t = {};
  t.tm_sec = fromBcd(r[0] & 0x7f); t.tm_min = fromBcd(r[1] & 0x7f);
  if (r[2] & 0x40) {
    int hour = fromBcd(r[2] & 0x1f);
    t.tm_hour = hour >= 1 && hour <= 12 ? hour % 12 + ((r[2] & 0x20) ? 12 : 0) : -1;
  } else t.tm_hour = fromBcd(r[2] & 0x3f);
  t.tm_mday = fromBcd(r[4] & 0x3f);
  t.tm_mon = fromBcd(r[5] & 0x1f) - 1;
  t.tm_year = 100 + fromBcd(r[6]) + ((r[5] & 0x80) ? 100 : 0);
  if (t.tm_sec < 0 || t.tm_sec > 59 || t.tm_min < 0 || t.tm_min > 59 ||
      t.tm_hour < 0 || t.tm_hour > 23 || t.tm_mday < 1 || t.tm_mon < 0 || t.tm_mon > 11 ||
      t.tm_year < 120 || t.tm_year > 199) return false;
  tm original = t;
  time_t seconds = mktime(&t); // TZ is set to UTC0 in setup()
  tm check; gmtime_r(&seconds, &check);
  if (seconds < 1700000000 || check.tm_year != original.tm_year || check.tm_mon != original.tm_mon ||
      check.tm_mday != original.tm_mday || check.tm_hour != original.tm_hour) return false;
  out = seconds; return true;
}

bool writeRtc(time_t value) {
  if (digitalRead(PIN_SDA) == LOW || digitalRead(PIN_SCL) == LOW) return false;
  tm t; if (!gmtime_r(&value, &t) || t.tm_year < 100 || t.tm_year > 199) return false;
  Wire.beginTransmission(0x68); Wire.write(0);
  Wire.write(toBcd(t.tm_sec)); Wire.write(toBcd(t.tm_min)); Wire.write(toBcd(t.tm_hour));
  Wire.write(toBcd(t.tm_wday + 1)); Wire.write(toBcd(t.tm_mday));
  Wire.write(toBcd(t.tm_mon + 1)); Wire.write(toBcd(t.tm_year - 100));
  if (Wire.endTransmission() != 0) return false;
  Wire.beginTransmission(0x68); Wire.write(0x0e);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom((uint8_t)0x68, (uint8_t)2) != 2) return false;
  uint8_t control = Wire.read(), status = Wire.read();
  Wire.beginTransmission(0x68); Wire.write(0x0e); Wire.write(control & 0x7f);
  Wire.write((status & 0x7f) | 0x03); // clear oscillator-stop flag after writing valid UTC
  return Wire.endTransmission() == 0;
}

int legacyQueueCount() {
  if (!fsReady) return -1;
  if (!LittleFS.exists(LEGACY_QUEUE_FILE)) return 0;
  File f = LittleFS.open(LEGACY_QUEUE_FILE, FILE_READ);
  if (!f) return -1;
  int n = 0;
  while (f.available()) { if (f.readStringUntil('\n').length() > 4) ++n; }
  return n;
}

void handleLegacyQueue() {
  if (!requireServiceAccess()) return;
  if (!fsReady) { server.send(503, "text/plain", "storage unavailable"); return; }
  File f = LittleFS.open(LEGACY_QUEUE_FILE, FILE_READ);
  if (!f) { server.send(404, "text/plain", "no old queue"); return; }
  server.sendHeader("Content-Disposition", "attachment; filename=legacy-punches.jsonl");
  server.streamFile(f, "application/x-ndjson");
}

void recordPunch(const char* ref, const char* source) {
  // Only the validated fingerprint-match path calls this; suppress rapid repeats.
  const uint32_t startedAt = millis();
  if (!ref || !ref[0]) { reject("no fingerprint ID"); return; }
  static char recent[16][26] = {};
  static uint32_t seen[16] = {};
  static uint8_t slot = 0;
  uint32_t now = millis();
  for (uint8_t i = 0; i < 16; ++i) {
    if (!strcmp(recent[i], ref) && now - seen[i] < DEBOUNCE_MS) return;
  }
  if (!PunchProtocol::validEventType(cfgEventType)) { reject("select device IN / OUT in configuration"); return; }
  String timestamp = utcNow();
  if (!timestamp.length()) { reject("clock not synchronized; connect WiFi and scan again"); return; }
  JsonDocument d;
  PunchProtocol::setPayload(d, ref, cfgDeviceId.c_str(), cfgEventType, timestamp.c_str());
  String line; serializeJson(d, line);
  if (!enqueue(line)) { reject("punch NOT saved: storage unavailable or queue full"); return; }
  strncpy(recent[slot], ref, sizeof(recent[slot]) - 1);
  recent[slot][sizeof(recent[slot]) - 1] = '\0';
  seen[slot] = now;
  slot = (slot + 1) % 16;
  // Offline is not a failure: the punch is safe and waits for WiFi. Red stays
  // reserved for scans that were refused, so it never accompanies "granted".
  punchFeedback = WiFi.status() == WL_CONNECTED ? PunchFeedback::Pending : PunchFeedback::Saved;
  setSolid(punchFeedback == PunchFeedback::Pending ? SFM_RING_CYAN : SFM_RING_YELLOW);
  announce(true); // the punch is durable now; delivery is reported by the ring
  lastPunchLine = String(source) + " " + ref + (cfgEventType == 1 ? " IN" : " OUT") + " | " + timestamp;
  logLine("saved " + lastPunchLine + " (local " + String(millis() - startedAt) + "ms)");
  requestImmediateUpload();
}

bool hasControlChars(const String& s) {
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (c < 0x20 || c == 0x7f) return true;
  }
  return false;
}

bool safeIdentifier(const String& s, size_t maxLen) {
  if (s.length() == 0 || s.length() > maxLen) return false;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (!(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')) return false;
  }
  return true;
}

bool safePath(const String& p) {
  return p.length() >= 2 && p.length() <= 96 && p[0] == '/' &&
         p.indexOf("..") < 0 && p.indexOf('?') < 0 && p.indexOf('#') < 0 &&
         !hasControlChars(p);
}

bool validateConfig(const DeviceConfig& cfg, String& error) {
  if (cfg.server.length() < 8 || cfg.server.length() > 192 ||
      (!cfg.server.startsWith("http://") && !cfg.server.startsWith("https://")) ||
      cfg.server.indexOf('@') >= 0 || cfg.server.indexOf('?') >= 0 ||
      cfg.server.indexOf('#') >= 0 || hasControlChars(cfg.server)) {
    error = "invalid server URL (http/https only, max 192 bytes, no credentials/query/fragment)";
    return false;
  }
  if (!safeIdentifier(cfg.deviceId, 32)) { error = "invalid device_id"; return false; }
  if (cfg.token.length() > 192 || hasControlChars(cfg.token)) { error = "invalid backend token"; return false; }
  if (cfg.wifiSsid.length() > 32 || hasControlChars(cfg.wifiSsid)) { error = "invalid WiFi SSID"; return false; }
  if (cfg.wifiPass.length() > 63 || hasControlChars(cfg.wifiPass)) { error = "invalid WiFi password"; return false; }
  if (cfg.punchesPath.length() && !safePath(cfg.punchesPath)) { error = "invalid punches_path"; return false; }
  if (cfg.eventType < -1 || cfg.eventType > 1) { error = "event_type must be 1 (IN) or 0 (OUT)"; return false; }
  return true;
}

void applyConfig(const DeviceConfig& cfg) {
  MutexGuard guard(networkMutex, portMAX_DELAY);
  cfgServer = cfg.server;
  while (cfgServer.endsWith("/")) cfgServer.remove(cfgServer.length() - 1);
  cfgDeviceId = cfg.deviceId;
  cfgToken = cfg.token;
  cfgWifiSsid = cfg.wifiSsid;
  cfgWifiPass = cfg.wifiPass;
  cfgPunchesPath = cfg.punchesPath;
  cfgEventType = cfg.eventType;
  serverOk = false;
}

// Atomic-ish LittleFS replacement with a recoverable backup. LittleFS has no
// portable fsync primitive in Arduino, so this is the strongest practical
// handoff implementation without moving to the production ESP-IDF journal.
bool atomicInstallTemp(const char* target, const char* tempPath) {
  String backup = String(target) + ".bak";
  LittleFS.remove(backup);
  bool hadTarget = LittleFS.exists(target);
  if (hadTarget && !LittleFS.rename(target, backup)) return false;
  if (!LittleFS.rename(tempPath, target)) {
    if (hadTarget) LittleFS.rename(backup, target);
    return false;
  }
  if (hadTarget) LittleFS.remove(backup);
  return true;
}

void recoverAtomic(const char* path) {
  String temp = String(path) + ".tmp";
  String backup = String(path) + ".bak";
  if (!LittleFS.exists(path) && LittleFS.exists(backup)) LittleFS.rename(backup, path);
  if (LittleFS.exists(temp)) LittleFS.remove(temp);
  if (LittleFS.exists(path) && LittleFS.exists(backup)) LittleFS.remove(backup);
}

bool atomicWrite(const char* path, const String& data) {
  if (!fsReady) return false;
  String temp = String(path) + ".tmp";
  LittleFS.remove(temp);
  File f = LittleFS.open(temp, FILE_WRITE);
  if (!f) return false;
  size_t written = f.write((const uint8_t*)data.c_str(), data.length());
  f.flush();
  size_t stored = f.size();
  f.close();
  if (written != data.length() || stored != data.length()) {
    LittleFS.remove(temp);
    return false;
  }
  File verify = LittleFS.open(temp, FILE_READ);
  bool ok = verify && verify.size() == data.length();
  if (verify) verify.close();
  if (!ok) { LittleFS.remove(temp); return false; }
  return atomicInstallTemp(path, temp.c_str());
}

bool physicalPresence() {
  return digitalRead(PIN_FP_TOUCH) == HIGH;
}

void startServiceMode() {
  serviceMode = true;
  serviceUntil = millis() + SERVICE_WINDOW_MS;
  WiFi.mode(cfgWifiSsid.length() ? WIFI_AP_STA : WIFI_AP);
  if (!apRunning) {
    apRunning = WiFi.softAP(AP_SSID, AP_PASS);
    logLine(apRunning ? "service AP enabled at http://" + WiFi.softAPIP().toString()
                      : "! service AP failed to start");
  }
}

void updateServiceMode() {
  if (!serviceMode || serviceUntil == 0) return;
  // Keep the setup network available whenever the station network is down.
  // Otherwise a wrong password can lock the owner out of configuration.
  if (WiFi.status() != WL_CONNECTED) return;
  if ((int32_t)(millis() - serviceUntil) < 0) return;
  serviceMode = false;
  serviceUntil = 0;
  if (apRunning) {
    WiFi.softAPdisconnect(true);
    apRunning = false;
  }
  if (cfgWifiSsid.length()) WiFi.mode(WIFI_STA);
  else WiFi.mode(WIFI_OFF);
  logLine("setup WiFi window closed; local-IP page remains available while WiFi is connected");
}

void performPhysicalFactoryReset(bool allowQueuedPunches) {
  if (!fsReady) {
    logLine("! factory reset refused: storage is not mounted");
    flash(SFM_RING_RED, 1800);
    return;
  }
  MutexGuard networkGuard(networkMutex, pdMS_TO_TICKS(6000));
  if (!networkGuard.acquired()) { logLine("! reset postponed: upload active; try again"); return; }
  int currentPending = queueCount();
  int legacyPending = legacyQueueCount();
  int pending = (currentPending < 0 || legacyPending < 0) ? -1 : currentPending + legacyPending;
  if (pending < 0) {
    logLine("! factory reset refused: queue is busy/unreadable");
    flash(SFM_RING_RED, 1800);
    return;
  }
  if (pending > 0 && !allowQueuedPunches) {
    logLine("! factory reset refused: " + String(pending) +
            " punch(es) pending; hold BOOT 20 seconds to discard them");
    flash(SFM_RING_RED, 1800);
    return;
  }
  if (haveFp && !eraseFingerprints())
    logLine("! fingerprint erase failed; reset will continue");

  if (pending > 0) {
    logLine("! factory reset discarding " + String(pending) + " pending punch(es)");
  }

  const char* paths[] = {CONFIG_FILE, USERS_FILE, STATE_FILE, QUEUE_FILE,
                         CACERT_FILE, BAD_QUEUE_FILE, LEGACY_QUEUE_FILE, "/queue.bad"};
  for (const char* path : paths) {
    LittleFS.remove(path);
    LittleFS.remove((String(path) + ".tmp").c_str());
    LittleFS.remove((String(path) + ".bak").c_str());
  }
  // Arduino-ESP32 may also cache the last STA credentials in WiFi NVS.
  // Remove that copy so factory reset cannot silently reconnect or retain a
  // site password after /config.json has been erased.
  WiFi.disconnect(true, true);
  logLine("FACTORY RESET completed by physical BOOT hold");
  setSolid(SFM_RING_CYAN);
  delay(500);
  ESP.restart();
}

void updateBootButton() {
  // GPIO9 is BOOT only. Debounce a continuous LOW as a physical press.
  bool low = digitalRead(PIN_BOOT) == LOW;
  if (low) {
    if (!bootButtonTracking) {
      bootButtonTracking = true;
      bootButtonConfirmed = false;
      bootButtonServiceCue = false;
      bootButtonFactoryCue = false;
      bootButtonQueuedResetCue = false;
      bootButtonLowSince = millis();
    }
    uint32_t held = millis() - bootButtonLowSince;
    if (!bootButtonConfirmed && held >= BOOT_DEBOUNCE_MS) {
      bootButtonConfirmed = true;
      logLine("BOOT button held - punches paused");
    }
    if (bootButtonConfirmed && !bootButtonQueuedResetCue && held >= BOOT_QUEUED_RESET_HOLD_MS) {
      bootButtonQueuedResetCue = true;
      setSolid(SFM_RING_PURPLE); // release now permits discarding pending punches
      logLine("BOOT queued-reset threshold reached; release to erase pending punches and reset");
    } else if (bootButtonConfirmed && !bootButtonFactoryCue && held >= BOOT_FACTORY_HOLD_MS) {
      bootButtonFactoryCue = true;
      setSolid(SFM_RING_RED); // release requests reset only if queue is empty
      logLine("BOOT factory-reset threshold reached; hold to 20 seconds to discard pending punches");
    } else if (bootButtonConfirmed && !bootButtonServiceCue &&
               held >= BOOT_SERVICE_HOLD_MS && held < BOOT_FACTORY_HOLD_MS) {
      bootButtonServiceCue = true;
      setSolid(SFM_RING_CYAN); // release now opens a service window
      logLine("BOOT service threshold reached; release before red cue to open setup AP");
    }
    return;
  }

  if (!bootButtonTracking) return;
  uint32_t held = millis() - bootButtonLowSince;
  bool confirmed = bootButtonConfirmed;
  bootButtonTracking = false;
  bootButtonConfirmed = false;
  bootButtonServiceCue = false;
  bootButtonFactoryCue = false;
  bootButtonQueuedResetCue = false;

  if (!confirmed) return;
  if (held >= BOOT_QUEUED_RESET_HOLD_MS) {
    performPhysicalFactoryReset(true);
  } else if (held >= BOOT_FACTORY_HOLD_MS) {
    performPhysicalFactoryReset(false);
  } else if (held >= BOOT_SERVICE_HOLD_MS) {
    startServiceMode();
    logLine("physical BOOT hold opened a 15-minute service window");
    refreshIdleIfNotBusy();
  } else {
    logLine("BOOT hold too short; no action");
    refreshIdleIfNotBusy();
  }
}

void addSecurityHeaders() {
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.sendHeader("X-Frame-Options", "DENY");
  server.sendHeader("Referrer-Policy", "no-referrer");
}

// ═══════════════════════════════════════════════════════════════════ setup ══
void setup() {
  // Drive the relay off before anything else; the pin floats until here.
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, LOW);
  Serial.begin(115200);
  setenv("TZ", "UTC0", 1); tzset();
  queueMutex = xSemaphoreCreateMutex();
  networkMutex = xSemaphoreCreateMutex();
  logMutex = xSemaphoreCreateMutex();
  syncPrimitivesReady = queueMutex && networkMutex && logMutex;
  fingerprintUart.begin(115200, SERIAL_8N1, PIN_FP_RX, PIN_FP_TX);

  pinMode(PIN_FP_TOUCH, INPUT_PULLDOWN);
  pinMode(PIN_BOOT, INPUT_PULLUP);

  Preferences bootPrefs;
  bootPrefs.begin("timestamp", false);
  bool fsWasInitialized = bootPrefs.getBool("fs_init", false);
  // NVS survives a partition-layout change but the filesystem moves. Track
  // where it was initialized so a new layout is formatted once instead of
  // reading as a broken field filesystem. Builds without this key only ever
  // used the default layout.
  const esp_partition_t* fsPartition =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  uint32_t fsAddress = fsPartition ? fsPartition->address : 0;
  uint32_t fsKnownAddress = bootPrefs.getUInt("fs_addr", fsWasInitialized ? LEGACY_FS_ADDRESS : 0);
  bool fsLayoutChanged = fsWasInitialized && fsAddress && fsKnownAddress != fsAddress;
  fsReady = LittleFS.begin(false); // NEVER format automatically on a known field filesystem
  if (!fsReady && (!fsWasInitialized || fsLayoutChanged)) {
    // First manufacturing boot, or first boot on a new partition layout: the
    // partition may not hold a filesystem yet. Otherwise mount failure never formats.
    if (LittleFS.format()) fsReady = LittleFS.begin(false);
    if (fsReady && fsLayoutChanged) logLine("partition layout changed - filesystem formatted once");
  }
  if (fsReady && !fsWasInitialized) bootPrefs.putBool("fs_init", true);
  if (fsReady && fsAddress && fsKnownAddress != fsAddress) bootPrefs.putUInt("fs_addr", fsAddress);
  bootPrefs.end();
  if (!syncPrimitivesReady) fsReady = false;
  if (!fsReady) {
    logLine("! LittleFS mount failed - attendance disabled; no auto-format performed");
  } else {
    recoverAtomic(CONFIG_FILE);
    recoverAtomic(USERS_FILE);
    recoverAtomic(STATE_FILE);
    recoverAtomic(CACERT_FILE);
    recoverAtomic(QUEUE_FILE);
    recoverAtomic(LEGACY_QUEUE_FILE);
  }

  uint64_t chipId = ESP.getEfuseMac();
  char name[32];
  snprintf(name, sizeof(name), "timestamp-%04X%08X", (unsigned)((chipId >> 32) & 0xFFFF), (unsigned)(chipId & 0xFFFFFFFF));
  deviceName = name;
  loadConfig();
  pinMode(PIN_LED, OUTPUT); digitalWrite(PIN_LED, HIGH);
  esp_sntp_set_time_sync_notification_cb(onNtpSync);
  char csrf[37]; makeUuid(csrf); csrfToken = csrf;
  logLine("server: " + cfgServer + "   device id: " + cfgDeviceId +
           "   auth: " + (cfgToken.length() ? "token set" : "none") +
           "   HTTPS: certificate verification off");

  /* GPIO9 is a strapping pin — held LOW at reset the C3 enters download mode.
   * GPIO6 is I2C SCL; recover the bus if a reset cut a transfer short.
   *
   * GPIO21 is UART0 TX: the ROM bootloader prints its banner there at every
   * boot; release the lines and check the bus before I2C starts. */
  delay(300);
  i2cRecover();
  Wire.begin(PIN_SDA, PIN_SCL, 100000);
  Wire.setTimeOut(50); // PN532 may stretch SCL briefly while waking
  time_t rtcTime;
  haveRtc = readRtc(rtcTime);
  if (haveRtc) { timeval tv = {rtcTime, 0}; settimeofday(&tv, nullptr); }
  logLine(String("DS3231: ") + (haveRtc ? "ok, clock restored" : "not ready"));

  logLine("boot");

  // PN532 is probed later by bounded I2C steps in the main loop. A stuck bus
  // cannot hold up WiFi, fingerprint startup, or the durable punch queue.
  logLine("PN532: starting in background");

  logLine("fingerprint: connecting on TX5 / RX10");

  if (legacyQueueCount() > 0) logLine("! old-format punches preserved: download from terminal before reset");
  logLine("device name: " + deviceName + "; fingerprint + RFID punches");

  WiFi.setSleep(false);                 // no power-save = faster reconnects, more current

  // Start STA-only with saved credentials. If joining fails, open the setup
  // AP so the WiFi settings remain reachable without a factory reset.
  if (!fsReady || cfgWifiSsid.length() == 0) {
    startServiceMode();
  } else {
    WiFi.mode(WIFI_STA);
    apRunning = false;
    logLine("service AP disabled - hold BOOT 3 seconds while running to enable for 15 minutes");
  }

  if (cfgWifiSsid.length() == 0) {
    logLine("no wifi configured yet — connect to the setup AP above to add it");
  } else {
    // No WiFi.config() here — DHCP only. A static IP is only safe on a
    // network you control; this unit may end up on any network, and a
    // hardcoded address would silently be wrong on someone else's subnet.
    WiFi.begin(cfgWifiSsid.c_str(), cfgWifiPass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(50);

    if (WiFi.status() == WL_CONNECTED) {
      logLine("wifi ok — http://" + WiFi.localIP().toString());
      configTime(0, 0, "pool.ntp.org", "time.google.com");
    } else {
      logLine("could not join '" + cfgWifiSsid + "' — check the password in Device configuration");
      startServiceMode();
    }
  }

  const char* headers[] = {"X-CSRF-Token", "Content-Length"};
  server.collectHeaders(headers, 2);
  server.on("/",                 HTTP_GET,  handleRoot);
  server.on("/api/state",        HTTP_GET,  handleState);
  server.on("/api/legacy-queue", HTTP_GET, handleLegacyQueue);
  server.on("/api/enroll", HTTP_POST, handleEnroll);
  server.on("/api/enroll/cancel", HTTP_POST, handleEnrollCancel);
  server.on("/api/flush",        HTTP_POST, handleFlush);
  server.on("/api/clear",        HTTP_POST, handleClear);
  server.on("/api/config",    HTTP_GET,  handleConfigGet);
  server.on("/api/config",    HTTP_POST, handleConfigSet);
  server.onNotFound([](){ addSecurityHeaders(); server.send(404, "text/plain", "not found"); });
  server.begin();
  logLine("web server up");

  if (syncPrimitivesReady) {
    BaseType_t created = xTaskCreate(
      uploaderTask, "punch-uploader", 8192, nullptr, 1,
      &uploaderTaskHandle
    );
    if (created != pdPASS) {
      uploaderTaskHandle = nullptr;
      fsReady = false; // never accept a punch that cannot enter the delivery path
      logLine("! uploader task creation failed - attendance disabled");
    } else {
      logLine("real-time uploader ready");
      // handleWifi() wakes it on the first connected loop iteration. Avoid a
      // second queued notification and duplicate POST during startup.
    }
  }

  // Speech never gates attendance: without I2S the terminal runs silently.
  voiceQueue = xQueueCreate(1, sizeof(uint8_t));
  if (voiceQueue && VoiceFeedback::begin(PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DIN, VOICE_VOLUME) &&
      xTaskCreate(voiceTask, "voice-feedback", 4096, nullptr, 2, nullptr) == pdPASS) {
    logLine("voice feedback ready");
  } else {
    if (voiceQueue) { vQueueDelete(voiceQueue); voiceQueue = nullptr; }
    VoiceFeedback::end();
    logLine("! voice feedback unavailable - punches continue without speech");
  }

  setIdle();   // yellow until the first successful contact with the server
  logLine("ready");
}

// ════════════════════════════════════════════════════════════════════ loop ══
void loop() {
  server.handleClient();
  handleWifi();
  updateServiceMode();
  updateBootButton();
  updateFingerprint();
  updateNfc();

  static uint32_t ledTick = 0, rtcTick = 0;
  if (millis() - ledTick >= 500) { ledTick = millis(); digitalWrite(PIN_LED, !digitalRead(PIN_LED)); }
  // The separate UART keeps working even when I2C is physically held LOW.
  // Defer I2C maintenance while a finger operation needs prompt attention.
  if (!fingerprintBusy() && !bootButtonTracking) {
    if (ntpTimeUpdated) {
      ntpTimeUpdated = false;
      haveRtc = writeRtc(time(nullptr));
      logLine(String("DS3231 UTC update: ") + (haveRtc ? "ok" : "unavailable; using NTP clock"));
    }
    if (millis() - rtcTick >= 60000) {
      rtcTick = millis(); time_t sample;
      bool ok = readRtc(sample);
      if (ok != haveRtc) logLine(String("DS3231: ") + (ok ? "ok" : "not ready"));
      haveRtc = ok;
    }
  }
  if (pendingRevertAt && (int32_t)(millis() - pendingRevertAt) >= 0) {
    pendingRevertAt = 0; setIdle();
  }
  if (idleRefreshRequested) {
    idleRefreshRequested = false; refreshIdleIfNotBusy();
  }
  delay(1);
}

void updateNfc() {
  uint8_t uid[10], uidLength = 0;
  Pn532Reader::Event event = nfc.tick(uid, uidLength, !fingerprintBusy() && !bootButtonTracking);
  switch (event) {
    case Pn532Reader::Event::Ready:
      haveNfc = true;
      logLine("PN532: ok; waiting for RFID card");
      break;
    case Pn532Reader::Event::Unavailable:
      haveNfc = false;
      cardArmed = true;
      logLine("PN532: NOT FOUND (" + String(nfc.lastError()) + "); retrying every 10s");
      break;
    case Pn532Reader::Event::Lost:
      haveNfc = false;
      cardArmed = true;
      logLine("PN532: communication lost (" + String(nfc.lastError()) + "); retrying every 10s");
      break;
    case Pn532Reader::Event::NoCard:
      cardArmed = true; // require removal before the same held card can punch again
      break;
    case Pn532Reader::Event::Card: {
      if (!cardArmed || fingerprintBusy() || bootButtonTracking) break;
      cardArmed = false;
      char ref[26], hex[21];
      if (!PunchProtocol::formatUidDecimal(uid, uidLength, false, ref, sizeof(ref)) ||
          !PunchProtocol::formatUid(uid, uidLength, hex, sizeof(hex))) {
        reject("invalid RFID UID; no punch");
        break;
      }
      lastCardRef = ref;
      logLine("RFID UID " + String(hex) + " = decimal " + ref);
      recordPunch(ref, "RFID");
      break;
    }
    case Pn532Reader::Event::None:
      break;
  }
}

// ══════════════════════════════════════════════════════════ offline queue ══
bool enqueue(const String& line) {
  MutexGuard guard(queueMutex, pdMS_TO_TICKS(750));
  if (!guard.acquired()) {
    logLine("! queue busy - punch not committed");
    return false;
  }
  if (!fsReady || line.length() < 20 || line.length() > 384) return false;
  size_t oldSize = 0;
  if (LittleFS.exists(QUEUE_FILE)) {
    File old = LittleFS.open(QUEUE_FILE, FILE_READ);
    if (!old) return false;
    oldSize = old.size(); old.close();
  }
  if (oldSize + line.length() + 1 > MAX_QUEUE_BYTES) {
    logLine("! queue capacity reached - service required before more punches");
    return false;
  }
  File f = LittleFS.open(QUEUE_FILE, FILE_APPEND);
  if (!f) { logLine("! queue write failed"); return false; }
  size_t written = f.print(line);
  written += f.print('\n');
  f.flush();
  size_t newSize = f.size();
  f.close();
  if (written != line.length() + 1 || newSize != oldSize + line.length() + 1) {
    logLine("! incomplete queue append");
    return false;
  }
  return true;
}

int queueCount() {
  MutexGuard guard(queueMutex, pdMS_TO_TICKS(250));
  if (!guard.acquired()) return -1;
  if (!fsReady) return -1;
  if (!LittleFS.exists(QUEUE_FILE)) return 0;
  File f = LittleFS.open(QUEUE_FILE, FILE_READ);
  if (!f) return -1;
  int n = 0;
  while (f.available()) { String l = f.readStringUntil('\n'); if (l.length() > 4) n++; }
  f.close();
  return n;
}

bool quarantineLine(const String& line) {
  if (!fsReady) return false;
  size_t current = 0;
  if (LittleFS.exists(BAD_QUEUE_FILE)) {
    File r = LittleFS.open(BAD_QUEUE_FILE, FILE_READ);
    if (r) { current = r.size(); r.close(); }
  }
  if (current + line.length() + 1 > MAX_BAD_QUEUE_BYTES) return false;
  File f = LittleFS.open(BAD_QUEUE_FILE, FILE_APPEND);
  if (!f) return false;
  size_t n = f.print(line); n += f.print('\n'); f.flush(); f.close();
  return n == line.length() + 1;
}

bool validQueuedPunch(const String& line) {
  if (line.length() < 20 || line.length() > 384) return false;
  JsonDocument d;
  return !deserializeJson(d, line) && PunchProtocol::validPayload(d);
}

// Remove corrupt/torn lines before batching. They are retained separately for
// forensic inspection instead of blocking every later valid punch forever.
bool sanitizeQueue() {
  if (!LittleFS.exists(QUEUE_FILE)) return true;
  File in = LittleFS.open(QUEUE_FILE, FILE_READ);
  if (!in) return false;
  bool changed = false;
  if (in.size()) {
    if (!in.seek(in.size() - 1)) { in.close(); return false; }
    changed = in.read() != '\n';
    if (!in.seek(0)) { in.close(); return false; }
  }
  while (in.available()) {
    String line = in.readStringUntil('\n');
    String clean = line; clean.trim();
    if (clean != line || !clean.length() || !validQueuedPunch(clean)) changed = true;
  }
  in.close();
  if (!changed) return true;

  in = LittleFS.open(QUEUE_FILE, FILE_READ);
  if (!in) return false;
  String temp = String(QUEUE_FILE) + ".tmp";
  LittleFS.remove(temp);
  File out = LittleFS.open(temp, FILE_WRITE);
  if (!out) { in.close(); return false; }
  bool ok = true;
  while (in.available()) {
    String line = in.readStringUntil('\n'); line.trim();
    if (!line.length()) continue;
    if (validQueuedPunch(line)) {
      size_t n = out.print(line); n += out.print('\n');
      if (n != line.length() + 1) ok = false;
    } else {
      if (!quarantineLine(line)) ok = false;
      logLine("! corrupt queue line quarantined");
    }
  }
  out.flush(); in.close(); out.close();
  if (!ok) { LittleFS.remove(temp); return false; }
  return atomicInstallTemp(QUEUE_FILE, temp.c_str());
}

bool removeQueuePrefix(size_t cutOffset) {
  File in = LittleFS.open(QUEUE_FILE, FILE_READ);
  if (!in || cutOffset > in.size()) { if (in) in.close(); return false; }
  in.seek(cutOffset);
  String temp = String(QUEUE_FILE) + ".tmp";
  LittleFS.remove(temp);
  File out = LittleFS.open(temp, FILE_WRITE);
  if (!out) { in.close(); return false; }
  uint8_t buf[256];
  size_t copied = 0;
  while (in.available()) {
    size_t n = in.read(buf, sizeof(buf));
    if (!n || out.write(buf, n) != n) { in.close(); out.close(); LittleFS.remove(temp); return false; }
    copied += n;
  }
  out.flush(); in.close();
  bool sizeOk = out.size() == copied; out.close();
  if (!sizeOk) { LittleFS.remove(temp); return false; }
  if (copied == 0) {
    LittleFS.remove(temp);
    return LittleFS.remove(QUEUE_FILE) || !LittleFS.exists(QUEUE_FILE);
  }
  return atomicInstallTemp(QUEUE_FILE, temp.c_str());
}

NetworkClient* pickClient() {
  if (!cfgServer.startsWith("https://")) { logLine("! configure an HTTPS punch server"); return nullptr; }
  if (!utcNow().length()) { logLine("HTTPS waiting for clock synchronization"); return nullptr; }
  // Reattach on every request: HTTPClient::end() closes the preceding session.
  secureClient.setInsecure();
  return &secureClient;
}

class PunchResponseBuffer : public Stream {
 public:
  static constexpr size_t LIMIT = 2048;
  String text;
  bool truncated = false;
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  size_t write(uint8_t b) override {
    if (text.length() < LIMIT) text += char(b); else truncated = true;
    return 1;
  }
  size_t write(const uint8_t* data, size_t size) override {
    size_t keep = size < LIMIT - text.length() ? size : LIMIT - text.length();
    if (keep) text.concat(reinterpret_cast<const char*>(data), keep);
    if (keep < size) truncated = true;
    return size;
  }
  bool parseMeta(int& statusCode, String& message) const {
    JsonDocument filter;
    filter["statusCode"] = true;
    filter["message"] = true;
    auto parse = [&](const String& json) {
      JsonDocument result;
      if (deserializeJson(result, json, DeserializationOption::Filter(filter)) ||
          !result["statusCode"].is<int>() || !result["message"].is<const char*>()) return false;
      statusCode = result["statusCode"].as<int>();
      message = result["message"].as<String>();
      return true;
    };
    if (!truncated) return parse(text);

    // The API puts statusCode and message before its potentially large data
    // field. Try complete top-level prefixes, never a partial JSON string.
    bool quoted = false, escaped = false;
    int depth = 0;
    for (size_t i = 0; i < text.length(); ++i) {
      char c = text[i];
      if (quoted) {
        if (escaped) escaped = false;
        else if (c == '\\') escaped = true;
        else if (c == '"') quoted = false;
      } else if (c == '"') quoted = true;
      else if (c == '{' || c == '[') ++depth;
      else if (c == '}' || c == ']') --depth;
      else if (c == ',' && depth == 1) {
        String prefix = text.substring(0, i) + "}";
        if (parse(prefix)) return true;
      }
    }
    return false;
  }
};

// Frame a newline-delimited snapshot as one JSON array without loading the
// (up to 512 KB) queue into RAM. The final newline becomes the closing bracket.
class QueueArrayStream : public Stream {
 public:
  QueueArrayStream(File& file, size_t snapshotBytes)
    : encoder(file, snapshotBytes) {}
  int available() override { return encoder.available(); }
  int read() override {
    char b;
    return readBytes(&b, 1) == 1 ? uint8_t(b) : -1;
  }
  int peek() override { return encoder.peek(); }
  size_t readBytes(char* buffer, size_t length) override { return encoder.readBytes(buffer, length); }
  size_t write(uint8_t) override { return 0; }
  bool complete() const { return encoder.complete(); }
 private:
  QueueArrayEncoder<File> encoder;
};

// Called only by the uploader task. Queue file access is locked only while a
// complete snapshot is validated and while its acknowledged prefix is compacted;
// the mutex is deliberately released during the potentially slow HTTP POST,
// so a new punch can still append immediately. Appends occur only at the tail,
// therefore the saved prefix offset remains valid.
bool flushQueue(uint16_t timeoutMs) {
  const uint32_t startedAt = millis();
  if (!fsReady || WiFi.status() != WL_CONNECTED) {
    serverOk = false;
    punchFeedback = PunchFeedback::Failed;
    idleRefreshRequested = true;
    return false;
  }

  File queueFile;
  int n = 0;
  size_t cutOffset = 0;
  {
    MutexGuard queueGuard(queueMutex, pdMS_TO_TICKS(1000));
    if (!queueGuard.acquired()) return false;
    if (!LittleFS.exists(QUEUE_FILE)) return true;
    if (!sanitizeQueue()) {
      logLine("! queue validation failed; upload stopped");
      return false;
    }

    queueFile = LittleFS.open(QUEUE_FILE, FILE_READ);
    if (!queueFile) return false;
    cutOffset = queueFile.size();
    if (cutOffset == 0) { queueFile.close(); return true; }
    while (queueFile.position() < cutOffset) {
      int b = queueFile.read();
      if (b < 0) { queueFile.close(); return false; }
      if (b == '\n') ++n;
    }
    if (n == 0 || !queueFile.seek(0)) {
      logLine("! queue framing failed; upload stopped");
      queueFile.close();
      return false;
    }
  }
  const uint32_t snapshotDoneAt = millis();

  int code = -1, apiStatusCode = -1;
  String apiMessage;
  bool apiResponseValid = false, uploadComplete = false, bodyComplete = false;
  uint32_t postDoneAt = snapshotDoneAt, bodyDoneAt = snapshotDoneAt;
  int bodyReadResult = -1;
  {
    MutexGuard networkGuard(networkMutex, pdMS_TO_TICKS(6000));
    if (!networkGuard.acquired()) return false;
    NetworkClient* client = pickClient();
    if (!client) {
      serverOk = false;
      punchFeedback = PunchFeedback::Failed;
      idleRefreshRequested = true;
      return false;
    }

    HTTPClient http;
    http.setTimeout(timeoutMs);
    if (!http.begin(*client, String(cfgServer + cfgPunchesPath))) {
      logLine("sync failed: invalid transport or URL");
      serverOk = false;
      punchFeedback = PunchFeedback::Failed;
      idleRefreshRequested = true;
      return false;
    }
    http.addHeader("Content-Type", "application/json");
    if (cfgToken.length()) http.addHeader("X-Api-Key", cfgToken);
    QueueArrayStream batch(queueFile, cutOffset);
    code = http.sendRequest("POST", &batch, cutOffset + 1);
    postDoneAt = millis();
    uploadComplete = batch.complete();
    if (code > 0) {
      // The server has already returned its status. A missing/slow error body
      // must not leave the user waiting through the full upload timeout.
      http.setTimeout(code == 200 ? timeoutMs : 500);
      // HTTPClient decodes both Content-Length and chunked responses here.
      // Keep only a small prefix in RAM; the API response also contains the
      // full punch record, which does not belong in the device log.
      PunchResponseBuffer response;
      bodyReadResult = http.writeToStream(&response);
      bodyComplete = bodyReadResult >= 0;
      apiResponseValid = response.parseMeta(apiStatusCode, apiMessage);
    }
    bodyDoneAt = millis();
    http.end();
  }
  queueFile.close();
  if (bodyDoneAt - startedAt >= 2000 || !apiResponseValid) {
    logLine("Punch timing: queue=" + String(snapshotDoneAt - startedAt) +
            "ms post=" + String(postDoneAt - snapshotDoneAt) +
            "ms body=" + String(bodyDoneAt - postDoneAt) +
            "ms read=" + String(bodyReadResult) + " rssi=" + String(WiFi.RSSI()));
  }

  if (apiResponseValid) {
    String clean;
    for (size_t i = 0; i < apiMessage.length() && clean.length() < 256; ++i) {
      char c = apiMessage[i];
      clean += (c == '\r' || c == '\n' || c == '\t') ? ' ' : (uint8_t(c) < 32 || c == 127) ? ' ' : c;
    }
    String line = "Punch API statusCode=" + String(apiStatusCode) + " message=" + clean;
    static String previousLine;
    static uint32_t lastLineAt = 0;
    if (line != previousLine || millis() - lastLineAt >= 300000UL) {
      logLine(line);
      previousLine = line;
      lastLineAt = millis();
    }
  } else {
    logLine(code > 0 ? "Punch API response missing statusCode/message (HTTP " + String(code) + ")"
                     : "Punch API connection failed (" + String(code) + ")");
  }

  if (!uploadComplete || !bodyComplete || !PunchProtocol::acceptedStatus(code) ||
      !apiResponseValid || apiStatusCode != 200) {
    serverOk = false;
    punchFeedback = PunchFeedback::Failed;
    idleRefreshRequested = true;
    return false;
  }

  bool compacted = false;
  {
    MutexGuard queueGuard(queueMutex, pdMS_TO_TICKS(1000));
    if (queueGuard.acquired()) compacted = removeQueuePrefix(cutOffset);
  }
  if (compacted) {
    logLine("synced and committed " + String(n) + " punch(es)");
    serverOk = true;
    punchFeedback = PunchFeedback::Accepted;
  } else {
    logLine("! server accepted batch but local compaction failed - retry may deliver a duplicate");
    serverOk = false;
    punchFeedback = PunchFeedback::Failed;
  }
  idleRefreshRequested = true;
  return compacted;
}

void requestImmediateUpload() {
  if (uploaderTaskHandle) xTaskNotifyGive(uploaderTaskHandle);
}

void uploaderTask(void* parameter) {
  (void)parameter;
  for (;;) {
    // A punch/reconnect/manual request wakes this immediately. A timeout is
    // the offline/server-failure retry path and prevents notification loss
    // from ever stranding durable records.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(FLUSH_RETRY_MS));
    if (!fsReady || WiFi.status() != WL_CONNECTED) continue;

    // Each wake sends every punch present at snapshot time in one array.
    // A punch appended during the POST wakes this task again afterward.
    flushQueue(15000);
  }
}

// ══════════════════════════════════════════════════════════ voice feedback ══
// Scan paths only queue the phrase; this task alone owns I2S, so the ~1.1 s
// playback never stalls the reader, the web page or the uploader. A newer
// decision replaces one that is still waiting instead of building a backlog.
void announce(bool granted) {
  if (!voiceQueue) return;
  uint8_t phrase = granted ? 1 : 0;
  xQueueOverwrite(voiceQueue, &phrase);
}

void voiceTask(void* parameter) {
  (void)parameter;
  bool failureLogged = false;
  for (;;) {
    uint8_t phrase;
    if (xQueueReceive(voiceQueue, &phrase, portMAX_DELAY) != pdTRUE) continue;
    bool played = phrase ? VoiceFeedback::playGranted() : VoiceFeedback::playDenied();
    if (!played && !failureLogged) { logLine("! voice playback failed"); failureLogged = true; }
  }
}

void makeUuid(char* out) {
  uint8_t b[16];
  for (int i = 0; i < 16; i++) b[i] = esp_random() & 0xFF;
  b[6] = (b[6] & 0x0F) | 0x40;
  b[8] = (b[8] & 0x3F) | 0x80;
  sprintf(out,
    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
    b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
}

/* The SFM ring does not auto-revert — setRingColor() fades TO the color you
 * give it and then holds there forever until something else sets it again.
 * So "show green for a bit" has to be implemented as our own timer, checked
 * once per loop() below, rather than trusted to the hardware. */

// Queue ring changes; never interrupt an active fingerprint capture.
void setSolid(uint8_t color) {
  desiredRing = color;
}

// A color that shows for `ms`, then automatically reverts to the idle
// (connectivity) color — used for anything that's a momentary signal:
// punch accepted/rejected and the physical service/reset cues.
void flash(uint8_t color, uint16_t ms) {
  setSolid(color);
  pendingRevertAt = millis() + ms;
}

// Idle color reflects whether the last attempt to reach the server
// succeeded — blue if yes, red if it failed, yellow if not yet tried or
// a punch is saved offline.
void setIdle() {
  setSolid(punchFeedback == PunchFeedback::Accepted ? SFM_RING_BLUE :
           punchFeedback == PunchFeedback::Failed ? SFM_RING_RED :
           punchFeedback == PunchFeedback::Pending ? SFM_RING_CYAN : SFM_RING_YELLOW);
}

// Call after anything that changes serverOk, so the ring updates promptly —
// but only if nothing else is actively showing a punch result right now,
// so we don't cut a green/red flash short.
void refreshIdleIfNotBusy() {
  if (!pendingRevertAt && !enrollActive && !bootButtonTracking) setIdle();
}

void reject(const String& why) {
  logLine("rejected: " + why);
  punchFeedback = PunchFeedback::Failed;
  pendingRevertAt = 0;
  setSolid(SFM_RING_RED);
  announce(false);
}

// ═════════════════════════════════════════════════════════════ device config ══
/* This is what makes the firmware backend-agnostic: server address, device
 * id and auth token live here, not in the compiled binary. Any company can
 * point an unmodified build at their own backend through this page, as long
 * as that backend speaks the documented wire protocol (PROTOCOL_VERSION). */
void loadConfig() {
  DeviceConfig cfg;
  cfg.server      = SERVER_DEFAULT;
  cfg.deviceId    = deviceName;
  cfg.token       = TOKEN_DEFAULT;
  cfg.wifiSsid    = WIFI_SSID_DEFAULT;
  cfg.wifiPass    = WIFI_PASS_DEFAULT;
  cfg.punchesPath = PUNCHES_PATH_DEFAULT;
  cfg.eventType   = -1; // explicit device direction required before accepting scans
  applyConfig(cfg);

  if (!fsReady || !LittleFS.exists(CONFIG_FILE)) return;   // first boot — defaults stand

  File f = LittleFS.open(CONFIG_FILE, FILE_READ);
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) { logLine("! config.json corrupt - using defaults"); return; }

  if (doc["server"].is<const char*>())       cfg.server      = doc["server"].as<String>();
  if (doc["device_id"].is<const char*>())    cfg.deviceId    = doc["device_id"].as<String>();
  if (doc["token"].is<const char*>())        cfg.token       = doc["token"].as<String>();
  if (doc["wifi_ssid"].is<const char*>())    cfg.wifiSsid    = doc["wifi_ssid"].as<String>();
  if (doc["wifi_pass"].is<const char*>())    cfg.wifiPass    = doc["wifi_pass"].as<String>();
  if (doc["punches_path"].is<const char*>()) cfg.punchesPath = doc["punches_path"].as<String>();
  if (doc["event_type"].is<int>()) cfg.eventType = doc["event_type"].as<int>();
  if (cfg.deviceId == "proto-01") cfg.deviceId = deviceName;
  String error;
  if (!validateConfig(cfg, error)) { logLine("! stored config rejected: " + error); return; }
  applyConfig(cfg);
}

bool saveConfig(const DeviceConfig& cfg) {
  JsonDocument doc;
  doc["server"]       = cfg.server;
  doc["device_id"]    = cfg.deviceId;
  doc["token"]        = cfg.token;
  doc["wifi_ssid"]    = cfg.wifiSsid;
  doc["wifi_pass"]    = cfg.wifiPass;
  doc["punches_path"] = cfg.punchesPath;
  doc["event_type"]   = cfg.eventType;
  String data;
  serializeJson(doc, data);
  return atomicWrite(CONFIG_FILE, data);
}

bool requireServiceAccess() {
  addSecurityHeaders();
  // The setup AP has a limited window, but the user can configure the
  // terminal through its assigned LAN address while station WiFi is connected.
  if (!serviceMode && WiFi.status() != WL_CONNECTED) {
    server.send(403, "text/plain", "WiFi offline; hold BOOT for 3 seconds to open setup WiFi");
    return false;
  }
  return true;
}

bool requireMutation() {
  if (!requireServiceAccess()) return false;
  if (server.header("X-CSRF-Token") != csrfToken) {
    addSecurityHeaders(); server.send(403, "text/plain", "invalid CSRF token"); return false;
  }
  return true;
}

void handleConfigGet() {
  if (!requireServiceAccess()) return;
  JsonDocument doc;
  doc["server"]        = cfgServer;
  doc["device_id"]     = cfgDeviceId;
  doc["token_set"]     = cfgToken.length() > 0;      // never echo the token itself back
  doc["wifi_ssid"]     = cfgWifiSsid;
  doc["wifi_pass_set"] = cfgWifiPass.length() > 0;   // never echo the password back
  doc["punches_path"]  = cfgPunchesPath;
  doc["event_type"]    = cfgEventType;
  doc["device_name"]   = deviceName;
  doc["protocol"]      = PROTOCOL_VERSION;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleConfigSet() {
  if (!requireMutation()) return;
  if (fingerprintBusy()) { server.send(409, "text/plain", "Finish enrollment or lift your finger before saving"); return; }
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "missing body"); return; }
  if (server.arg("plain").length() > 1024) { server.send(413, "text/plain", "config body too large"); return; }

  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "bad json"); return;
  }

  DeviceConfig next{cfgServer, cfgDeviceId, cfgToken, cfgWifiSsid,
                    cfgWifiPass, cfgPunchesPath, cfgEventType};
  if (doc["server"].is<const char*>())       next.server = doc["server"].as<String>();
  if (doc["device_id"].is<const char*>())    next.deviceId = doc["device_id"].as<String>();
  if (doc["token"].is<const char*>())        next.token = doc["token"].as<String>();
  if (doc["punches_path"].is<const char*>()) next.punchesPath = doc["punches_path"].as<String>();
  if (doc["wifi_ssid"].is<const char*>())    next.wifiSsid = doc["wifi_ssid"].as<String>();
  if (doc["wifi_pass"].is<const char*>())    next.wifiPass = doc["wifi_pass"].as<String>();
  while (next.server.endsWith("/")) next.server.remove(next.server.length() - 1);
  if (!doc["event_type"].is<int>() || !PunchProtocol::validEventType(doc["event_type"].as<int>())) {
    server.send(400, "text/plain", "select IN (1) or OUT (0)"); return;
  }
  next.eventType = doc["event_type"].as<int>();
  if (!next.server.startsWith("https://")) { server.send(400, "text/plain", "HTTPS server URL required"); return; }
  String validationError;
  if (!validateConfig(next, validationError)) { server.send(400, "text/plain", validationError); return; }

  bool wifiChanged = next.wifiSsid != cfgWifiSsid || next.wifiPass != cfgWifiPass;
  if (!saveConfig(next)) {
    logLine("! config commit failed; live config unchanged");
    server.send(500, "text/plain", "could not durably save configuration"); return;
  }
  applyConfig(next);
  logLine("config updated — server: " + cfgServer + "   device id: " + cfgDeviceId +
           "   auth: " + (cfgToken.length() ? "token set" : "none"));

  // Send the response BEFORE the blocking WiFi/sync calls below — if the
  // browser is talking to us over the setup AP right now, that's fine (the
  // AP never drops), but there's no reason to make it wait on a ~15s WiFi
  // attempt just to find out the config was saved.
  server.send(200, "text/plain", "ok");

  if (wifiChanged && cfgWifiSsid.length()) {
    // Safe to switch networks immediately: the setup AP is a SEPARATE
    // interface in AP_STA mode and stays up throughout, so this can't drop
    // whoever is configuring the device the way a plain STA-only switch
    // would. See the AP_STA comment in setup() for the full reasoning.
    logLine("applying new wifi — joining '" + cfgWifiSsid + "'");
    WiFi.begin(cfgWifiSsid.c_str(), cfgWifiPass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) delay(50);
    if (WiFi.status() == WL_CONNECTED) {
      logLine("wifi ok — http://" + WiFi.localIP().toString());
      configTime(0, 0, "pool.ntp.org", "time.google.com");
    } else {
      logLine("could not join '" + cfgWifiSsid + "' — check the password");
    }
  }

  // Don't make the user hunt for the Flush/Sync buttons after fixing the
  // server address — try the new one immediately, same idea as v5's
  // immediate push after a punch.
  logLine("applying new config — syncing now");
  requestImmediateUpload();

}

void handleWifi() {
  static bool was = false;
  bool now = WiFi.status() == WL_CONNECTED;
  if (now != was) {
    was = now;
    if (now) {
      logLine("wifi connected — " + WiFi.localIP().toString());
      // Give the owner a full window to finish setup after reconnection.
      if (serviceMode) serviceUntil = millis() + SERVICE_WINDOW_MS;
      configTime(0, 0, "pool.ntp.org", "time.google.com");
      requestImmediateUpload();
    } else {
      logLine("wifi lost — punches will queue locally");
      serverOk = false;
      refreshIdleIfNotBusy();
      if (cfgWifiSsid.length()) startServiceMode();
    }
  }
  // Arduino-ESP32 already retries reconnectable station failures. A second
  // manual attempt collides with the active association and logs an error.
}

// Call only while Wire is stopped. pinMode() on an active I2C pin detaches
// the bus in Arduino-ESP32 3.x, even when the line already reads HIGH.
void i2cRecover() {
  pinMode(PIN_SDA, INPUT_PULLUP);
  pinMode(PIN_SCL, INPUT_PULLUP);
  delayMicroseconds(10);
  if (digitalRead(PIN_SDA) == LOW) {
    pinMode(PIN_SCL, OUTPUT_OPEN_DRAIN);
    digitalWrite(PIN_SCL, HIGH);
    for (int i = 0; i < 9 && digitalRead(PIN_SDA) == LOW; i++) {
      digitalWrite(PIN_SCL, LOW);  delayMicroseconds(5);
      digitalWrite(PIN_SCL, HIGH); delayMicroseconds(5);
    }
    pinMode(PIN_SCL, INPUT_PULLUP);
  }
  bool sdaLow = digitalRead(PIN_SDA) == LOW;
  bool sclLow = digitalRead(PIN_SCL) == LOW;
  static int lastFault = 0;
  int fault = (sdaLow ? 1 : 0) | (sclLow ? 2 : 0);
  if (fault != lastFault) {
    if (fault) logLine(String("! I2C stuck: SDA=") + (sdaLow ? "LOW" : "HIGH") +
                      " SCL=" + (sclLow ? "LOW" : "HIGH") + "; RFID/RTC unavailable");
    else logLine("I2C lines released");
    lastFault = fault;
  }
}

// ══════════════════════════════════════════════════════════════ http handlers ══
void handleState() {
  if (!requireServiceAccess()) return;
  JsonDocument d;
  d["nfc"] = haveNfc; d["fp"] = haveFp;
  d["wifi"] = WiFi.status() == WL_CONNECTED; d["serverOk"] = serverOk;
  d["punchFeedback"] = static_cast<uint8_t>(punchFeedback);
  d["ip"] = WiFi.localIP().toString(); d["fsReady"] = fsReady;
  d["queue"] = queueCount(); d["legacyQueue"] = legacyQueueCount();
  d["lastPunch"] = lastPunchLine;
  d["deviceName"] = deviceName; d["deviceId"] = cfgDeviceId;
  d["clock"] = utcNow(); d["eventType"] = cfgEventType;
  d["rtc"] = haveRtc;
  d["fingerTouch"] = digitalRead(PIN_FP_TOUCH) == HIGH;
  d["fingerCount"] = fingerCount; d["fingerId"] = lastFingerId;
  d["cardId"] = lastCardRef;
  d["enrolledId"] = enrolledFingerId; d["enrolling"] = enrollActive;
  d["fingerMessage"] = fingerMessage;
  d["canEnroll"] = haveFp && fingerModeReady && fingerCount >= 0 && fingerCount < SFM_CAPACITY &&
                     !enrollActive && (!fingerLink.busy() || fingerLink.command() == 0xc3);
  d["server"] = cfgServer; d["punchesPath"] = cfgPunchesPath;
  d["tokenSet"] = cfgToken.length() > 0;
  d["wifiSsid"] = cfgWifiSsid; d["wifiPassSet"] = cfgWifiPass.length() > 0;
  d["apSsid"] = AP_SSID; d["apIp"] = apRunning ? WiFi.softAPIP().toString() : "disabled";
  d["csrf"] = csrfToken;
  {
    MutexGuard guard(logMutex, pdMS_TO_TICKS(100));
    if (guard.acquired()) {
      d["seq"] = logSeq;
      JsonArray logs = d["log"].to<JsonArray>();
      int n = logHead < LOG_LINES ? logHead : LOG_LINES;
      for (int i = 0; i < n; ++i) logs.add(logBuf[(logHead - n + i) % LOG_LINES]);
    }
  }
  String out; serializeJson(d, out); server.send(200, "application/json", out);
}

void handleFlush() {
  if (!requireMutation()) return;
  logLine("manual real-time upload requested");
  requestImmediateUpload();
  server.send(202, "text/plain", "upload requested; inspect queue/status");
}

void handleClear() {
  if (!requireMutation()) return;
  logHead = 0; logSeq++;
  logLine("log cleared");
  server.send(200, "text/plain", "ok");
}

// ══════════════════════════════════════════════════════════════════ the page ══
#include "page.h"

void handleRoot() {
  if (!requireServiceAccess()) return;
  server.sendHeader("Content-Security-Policy",
    "default-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; frame-ancestors 'none'");
  server.send_P(200, "text/html", PAGE);
}
