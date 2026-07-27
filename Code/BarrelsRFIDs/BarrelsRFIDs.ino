//================================================
//  A Mermaid's Tale - Mini Barrels (v3.0.0)
//  Target board: ESP32-S3 (UART0 + UART1 + UART2 + 2x SoftwareSerial)
//  BUILD REQUIREMENT: "USB CDC On Boot = Enabled" (CDCOnBoot=cdc) or
//  Serial steals UART0 and kills the Vanilla reader.
//
//  Five barrels, five serial RFID readers, TWO publish layers:
//
//  1) PULSE layer - MermaidsTale/MiniBarrels/<Spice>
//     A 2s pulse per placement: "True" (correct barrel) or "False"
//     (wrong barrel) the moment a NEW tag lands, then "Clear" 2s
//     later. This drives M3's per-barrel sound effects: M3 plays the
//     SFX on True while its own <Spice>Latch topic reads "ready",
//     and needs the Clear to re-arm that latch for the next barrel.
//     (The <Spice>Latch topics belong to M3 - never published here.)
//
//  2) SEATED layer - MermaidsTale/MiniBarrels/system/<Spice> (retained)
//     The board's real memory of what is on each reader. Drives
//     checkSolved(): all five True -> status=SOLVED (M3 fires the
//     barrel-piston finale). Cleared only by a different tag landing
//     on the reader or by PUZZLE_RESET - NEVER by reader silence,
//     because these readers re-report a seated tag erratically
//     (4s to ~2min apart; v2.7.x tried silence timeouts and flapped).
//
//  Hardening (v3.0.0, mirrors SunDial Bridge 4.3.0):
//    - MQTT LWT: broker publishes retained OFFLINE to /status if the
//      connection dies, so WatchTower sees a silent death.
//    - 30s task watchdog reboots the chip if loop() ever stalls.
//    - 2min offline self-reboot if the broker stays unreachable.
//    - Non-blocking MQTT retry: readers keep scanning during outages;
//      republishAll() re-syncs the retained layer on every reconnect.
//================================================

#include <WiFi.h>
#include <PubSubClient.h>
#include <HardwareSerial.h>
#include <SoftwareSerial.h>
#include <esp_task_wdt.h>
#include <stdarg.h>
#include "MANIFEST.h"   // single source of truth: tag IDs, version, broker, config

// Identity/version come from MANIFEST.h; these bridge the manifest names
// onto the names the code uses.
#define VERSION                FIRMWARE_VERSION
#define PROP_NAME              DEVICE_NAME
#define NUM_SPICES             5
#define ID_LEN                 12
#define TOPIC_BUF              48
#define DEBUG_RFID             0      // 1 = log every raw byte to USB serial

// WatchTower heartbeat standard = 5 minutes (from MANIFEST.h).
const unsigned long HEARTBEAT_MS = HEARTBEAT_MS_MANIFEST;

// Wire pulse width for the SFX layer (see header). 2s matches the timing
// M3's latch machine was tuned around in the original room.
const unsigned long PULSE_MS = 2000UL;

// Hang recovery (mirrors SunDial Bridge 4.3.0)
const uint32_t      WDT_TIMEOUT_S     = 30;      // loop() stall -> panic reboot
const unsigned long WIFI_WAIT_MS      = 10000;   // per ensureWiFi() attempt
const unsigned long MQTT_RETRY_MS     = 2000;    // min gap between connect attempts
const unsigned long OFFLINE_REBOOT_MS = 120000;  // no broker for 2min -> restart

// Frame markers + baud for the serial RFID modules
static const byte STX = 0x02;
static const byte ETX = 0x03;
static const unsigned long RFID_BAUD = 9600;

// RFID reader RX pins (must match MANIFEST PIN_CONFIG)
#define S1_RX   4
#define S2_RX   5
#define S3_RX   6
#define S4_RX   7
#define S5_RX  15

// WiFi + MQTT. Broker address/port come from MANIFEST.h.
static const char* WIFI_SSID   = "AlchemyGuest";
static const char* WIFI_PASS   = "VoodooVacation5601";
static const char* MQTT_SERVER = BROKER_IP;
static const int   MQTT_PORT   = BROKER_PORT;

