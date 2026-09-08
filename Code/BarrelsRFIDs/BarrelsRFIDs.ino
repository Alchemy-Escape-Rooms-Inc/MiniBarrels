//================================================
//  A Mermaid's Tale - Mini Barrels (v3.1.0)
//  Target board: ESP32-S3 (UART0 + UART1 + UART2 + 2x SoftwareSerial)
//  BUILD REQUIREMENT: "USB CDC On Boot = Enabled" (CDCOnBoot=cdc) or
//  Serial steals UART0 and kills the Vanilla reader.
//
//  Five barrels, five serial RFID readers, one WS2811 light string.
//
//  THE TWIST (v3.1.0): a barrel only COUNTS as correct when BOTH
//    (a) the right RFID tag is sitting on its reader, and
//    (b) the Balancing Scale has reported that spice weighed correctly
//        (MermaidsTale/BalancingScale/<Spice> = "true", sent once by the
//        scale on the transition - see Balancing-Scale printSuccessToMQTT).
//  Either order works: weigh first then place, or place first then weigh.
//
//  FEEDBACK LIGHTS: LEDS_PER_BARREL bullets per barrel on one string.
//    off    = empty, wrong barrel, or right barrel not yet weighed
//    YELLOW = that barrel counts as correct
//    GREEN  = all five correct (puzzle solved)
//
//  Publish layers:
//  1) PULSE layer - MermaidsTale/MiniBarrels/<Spice>
//     A 2s pulse per placement: "True" (counts as correct) or "False"
//     (wrong barrel, or right barrel not yet weighed), then "Clear" 2s
//     later. Drives M3's per-barrel SFX; M3 needs the Clear to re-arm its
//     own <Spice>Latch (those Latch topics belong to M3 - never published
//     here). When a weigh-in arrives for a barrel already seated right,
//     a True pulse fires at that moment (the moment it registers).
//
//  2) SEATED layer - MermaidsTale/MiniBarrels/system/<Spice> (retained)
//     Real state per reader: Clear | False | Unweighed | True.
//     "Unweighed" = right tag, scale hasn't credited it yet. The AI
//     character reacts only to true/false, so its praise line lands when
//     the barrel actually registers. checkSolved(): all five True ->
//     status=SOLVED (M3 fires the barrel-piston finale). Seated memory
//     clears only on a different tag or PUZZLE_RESET - NEVER on reader
//     silence (readers re-report a seated tag erratically, 4s to ~2min).
//
//  3) WEIGH layer - MermaidsTale/MiniBarrels/system/weighed/<Spice>
//     (retained true|false). The scale's credit is non-retained and the
//     scale never sends "false", so this board keeps its own retained
//     mirror and re-reads it on boot: a mid-game reboot keeps the credits.
//     Credits clear on PUZZLE_RESET to this board OR to the scale (M3's
//     game reset sends the scale one).
//
//  Hardening (v3.0.0, mirrors SunDial Bridge 4.3.0):
//    - MQTT LWT retained OFFLINE on /status, 30s task WDT, 2min offline
//      self-reboot, non-blocking MQTT retry, republishAll() on reconnect.
//================================================

#include <WiFi.h>
#include <PubSubClient.h>
#include <HardwareSerial.h>
#include <SoftwareSerial.h>
#include <FastLED.h>
#include <esp_task_wdt.h>
#include <stdarg.h>
#include "MANIFEST.h"   // single source of truth: tag IDs, version, broker, pins, lights

// Identity/version come from MANIFEST.h; these bridge the manifest names
// onto the names the code uses.
#define VERSION                FIRMWARE_VERSION
#define PROP_NAME              DEVICE_NAME
#define NUM_SPICES             5
#define ID_LEN                 12
#define TOPIC_BUF              64
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

// Feedback lights (pin / count / order / brightness live in MANIFEST.h)
#define LED_COUNT  (NUM_SPICES * LEDS_PER_BARREL)
CRGB leds[LED_COUNT];
static const CRGB COLOR_OFF     = CRGB(0, 0, 0);
static const CRGB COLOR_CORRECT = CRGB(255, 200, 0);   // yellow
static const CRGB COLOR_SOLVED  = CRGB(0, 255, 0);     // green
bool lightsDirty = true;                               // render on next loop

// WiFi + MQTT. Broker address/port come from MANIFEST.h.
static const char* WIFI_SSID   = "AlchemyGuest";
static const char* WIFI_PASS   = "VoodooVacation5601";
static const char* MQTT_SERVER = BROKER_IP;
static const int   MQTT_PORT   = BROKER_PORT;

