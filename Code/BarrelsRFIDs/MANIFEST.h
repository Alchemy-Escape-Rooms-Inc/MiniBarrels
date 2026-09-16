// ============================================================
// MANIFEST.h - WatchTower Device Manifest
// This file is parsed by sync_manifests.py for the WatchTower dashboard.
// Keep all values as #define strings unless noted otherwise.
//
// >>> TO CHANGE A BARREL'S RFID TAG: edit the ONE matching TAG_* line
//     in the "Barrel RFID Tag IDs" section below, save, and re-flash.
//     Nothing else needs to change. <<<
// >>> TO CHANGE THE FEEDBACK LIGHTS: edit the "Feedback lights" section
//     (data pin, bullets per barrel, color order, brightness). <<<
// ============================================================

#pragma once

#define DEVICE_NAME           "MiniBarrels"
#define FIRMWARE_VERSION      "3.2.0"
#define BOARD_TYPE            "ESP32-S3"
#define ROOM                  "MermaidsTale"
#define DESCRIPTION           "Five RFID barrel readers with WS2811 feedback lights. A barrel counts as CORRECT only when the right tag is on its reader AND the BalancingScale has reported that spice weighed correctly (MermaidsTale/BalancingScale/<Spice>=true). Light per barrel: off (empty/wrong/right-but-unweighed), YELLOW (correct), the five barrel bullets GREEN when all five correct -> status=SOLVED -> M3 barrel-piston finale. Public MermaidsTale/MiniBarrels/<Spice> gets a 2s True/False PULSE per placement (M3 per-barrel SFX); retained .../system/<Spice> holds the real state (Clear/False/Unweighed/True)."

#define BUILD_STATUS          "stable"
#define CODE_HEALTH           "good"
#define WATCHTOWER_COMPLIANCE "full"

// MQTT
#define BROKER_IP             "10.1.10.115"
#define BROKER_PORT           1883
#define HEARTBEAT_MS_MANIFEST 300000

#define SUBSCRIBE_TOPICS      "MermaidsTale/MiniBarrels/command, MermaidsTale/BalancingScale/{Vanilla|Cloves|Molasses|SugarCane|Yeast} (weigh credit, 'true'), MermaidsTale/BalancingScale/command (PUZZLE_RESET clears weigh credits), MermaidsTale/MiniBarrels/system/weighed/{Spice} (own retained mirror, reboot recovery)"
#define PUBLISH_TOPICS        "MermaidsTale/MiniBarrels/status, MermaidsTale/MiniBarrels/log, MermaidsTale/MiniBarrels/{Vanilla|Cloves|Molasses|SugarCane|Yeast} (2s pulses), MermaidsTale/MiniBarrels/system/{Spice} (retained: Clear|False|Unweighed|True), MermaidsTale/MiniBarrels/system/weighed/{Spice} (retained true|false)"
#define SUPPORTED_COMMANDS    "PING, STATUS, RESET, PUZZLE_RESET, LIGHTS_TEST"

// The scale's per-pouch topics are built as SCALE_TOPIC_ROOT + spice name.
// Must match Balancing-Scale printSuccessToMQTT() (MQTT_TOPIC "/<Spice>").
#define SCALE_TOPIC_ROOT      "MermaidsTale/BalancingScale/"

// ------------------------------------------------------------
// Barrel RFID Tag IDs  (the correct/expected UID for each barrel)
// 12 hex chars each, exactly as the reader reports on the wire.
// EDIT HERE to swap a tag, then re-flash. The .ino reads these directly.
// Yeast UID captured off WatchTower wire log 2026-07-26 14:06 (new tag,
// replaced the original 0112D7B87A06).
// ------------------------------------------------------------
#define TAG_VANILLA           "51000D90AF63"
#define TAG_CLOVES            "0112D7B8710D"
#define TAG_MOLASSES          "51000C74FAD3"
#define TAG_SUGARCANE         "0112D7B8661A"
#define TAG_YEAST             "0112D7BB8CF3"

// ------------------------------------------------------------
// Feedback lights - one 15-bullet WS2811 string runs past all five
// barrels. Only five of the bullets are used, one per barrel; the rest
// stay dark. LED_BULLET_POSITIONS lists which bullet (counting from 1 at
// the data-in end of the string) belongs to each barrel, in reader order:
//   Vanilla, Cloves, Molasses, SugarCane, Yeast
// If a barrel lights the wrong bullet, fix the number here - not the .ino.
// If YELLOW shows up as some other color, change LED_COLOR_ORDER
// (WS2811 bullet strings ship as RGB, GRB or BRG - the boot self-test
// flashes RED, GREEN, BLUE in that order so you can tell which you have).
// ------------------------------------------------------------
#define LED_DATA_PIN          16       // GPIO for the string's data wire
#define LED_STRING_LENGTH     15       // total bullets on the string
#define LED_BULLET_POSITIONS  { 1, 4, 8, 11, 15 }   // 1-based bullet per barrel (Vanilla..Yeast)
#define LED_COLOR_ORDER       RGB      // RGB | GRB | BRG (FastLED order token)
#define LED_BRIGHTNESS        200      // 0-255 global cap

// Hardware - RFID reader RX pins (one UART per barrel) + light data pin
#define PIN_CONFIG            "VANILLA_RX=4 (UART0), CLOVES_RX=5 (UART1), MOLASSES_RX=6 (UART2), SUGARCANE_RX=7 (SoftSerial), YEAST_RX=15 (SoftSerial), LED_DATA=16 (WS2811 string)"
#define COMPONENTS            "5x serial RFID readers (STX/ETX framed, 9600 baud). Vanilla/Cloves/Molasses on hardware UARTs, SugarCane/Yeast on SoftwareSerial. 1x WS2811 12V bullet-pixel string, 15 bullets (FastLED), one bullet per barrel at positions 1/4/8/11/15 (LED_BULLET_POSITIONS), single data line on GPIO16."
#define KNOWN_QUIRKS          "MUST be built with 'USB CDC On Boot = Enabled' (CDCOnBoot=cdc) or Serial steals UART0 and kills the Vanilla reader. Readers re-report seated tags erratically (4s-2min) and go silent in between - NEVER add silence-based removal (v2.7.1/v2.7.2 flap bug); seated state clears only via different-tag or PUZZLE_RESET. <Spice>Latch topics on the wire belong to M3 (SFX bookkeeping), never publish to them. Tag IDs live in MANIFEST.h - edit there, not in the .ino. v3.0.0 hardening: LWT retained OFFLINE on /status, 30s task WDT, 2min offline self-reboot, non-blocking MQTT retry w/ full retained re-sync on reconnect; heartbeat = fleet-standard HEARTBEAT:STATE:UPxs:RSSIx. v3.1.0 weigh gate: the scale's BalancingScale/<Spice>=true is NON-retained and the scale never sends false - this board mirrors credits to retained system/weighed/<Spice> so a reboot mid-game keeps them; credits clear on PUZZLE_RESET to EITHER this board OR the scale (M3 game reset sends the scale one). A stale credit from an aborted game = send PUZZLE_RESET. Lights: 12V bullets need common ground with the S3 and ideally a 5V level shifter (74AHCT125) on the data line; 3.3V data straight in usually works for short runs. A right barrel that is NOT yet weighed pulses False on the wire (wrong-barrel SFX) and reads Unweighed on system/<Spice> - the AI ignores that value and praises only on True."

#define REPO_URL              "https://github.com/Alchemy-Escape-Rooms-Inc/MiniBarrels"