// All topics share one root (also used to build the per-spice topics).
#define TOPIC_ROOT "MermaidsTale/MiniBarrels/"
static const char* TOPIC_BASE         = TOPIC_ROOT;
static const char* MQTT_TOPIC_STATUS  = TOPIC_ROOT "status";
static const char* MQTT_TOPIC_LOG     = TOPIC_ROOT "log";
static const char* MQTT_TOPIC_COMMAND = TOPIC_ROOT "command";

//================================================
//            Per-spice state
//================================================
enum SpiceState { ST_CLEAR = 0, ST_FALSE = 1, ST_TRUE = 2 };
static const char* const STATE_NAMES[] = { "Clear", "False", "True" };

struct Spice {
  const char* name;
  const char* expected;           // expected UID for "True" (12 chars, from MANIFEST.h)
  Stream*     port;
  char        topic[TOPIC_BUF];   // pulse topic    (MermaidsTale/MiniBarrels/<Spice>)
  char        sysTopic[TOPIC_BUF];// seated topic   (.../system/<Spice>, retained)

  // runtime
  char          rx[ID_LEN];       // accumulating frame buffer
  byte          rxLen;
  bool          hasTag;           // tracking a UID right now
  char          lastUid[ID_LEN];
  SpiceState    seated;           // private memory of what is on this reader
                                  // (drives the solve; survives the wire pulse).
                                  // NO relation to M3's <Spice>Latch topics.
  unsigned long pulseClearAtMs;   // when to send the pulse's trailing Clear (0 = none)
};

// Vanilla on UART0 requires "USB CDC On Boot = Enabled" so Serial
// does not steal UART0. Molasses uses UART2 to match the wired reader
// layout; only SugarCane and Yeast use software serial.
HardwareSerial rfid1(0);   // Vanilla
HardwareSerial rfid2(1);   // Cloves
HardwareSerial rfid3(2);   // Molasses
EspSoftwareSerial::UART rfid4, rfid5;

// Expected tag UIDs come straight from MANIFEST.h. To swap a barrel's tag,
// edit the matching TAG_* line in MANIFEST.h and re-flash - nothing here changes.
Spice spices[NUM_SPICES] = {
  { "Vanilla",   TAG_VANILLA,   &rfid1 },
  { "Cloves",    TAG_CLOVES,    &rfid2 },
  { "Molasses",  TAG_MOLASSES,  &rfid3 },
  { "SugarCane", TAG_SUGARCANE, &rfid4 },
  { "Yeast",     TAG_YEAST,     &rfid5 },
};

// Catch a mistyped tag in MANIFEST.h at BUILD time, not in the room. Every
// TAG_* must be exactly ID_LEN (12) hex chars, or the reader compare (which
// memcmp's ID_LEN bytes) would read past the string.
static_assert(sizeof(TAG_VANILLA)   - 1 == ID_LEN, "TAG_VANILLA must be 12 chars");
static_assert(sizeof(TAG_CLOVES)    - 1 == ID_LEN, "TAG_CLOVES must be 12 chars");
static_assert(sizeof(TAG_MOLASSES)  - 1 == ID_LEN, "TAG_MOLASSES must be 12 chars");
static_assert(sizeof(TAG_SUGARCANE) - 1 == ID_LEN, "TAG_SUGARCANE must be 12 chars");
static_assert(sizeof(TAG_YEAST)     - 1 == ID_LEN, "TAG_YEAST must be 12 chars");

WiFiClient   espClient;
PubSubClient mqtt(espClient);

// Puzzle solve state. SOLVED when all 5 readers' seated states are ST_TRUE.
// M3 event 26 "Mini Barrels Solved" is gated on status=SOLVED (fires the
// piston, objectives, GoldSolved). Edge-triggered: publishes once per edge.
bool          puzzleSolved      = false;
unsigned long lastHeartbeat     = 0;
unsigned long lastMqttOkMs      = 0;   // last time the broker connection was up
unsigned long lastMqttAttemptMs = 0;   // last connect attempt (retry backoff)

