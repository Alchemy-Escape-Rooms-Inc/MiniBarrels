//================================================
//  A Mermaid's Tale - Mini Barrels sensor status
//  Target board: ESP32-S3 (UART0 + UART1 + UART2 + 2x SoftwareSerial)
//
//  Five barrels, five RFID readers. One MQTT topic per reader,
//  RETAINED. Payload is always one of three words:
//
//      "True"   - the correct barrel is on this reader
//      "False"  - a wrong barrel is on this reader
//      "Clear"  - the reader sees nothing
//
//      MermaidsTale/MiniBarrels/Vanilla
//      MermaidsTale/MiniBarrels/Cloves
//      MermaidsTale/MiniBarrels/Molasses
//      MermaidsTale/MiniBarrels/SugarCane
//      MermaidsTale/MiniBarrels/Yeast
//
//  EDGE-TRIGGERED publishes:
//    A publish fires only when a reader's state actually changes
//    (Clear -> True, True -> Clear, False -> True, etc.). A tag
//    sitting still emits no traffic. The same wrong tag lifted and
//    replaced within REMOVAL_TIMEOUT_MS emits no traffic. The
//    retained MQTT value is always the current state.
//================================================

#include <WiFi.h>
#include <PubSubClient.h>
#include <HardwareSerial.h>
#include <SoftwareSerial.h>
#include <stdarg.h>
#include "MANIFEST.h"   // single source of truth: tag IDs, version, topics, config

// Identity/version/topics/tags all come from MANIFEST.h (single source of
// truth). These bridge the manifest names onto the names the code already uses.
#define VERSION                FIRMWARE_VERSION
#define PROP_NAME              DEVICE_NAME
#define NUM_SPICES             5
#define ID_LEN                 12
#define TOPIC_BUF              48
#define DEBUG_RFID             0      // 1 = log every raw byte to USB serial

// WatchTower heartbeat standard = 5 minutes (300000ms). The deployed v1.0.0
// firmware used ~5s here, which spammed the broker and looked like a reboot
// loop on the wire (online + status + all-Clear re-announced every 5s).
const unsigned long HEARTBEAT_MS = HEARTBEAT_MS_MANIFEST;

// "Clear" fires only after this long with NO bytes at all from the
// reader (not just "no good frames"). With three SoftwareSerial
// streams sharing the CPU it is normal for the occasional frame to
// be corrupted; bytes still arrive and the tag is still present.
// 2026-07-27 (v2.7.2): 2000 -> 20000. The live board proved these readers
// do NOT stream while a tag sits in the field — they re-report a seated
// tag only every ~4-10s (wire log 07-26 18:50-18:59: every True was
// followed by Clear at exactly +2.000s, then True again on the next
// re-poll; all five barrels flapped and the all-True solve window never
// existed). 20s = 2x margin over the worst observed re-poll gap. Cost:
// a lifted barrel reads empty after up to 20s — fine for this puzzle,
// since a swapped-on tag still classifies True/False instantly.
const unsigned long REMOVAL_TIMEOUT_MS = 20000UL;

// Frame markers from the serial RFID modules
static const byte STX = 0x02;
static const byte ETX = 0x03;

// RFID reader RX pins
#define S1_RX   4
#define S2_RX   5
#define S3_RX   6
#define S4_RX   7
#define S5_RX  15

// WiFi + MQTT
static const char* WIFI_SSID   = "AlchemyGuest";
static const char* WIFI_PASS   = "VoodooVacation5601";
static const char* MQTT_SERVER = "10.1.10.115";
static const int   MQTT_PORT   = 1883;
static const char* TOPIC_BASE  = "MermaidsTale/MiniBarrels/";

// WatchTower standard topic suffixes
static const char* MQTT_TOPIC_STATUS  = "MermaidsTale/MiniBarrels/status";
static const char* MQTT_TOPIC_LOG     = "MermaidsTale/MiniBarrels/log";
static const char* MQTT_TOPIC_COMMAND = "MermaidsTale/MiniBarrels/command";

//================================================
//            Per-spice state
//================================================
enum SpiceState { ST_CLEAR = 0, ST_FALSE = 1, ST_TRUE = 2 };
static const char* const STATE_NAMES[] = { "Clear", "False", "True" };

struct Spice {
  const char* name;
  const char* expected;           // expected UID for "True" (12 chars, from MANIFEST.h)
  Stream*     port;
  char        topic[TOPIC_BUF];   // precomputed full MQTT topic