// All topics share one root (also used to build the per-spice topics).
#define TOPIC_ROOT "MermaidsTale/MiniBarrels/"
static const char* TOPIC_BASE           = TOPIC_ROOT;
static const char* MQTT_TOPIC_STATUS    = TOPIC_ROOT "status";
static const char* MQTT_TOPIC_LOG       = TOPIC_ROOT "log";
static const char* MQTT_TOPIC_COMMAND   = TOPIC_ROOT "command";
static const char* SCALE_TOPIC_COMMAND  = SCALE_TOPIC_ROOT "command";

//================================================
//            Per-spice state
//================================================
// What the reader physically sees.
enum SpiceState { ST_CLEAR = 0, ST_FALSE = 1, ST_TRUE = 2 };
static const char* const STATE_NAMES[] = { "Clear", "False", "True" };

// What goes on the retained system/<Spice> topic (seated + weigh credit).
enum SysWord { W_CLEAR = 0, W_FALSE = 1, W_UNWEIGHED = 2, W_TRUE = 3 };
static const char* const SYS_NAMES[] = { "Clear", "False", "Unweighed", "True" };

struct Spice {
  const char* name;
  const char* expected;             // expected UID for "True" (12 chars, from MANIFEST.h)
  Stream*     port;
  char        topic[TOPIC_BUF];        // pulse topic  (MermaidsTale/MiniBarrels/<Spice>)
  char        sysTopic[TOPIC_BUF];     // seated topic (.../system/<Spice>, retained)
  char        weighedTopic[TOPIC_BUF]; // own retained weigh mirror (.../system/weighed/<Spice>)
  char        scaleTopic[TOPIC_BUF];   // scale's credit topic (MermaidsTale/BalancingScale/<Spice>)