void mqttLogf(const char* format, ...) {
  char buffer[128];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  mqtt.publish(MQTT_TOPIC_LOG, buffer);
  Serial.println(buffer);
}

//================================================
//            WiFi + MQTT
//================================================
void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_WAIT_MS) {
    esp_task_wdt_reset();
    delay(200);
  }
}

// Re-publish the retained layer (seated states + solve status) so the broker
// and every watcher re-sync after a reconnect or broker restart. The pulse
// topics get their resting Clear too - but never while a pulse is in flight
// (that would cut a sound trigger short).
void republishAll() {
  for (byte i = 0; i < NUM_SPICES; i++) {
    mqtt.publish(spices[i].sysTopic, STATE_NAMES[spices[i].seated], true);
    if (spices[i].pulseClearAtMs == 0) {
      mqtt.publish(spices[i].topic, STATE_NAMES[ST_CLEAR], true);
    }
  }
  mqtt.publish(MQTT_TOPIC_STATUS, puzzleSolved ? "SOLVED" : "ONLINE", true);
}

void ensureMqtt() {
  if (mqtt.connected()) return;
  if (millis() - lastMqttAttemptMs < MQTT_RETRY_MS) return;
  lastMqttAttemptMs = millis();
  String clientId = String(PROP_NAME) + "-" + String(random(0xffff), HEX);
  // LWT: broker publishes retained OFFLINE to /status if this connection
  // dies (keepalive timeout ~22s after a silent hang).
  if (mqtt.connect(clientId.c_str(), MQTT_TOPIC_STATUS, 0, true, "OFFLINE")) {
    mqtt.subscribe(MQTT_TOPIC_COMMAND);
    republishAll();   // retained ONLINE overwrites stale OFFLINE + full re-sync
    mqttLogf("%s v%s online", PROP_NAME, VERSION);
  } else {
    Serial.printf("MQTT failed rc=%d\n", mqtt.state());
  }
}

// ---- WatchTower command handling ----------------------------------------
// Report the number of correct barrels as a quick diagnostic state string.
// Protocol standard: the reply goes back on /command (same as PONG); the
// echo lands in the unknown-command branch, which only prints to serial.
void promptStatus() {
  byte correct = 0;
  for (byte i = 0; i < NUM_SPICES; i++)
    if (spices[i].seated == ST_TRUE) correct++;
  char reply[64];
  snprintf(reply, sizeof(reply), "%s|%u/%u|UP:%lus|V%s",
           puzzleSolved ? "SOLVED" : "PLAYING",
           correct, (unsigned)NUM_SPICES, millis() / 1000UL, VERSION);
  mqtt.publish(MQTT_TOPIC_COMMAND, reply);
  mqttLogf("STATUS -> %s", reply);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char message[64];
  if (length >= sizeof(message)) length = sizeof(message) - 1;
  memcpy(message, payload, length);
  message[length] = '\0';

  // trim surrounding whitespace
  char* msg = message;
  while (*msg == ' ' || *msg == '\r' || *msg == '\n') msg++;
  if (*msg == '\0') return;   // retained-erase publishes "" - not a command
  char* end = msg + strlen(msg) - 1;
  while (end > msg && (*end == ' ' || *end == '\r' || *end == '\n')) *end-- = '\0';

  if (strcmp(topic, MQTT_TOPIC_COMMAND) != 0) return;
  Serial.printf("[MQTT] command: %s\n", msg);

  if (strcmp(msg, "PING") == 0) {
    mqtt.publish(MQTT_TOPIC_COMMAND, "PONG");
    return;
  }
  if (strcmp(msg, "STATUS") == 0) {
    promptStatus();
    return;
  }
  if (strcmp(msg, "RESET") == 0) {
    mqtt.publish(MQTT_TOPIC_COMMAND, "OK");
    Serial.println("[MQTT] RESET -> rebooting");
    delay(100);
    ESP.restart();
    return;
  }
  if (strcmp(msg, "PUZZLE_RESET") == 0) {
    // The reset is the only path that empties the seated memory. Wipe both
    // layers to Clear and cancel any in-flight pulses. A barrel left seated
    // re-announces on its next reader re-poll (4s-2min) and simply counts
    // again - fine between games, when staff strike the barrels anyway.
    for (byte i = 0; i < NUM_SPICES; i++) {
      spices[i].hasTag         = false;
      spices[i].pulseClearAtMs = 0;
      memset(spices[i].lastUid, 0, ID_LEN);
      setSeated(spices[i], ST_CLEAR);
      mqtt.publish(spices[i].topic, STATE_NAMES[ST_CLEAR], true);
    }
    puzzleSolved = false;
    mqtt.publish(MQTT_TOPIC_STATUS, "ONLINE", true);
    mqtt.publish(MQTT_TOPIC_COMMAND, "OK");
    Serial.println("[MQTT] PUZZLE_RESET -> both layers cleared");
    return;
  }
  Serial.printf("[MQTT] unknown command: %s\n", msg);
}