  // runtime
  char          rx[ID_LEN];       // accumulating frame buffer
  byte          rxLen;
  unsigned long lastByteMs;       // ANY byte from this reader resets this
  bool          hasTag;           // tracking a UID right now
  char          lastUid[ID_LEN];
  SpiceState    state;            // last PUBLISHED state
  bool          published;        // we have published at least once
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
// memcmp's ID_LEN bytes) would read past the string. A wrong length fails
// the build here with a clear message instead of silently deadening a barrel.
static_assert(sizeof(TAG_VANILLA)   - 1 == ID_LEN, "TAG_VANILLA must be 12 chars");
static_assert(sizeof(TAG_CLOVES)    - 1 == ID_LEN, "TAG_CLOVES must be 12 chars");
static_assert(sizeof(TAG_MOLASSES)  - 1 == ID_LEN, "TAG_MOLASSES must be 12 chars");
static_assert(sizeof(TAG_SUGARCANE) - 1 == ID_LEN, "TAG_SUGARCANE must be 12 chars");
static_assert(sizeof(TAG_YEAST)     - 1 == ID_LEN, "TAG_YEAST must be 12 chars");

WiFiClient   espClient;
PubSubClient mqtt(espClient);

// Puzzle solve state. The barrel puzzle is SOLVED when all 5 readers
// simultaneously report their correct (expected) tag = ST_TRUE. M3 event 26
// "Mini Barrels Solved" is gated on status=SOLVED (fires the piston, pirate
// bell, objectives, GoldSolved). Edge-triggered so it publishes once per edge.
bool          puzzleSolved   = false;
unsigned long lastHeartbeat  = 0;

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
void connectWiFi() {
  Serial.printf("WiFi: %s ", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print('.');
    delay(200);
  }
  Serial.println(" up");
}

void connectMQTT() {
  while (!mqtt.connected()) {
    String clientId = String("MiniBarrels_") + String(random(0xffff), HEX);
    if (mqtt.connect(clientId.c_str())) {
      Serial.printf("MQTT connected to %s\n", MQTT_SERVER);
      // WatchTower boot sequence: subscribe to /command, announce ONLINE.
      mqtt.subscribe(MQTT_TOPIC_COMMAND);
      mqtt.publish(MQTT_TOPIC_STATUS, "ONLINE");
      mqttLogf("%s v%s online", PROP_NAME, VERSION);
    } else {
      Serial.printf("MQTT failed rc=%d, retry 2s\n", mqtt.state());
      delay(2000);
    }
  }
}

// ---- WatchTower command handling ----------------------------------------
// Report the number of correct barrels as a quick diagnostic state string.
// Protocol standard: the reply goes back on /command (same as PONG); the
// echo lands in the unknown-command branch, which is harmless.
void promptStatus() {
  byte correct = 0;
  for (byte i = 0; i < NUM_SPICES; i++)
    if (spices[i].state == ST_TRUE) correct++;
  char reply[64];
  snprintf(reply, sizeof(reply), "%s|%u/%u|UP:%lus|V%s",
           puzzleSolved ? "SOLVED" : "PLAYING",
           correct, (unsigned)NUM_SPICES, millis() / 1000UL, VERSION);
  mqtt.publish(MQTT_TOPIC_COMMAND, reply);
  mqttLogf("STATUS -> %s", reply);
}

// Re-publish every barrel's current retained value and the solve state.
// Used by PUZZLE_RESET so M3 re-syncs without the board rebooting.
void republishAll() {
  for (byte i = 0; i < NUM_SPICES; i++)
    mqtt.publish(spices[i].topic, STATE_NAMES[spices[i].state], true);
  mqtt.publish(MQTT_TOPIC_STATUS, puzzleSolved ? "SOLVED" : "ONLINE", true);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char message[64];
  if (length >= sizeof(message)) length = sizeof(message) - 1;
  memcpy(message, payload, length);
  message[length] = '\0';

  // trim surrounding whitespace
  char* msg = message;
  while (*msg == ' ' || *msg == '\r' || *msg == '\n') msg++;
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
    // Re-sync M3 without rebooting: republish every barrel's current state
    // and let checkSolved() recompute from live sensors. Clearing lastUid
    // makes the next frame re-classify the tag; hasTag stays set so removal
    // detection still works. (Clearing `published` here would force retained
    // Clear and deaden any barrel already sitting on its reader.)
    for (byte i = 0; i < NUM_SPICES; i++) memset(spices[i].lastUid, 0, ID_LEN);
    puzzleSolved = false;
    republishAll();
    mqtt.publish(MQTT_TOPIC_COMMAND, "OK");
    Serial.println("[MQTT] PUZZLE_RESET -> re-synced");
    return;
  }
  Serial.printf("[MQTT] unknown command: %s\n", msg);
}