  // runtime
  char          rx[ID_LEN];         // accumulating frame buffer
  byte          rxLen;
  bool          hasTag;             // tracking a UID right now
  char          lastUid[ID_LEN];
  SpiceState    seated;             // private memory of what is on this reader
  bool          weighed;            // scale has credited this spice this game
  bool          credited;           // seated==TRUE && weighed -> light + solve
  SysWord       lastSys;            // last word published on sysTopic
  unsigned long pulseClearAtMs;     // when to send the pulse's trailing Clear (0 = none)
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
// Order here = order of the bullets along the light string.
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
static_assert(LEDS_PER_BARREL >= 1, "LEDS_PER_BARREL must be at least 1");

WiFiClient   espClient;
PubSubClient mqtt(espClient);

// Puzzle solve state. SOLVED when all 5 barrels are credited (right tag AND
// weighed). M3 event 26 "Mini Barrels Solved" is gated on status=SOLVED
// (fires the piston, objectives, GoldSolved). Edge-triggered.
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
//            Feedback lights
//================================================
void renderLights() {
  for (byte i = 0; i < NUM_SPICES; i++) {
    CRGB c = puzzleSolved ? COLOR_SOLVED
           : (spices[i].credited ? COLOR_CORRECT : COLOR_OFF);
    for (byte p = 0; p < LEDS_PER_BARREL; p++)
      leds[i * LEDS_PER_BARREL + p] = c;
  }
  FastLED.show();
  lightsDirty = false;
}

// Boot / LIGHTS_TEST: every bullet RED, then GREEN, then BLUE, then off.
// Lets a tech confirm the string is wired and see the color order
// (if "RED" shows green, change LED_COLOR_ORDER in MANIFEST.h).
void lightsSelfTest() {
  const CRGB seq[3] = { CRGB(255, 0, 0), CRGB(0, 255, 0), CRGB(0, 0, 255) };
  for (byte k = 0; k < 3; k++) {
    fill_solid(leds, LED_COUNT, seq[k]);
    FastLED.show();
    delay(300);
    esp_task_wdt_reset();
  }
  lightsDirty = true;   // renderLights() restores the real state next loop
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

SysWord sysWordOf(const Spice& s) {
  if (s.seated == ST_TRUE)  return s.weighed ? W_TRUE : W_UNWEIGHED;
  if (s.seated == ST_FALSE) return W_FALSE;
  return W_CLEAR;
}

// Re-publish the retained layer (seated words + weigh credits + solve
// status) so the broker and every watcher re-sync after a reconnect or
// broker restart. Pulse topics get their resting Clear - never while a
// pulse is in flight (that would cut a sound trigger short). Weigh mirrors
// are only re-pushed when TRUE: a fresh boot's "false" must not clobber a
// retained credit that is about to arrive on subscribe.
void republishAll() {
  for (byte i = 0; i < NUM_SPICES; i++) {
    mqtt.publish(spices[i].sysTopic, SYS_NAMES[sysWordOf(spices[i])], true);
    if (spices[i].weighed) mqtt.publish(spices[i].weighedTopic, "true", true);
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
    mqtt.subscribe(SCALE_TOPIC_COMMAND);          // scale PUZZLE_RESET clears credits
    for (byte i = 0; i < NUM_SPICES; i++) {
      mqtt.subscribe(spices[i].scaleTopic);       // scale credit ("true")
      mqtt.subscribe(spices[i].weighedTopic);     // own retained mirror (boot recovery)
    }
    republishAll();   // retained ONLINE overwrites stale OFFLINE + full re-sync
    mqttLogf("%s v%s online", PROP_NAME, VERSION);
  } else {
    Serial.printf("MQTT failed rc=%d\n", mqtt.state());
  }
}

//================================================
//            State machine helpers
//================================================
byte creditedCount() {
  byte n = 0;
  for (byte i = 0; i < NUM_SPICES; i++) if (spices[i].credited) n++;
  return n;
}
byte weighedCount() {
  byte n = 0;
  for (byte i = 0; i < NUM_SPICES; i++) if (spices[i].weighed) n++;
  return n;
}

// Recompute a barrel's credited flag from seated + weighed, mirror the
// resulting word to retained system/<Spice>, and flag the lights.
void syncSpice(Spice& s) {
  SysWord w = sysWordOf(s);
  bool nowCredited = (w == W_TRUE);
  if (nowCredited != s.credited) {
    s.credited  = nowCredited;
    lightsDirty = true;
  }
  if (w != s.lastSys) {
    s.lastSys = w;
    mqtt.publish(s.sysTopic, SYS_NAMES[w], true);   // retained truth
    Serial.printf("[%s] system=%s\n", s.name, SYS_NAMES[w]);
  }
}

// Update the reader's physical memory for one barrel.
void setSeated(Spice& s, SpiceState newState) {
  if (s.seated == newState) return;
  s.seated = newState;
  syncSpice(s);
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

// Scale credit for one spice arrives (or is cleared). If the right barrel
// is already sitting on its reader, this is the moment it registers as
// correct: fire the True pulse so M3 plays the SFX now.
void setWeighed(Spice& s, bool on, bool announce) {
  if (s.weighed == on) return;
  s.weighed = on;
  mqtt.publish(s.weighedTopic, on ? "true" : "false", true);
  bool wasCredited = s.credited;
  syncSpice(s);
  if (announce) mqttLogf("%s weighed %s (credits %u/%u)", s.name, on ? "OK" : "cleared",
                         (unsigned)weighedCount(), (unsigned)NUM_SPICES);
  if (s.credited && !wasCredited) firePulse(s, ST_TRUE);
}

void clearAllWeighed(const char* why) {
  for (byte i = 0; i < NUM_SPICES; i++) setWeighed(spices[i], false, false);
  mqttLogf("weigh credits cleared (%s)", why);
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

// Publish status=SOLVED once when all 5 barrels are credited; revert to
// ONLINE (edge-triggered) if a credited barrel is displaced or a credit
// is cleared.
void checkSolved() {
  bool all = (creditedCount() == NUM_SPICES);
  if (all && !puzzleSolved) {
    puzzleSolved = true;
    lightsDirty  = true;
    mqtt.publish(MQTT_TOPIC_STATUS, "SOLVED", true);
    mqttLogf("%s SOLVED - all 5 barrels placed and weighed", PROP_NAME);
  } else if (!all && puzzleSolved) {
    puzzleSolved = false;
    lightsDirty  = true;
    mqtt.publish(MQTT_TOPIC_STATUS, "ONLINE", true);
    mqttLogf("%s unsolved - a barrel changed", PROP_NAME);
  }
}

//================================================
//            WatchTower / MQTT command handling
//================================================
// Report correct-barrel count as a quick diagnostic state string.
// Protocol standard: the reply goes back on /command (same as PONG).
void promptStatus() {
  char reply[64];
  snprintf(reply, sizeof(reply), "%s|%u/%u|UP:%lus|V%s",
           puzzleSolved ? "SOLVED" : "PLAYING",
           (unsigned)creditedCount(), (unsigned)NUM_SPICES, millis() / 1000UL, VERSION);
  mqtt.publish(MQTT_TOPIC_COMMAND, reply);
  mqttLogf("STATUS -> %s (weighed %u/%u)", reply, (unsigned)weighedCount(), (unsigned)NUM_SPICES);
}

// The only path that empties the seated memory. Wipe every layer and
// cancel in-flight pulses. A barrel left seated re-announces on its next
// reader re-poll (4s-2min) and simply counts again (once re-weighed).
void puzzleReset() {
  for (byte i = 0; i < NUM_SPICES; i++) {
    spices[i].hasTag         = false;
    spices[i].pulseClearAtMs = 0;
    memset(spices[i].lastUid, 0, ID_LEN);
    setWeighed(spices[i], false, false);
    setSeated(spices[i], ST_CLEAR);
    mqtt.publish(spices[i].topic, STATE_NAMES[ST_CLEAR], true);
  }
  puzzleSolved = false;
  lightsDirty  = true;
  mqtt.publish(MQTT_TOPIC_STATUS, "ONLINE", true);
  mqttLogf("PUZZLE_RESET -> all layers cleared");
}

static bool payloadTrue(const char* p)  { return !strcasecmp(p, "true")  || !strcmp(p, "1"); }
static bool payloadFalse(const char* p) { return !strcasecmp(p, "false") || !strcmp(p, "0") || !strcasecmp(p, "clear"); }

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

  // ---- Balancing Scale: PUZZLE_RESET on its command topic clears credits.
  // (Everything else on that topic - PING/PONG/OK/STATUS - is not ours.)
  if (strcmp(topic, SCALE_TOPIC_COMMAND) == 0) {
    if (strcmp(msg, "PUZZLE_RESET") == 0) clearAllWeighed("scale PUZZLE_RESET");
    return;
  }

  // ---- Per-spice: scale credit, or our own retained mirror on boot.
  for (byte i = 0; i < NUM_SPICES; i++) {
    Spice& s = spices[i];
    if (strcmp(topic, s.scaleTopic) == 0) {
      if (payloadTrue(msg))       setWeighed(s, true,  true);
      else if (payloadFalse(msg)) setWeighed(s, false, true);
      return;
    }
    if (strcmp(topic, s.weighedTopic) == 0) {
      // Only ever RESTORE a credit from the mirror (retained "true" after a
      // reboot). "false" there is our own write - ignoring it can't hurt.
      if (payloadTrue(msg) && !s.weighed) {
        setWeighed(s, true, false);
        mqttLogf("%s weigh credit restored from retained mirror", s.name);
      }
      return;
    }
  }

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
    puzzleReset();
    mqtt.publish(MQTT_TOPIC_COMMAND, "OK");
    return;
  }
  if (strcmp(msg, "LIGHTS_TEST") == 0) {
    mqtt.publish(MQTT_TOPIC_COMMAND, "OK");
    lightsSelfTest();
    return;
  }
  Serial.printf("[MQTT] unknown command: %s\n", msg);
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
//            Reader scan
//================================================
// Drain one reader. On every complete STX...ETX frame, classify the UID and
// - if it is a NEW tag for this reader - update the seated memory and fire
// the SFX pulse (True only if the barrel counts, i.e. right tag AND weighed).
// Reader re-reports of the same seated tag are ignored.
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
          setSeated(s, ok ? ST_TRUE : ST_FALSE);          // seated memory -> credit -> solve
          firePulse(s, s.credited ? ST_TRUE : ST_FALSE);  // wire pulse -> M3 sound
          if (ok && !s.credited)
            mqttLogf("%s right barrel placed but NOT weighed yet", s.name);
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
    snprintf(spices[i].topic,        TOPIC_BUF, "%s%s",                TOPIC_BASE,       spices[i].name);
    snprintf(spices[i].sysTopic,     TOPIC_BUF, "%ssystem/%s",         TOPIC_BASE,       spices[i].name);
    snprintf(spices[i].weighedTopic, TOPIC_BUF, "%ssystem/weighed/%s", TOPIC_BASE,       spices[i].name);
    snprintf(spices[i].scaleTopic,   TOPIC_BUF, "%s%s",                SCALE_TOPIC_ROOT, spices[i].name);
    spices[i].rxLen          = 0;
    spices[i].hasTag         = false;
    spices[i].seated         = ST_CLEAR;
    spices[i].weighed        = false;
    spices[i].credited       = false;
    spices[i].lastSys        = W_CLEAR;
    spices[i].pulseClearAtMs = 0;
    memset(spices[i].lastUid, 0, ID_LEN);
  }
}

void setupLights() {
  FastLED.addLeds<WS2811, LED_DATA_PIN, LED_COLOR_ORDER>(leds, LED_COUNT);
  FastLED.setBrightness(LED_BRIGHTNESS);
  fill_solid(leds, LED_COUNT, COLOR_OFF);
  FastLED.show();
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

  setupLights();
  lightsSelfTest();   // R/G/B sweep the moment power lands - proves wiring

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
  if (lightsDirty) renderLights();   // one show() per change, never per loop
  heartBeat();
}