// Publish status=SOLVED once when all 5 seated states are True; revert to
// ONLINE (edge-triggered) if a correct barrel is displaced by a wrong tag.
void checkSolved() {
  bool allTrue = true;
  for (byte i = 0; i < NUM_SPICES; i++)
    if (spices[i].seated != ST_TRUE) { allTrue = false; break; }

  if (allTrue && !puzzleSolved) {
    puzzleSolved = true;
    mqtt.publish(MQTT_TOPIC_STATUS, "SOLVED", true);
    mqttLogf("%s SOLVED - all 5 barrels correct", PROP_NAME);
  } else if (!allTrue && puzzleSolved) {
    puzzleSolved = false;
    mqtt.publish(MQTT_TOPIC_STATUS, "ONLINE", true);
    mqttLogf("%s unsolved - a barrel changed", PROP_NAME);
  }
}

// WatchTower 5-minute heartbeat in the fleet-standard format. Non-retained:
// the retained /status resting value stays ONLINE/SOLVED (that is what M3's
// solve condition and the LWT overwrite logic key on).
void heartBeat() {
  unsigned long now = millis();
  if (now - lastHeartbeat < HEARTBEAT_MS) return;
  lastHeartbeat = now;
  char buf[64];
  snprintf(buf, sizeof(buf), "HEARTBEAT:%s:UP%lus:RSSI%d",
           puzzleSolved ? "SOLVED" : "RUNNING",
           millis() / 1000UL, (int)WiFi.RSSI());
  mqtt.publish(MQTT_TOPIC_STATUS, buf);
}

//================================================
//            Sensor state machine
//================================================

// Update the seated memory for one reader and mirror it to the retained
// .../system/<Spice> topic (the "real list" - what is actually on the
// readers right now, for WatchTower/diagnostics and anyone watching MQTT).
void setSeated(Spice& s, SpiceState newState) {
  if (s.seated == newState) return;
  s.seated = newState;
  mqtt.publish(s.sysTopic, STATE_NAMES[newState], true);   // retained truth
  Serial.printf("[%s] seated=%s\n", s.name, STATE_NAMES[newState]);
}

// Fire a wire pulse on the public spice topic: True/False now, Clear
// scheduled PULSE_MS later (sent by servicePulses). Purely for M3's
// per-placement sound effects.
void firePulse(Spice& s, SpiceState st) {
  mqtt.publish(s.topic, STATE_NAMES[st], false);   // not retained - a pulse, not a state
  s.pulseClearAtMs = millis() + PULSE_MS;
  if (s.pulseClearAtMs == 0) s.pulseClearAtMs = 1;  // 0 means "no pulse pending"
  Serial.printf("[%s] pulse %s\n", s.name, STATE_NAMES[st]);
}

// Send the trailing Clear of any elapsed pulse (retained - Clear is the
// resting value on the public topic, so M3's latch re-arms and a replayed
// retained True can never re-fire a sound after a broker/M3 restart).
void servicePulses() {
  unsigned long now = millis();
  for (byte i = 0; i < NUM_SPICES; i++) {
    Spice& s = spices[i];
    if (s.pulseClearAtMs != 0 && (long)(now - s.pulseClearAtMs) >= 0) {
      s.pulseClearAtMs = 0;
      mqtt.publish(s.topic, STATE_NAMES[ST_CLEAR], true);
      Serial.printf("[%s] pulse Clear\n", s.name);
    }
  }
}