// Publish status=SOLVED once when all 5 barrels are correct; revert to
// ONLINE (edge-triggered) if the puzzle is broken back apart.
void checkSolved() {
  bool allTrue = true;
  for (byte i = 0; i < NUM_SPICES; i++)
    if (spices[i].state != ST_TRUE) { allTrue = false; break; }

  if (allTrue && !puzzleSolved) {
    puzzleSolved = true;
    mqtt.publish(MQTT_TOPIC_STATUS, "SOLVED", true);
    mqttLogf("%s SOLVED - all 5 barrels correct", PROP_NAME);
  } else if (!allTrue && puzzleSolved) {
    puzzleSolved = false;
    mqtt.publish(MQTT_TOPIC_STATUS, "ONLINE", true);
    mqttLogf("%s unsolved - barrel removed", PROP_NAME);
  }
}

// WatchTower 5-minute heartbeat. Publishes ONLINE (or SOLVED if solved) on
// /status. NOT a reboot — this replaces the ~5s spam of the old firmware.
void heartBeat() {
  unsigned long now = millis();
  if (now - lastHeartbeat < HEARTBEAT_MS) return;
  lastHeartbeat = now;
  mqtt.publish(MQTT_TOPIC_STATUS, puzzleSolved ? "SOLVED" : "ONLINE", true);
  mqttLogf("%s v%s online", PROP_NAME, VERSION);
}

//================================================
//            Sensor state machine
//================================================

// Edge-triggered: publishes only if newState is different from the
// last state we published for this spice (or this is the first publish).
void setState(Spice& s, SpiceState newState) {
  if (s.published && s.state == newState) return;
  s.state     = newState;
  s.published = true;
  mqtt.publish(s.topic, STATE_NAMES[newState], true);   // retained
  Serial.printf("[%s] %s\n", s.name, STATE_NAMES[newState]);
}

// Drain one reader. On every complete frame, classify the UID as
// True or False and (only on transition) publish. On REMOVAL_TIMEOUT_MS
// of silence, publish Clear. On first run, publish Clear once so the
// retained value exists on every topic.
void scan(Spice& s) {
  while (s.port->available()) {
    int b = s.port->read();
    s.lastByteMs = millis();          // ANY byte = reader is alive = tag present
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
          setState(s, ok ? ST_TRUE : ST_FALSE);
        }
      }
      s.rxLen = 0;
      continue;
    }
    if (b != '\r' && b != '\n' && s.rxLen < ID_LEN) {
      s.rx[s.rxLen++] = (char)b;
    }
  }

  // Reader has gone TOTALLY silent (no bytes at all) -> barrel was lifted.
  // Garbled frames during heavy WiFi/SoftwareSerial contention no longer
  // count as "removal" - any byte resets lastByteMs above.
  if (s.hasTag && s.lastByteMs != 0 &&
      (millis() - s.lastByteMs) > REMOVAL_TIMEOUT_MS) {
    s.hasTag = false;
    memset(s.lastUid, 0, ID_LEN);
    setState(s, ST_CLEAR);
  }

  // First time through -> publish Clear once so the retained value exists
  if (!s.published) {
    setState(s, ST_CLEAR);
  }
}

//================================================
//            Setup / Loop
//================================================
void setupRFID() {
  rfid1.begin(9600, SERIAL_8N1,   S1_RX, -1);
  rfid2.begin(9600, SERIAL_8N1,   S2_RX, -1);
  rfid3.begin(9600, SERIAL_8N1,   S3_RX, -1);
  rfid4.begin(9600, SWSERIAL_8N1, S4_RX, -1);
  rfid5.begin(9600, SWSERIAL_8N1, S5_RX, -1);

  for (byte i = 0; i < NUM_SPICES; i++) {
    snprintf(spices[i].topic, TOPIC_BUF, "%s%s", TOPIC_BASE, spices[i].name);
    spices[i].rxLen        = 0;
    spices[i].lastByteMs   = 0;
    spices[i].hasTag       = false;
    spices[i].state        = ST_CLEAR;
    spices[i].published    = false;
    memset(spices[i].lastUid, 0, ID_LEN);
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\nMiniBarrels v%s\n", VERSION);
  connectWiFi();
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(256);
  setupRFID();
}

void loop() {
  if (!mqtt.connected()) connectMQTT();
  mqtt.loop();
  for (byte i = 0; i < NUM_SPICES; i++) scan(spices[i]);
  checkSolved();
  heartBeat();
}