// Drain one reader. On every complete STX...ETX frame, classify the UID and
// - if it is a NEW tag for this reader - update the seated memory and fire
// the SFX pulse. Reader re-reports of the same seated tag are ignored.
void scan(Spice& s) {
  while (s.port->available()) {
    int b = s.port->read();
#if DEBUG_RFID
    Serial.printf("[%s] 0x%02X @%lu\n", s.name, b, millis());
#endif
    if (b == STX) { s.rxLen = 0; continue; }
    if (b == ETX) {
      if (s.rxLen == ID_LEN) {
        bool sameTag = s.hasTag && memcmp(s.rx, s.lastUid, ID_LEN) == 0;
        if (!sameTag) {
          memcpy(s.lastUid, s.rx, ID_LEN);
          s.hasTag = true;
          bool ok = memcmp(s.rx, s.expected, ID_LEN) == 0;
          setSeated(s, ok ? ST_TRUE : ST_FALSE);   // seated memory -> solve
          firePulse(s, ok ? ST_TRUE : ST_FALSE);   // wire pulse -> M3 sound
        }
      }
      s.rxLen = 0;
      continue;
    }
    if (b != '\r' && b != '\n' && s.rxLen < ID_LEN) {
      s.rx[s.rxLen++] = (char)b;
    }
  }
}

//================================================
//            Setup / Loop
//================================================
void setupRFID() {
  rfid1.begin(RFID_BAUD, SERIAL_8N1,   S1_RX, -1);
  rfid2.begin(RFID_BAUD, SERIAL_8N1,   S2_RX, -1);
  rfid3.begin(RFID_BAUD, SERIAL_8N1,   S3_RX, -1);
  rfid4.begin(RFID_BAUD, SWSERIAL_8N1, S4_RX, -1);
  rfid5.begin(RFID_BAUD, SWSERIAL_8N1, S5_RX, -1);

  for (byte i = 0; i < NUM_SPICES; i++) {
    snprintf(spices[i].topic,    TOPIC_BUF, "%s%s",        TOPIC_BASE, spices[i].name);
    snprintf(spices[i].sysTopic, TOPIC_BUF, "%ssystem/%s", TOPIC_BASE, spices[i].name);
    spices[i].rxLen          = 0;
    spices[i].hasTag         = false;
    spices[i].seated         = ST_CLEAR;
    spices[i].pulseClearAtMs = 0;
    memset(spices[i].lastUid, 0, ID_LEN);
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n%s v%s\n", PROP_NAME, VERSION);

  // Task watchdog on the loop task: if loop() ever stalls, the chip
  // panics and reboots itself. (Same pattern as SunDial Bridge 4.3.0.)
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t wdtCfg = {};
  wdtCfg.timeout_ms = WDT_TIMEOUT_S * 1000;
  wdtCfg.idle_core_mask = 0;
  wdtCfg.trigger_panic = true;
  esp_task_wdt_reconfigure(&wdtCfg);  // core 3.x inits the WDT itself
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(NULL);

  ensureWiFi();
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(256);
  setupRFID();
  ensureMqtt();
  lastMqttOkMs = millis();
}

void loop() {
  esp_task_wdt_reset();
  ensureWiFi();
  ensureMqtt();
  mqtt.loop();

  // Offline self-reboot: loop() can be alive while the WiFi/MQTT stack is
  // wedged (the watchdog can't see that). If the broker has been
  // unreachable for OFFLINE_REBOOT_MS, restart and start clean.
  if (mqtt.connected()) {
    lastMqttOkMs = millis();
  } else if (millis() - lastMqttOkMs >= OFFLINE_REBOOT_MS) {
    Serial.println("[WDT] no broker for 2min - restarting");
    ESP.restart();
  }

  for (byte i = 0; i < NUM_SPICES; i++) scan(spices[i]);
  servicePulses();
  checkSolved();
  heartBeat();
}
