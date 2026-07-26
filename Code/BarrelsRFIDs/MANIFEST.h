// ============================================================
// MANIFEST.h — WatchTower Device Manifest
// This file is parsed by sync_manifests.py for the WatchTower dashboard.
// Keep all values as #define strings unless noted otherwise.
//
// >>> TO CHANGE A BARREL'S RFID TAG: edit the ONE matching TAG_* line
//     in the "Barrel RFID Tag IDs" section below, save, and re-flash.
//     Nothing else needs to change. <<<
// ============================================================

#pragma once

#define DEVICE_NAME           "MiniBarrels"
#define FIRMWARE_VERSION      "2.7.1"
#define BOARD_TYPE            "ESP32-S3"
#define ROOM                  "MermaidsTale"
#define DESCRIPTION           "Five RFID barrel readers. Each reader publishes True/False/Clear (retained) for its spice; when all five correct barrels are present the board publishes status=SOLVED, which drives the M3 barrel-piston finale."

#define BUILD_STATUS          "stable"
#define CODE_HEALTH           "good"
#define WATCHTOWER_COMPLIANCE "full"

// MQTT
#define BROKER_IP             "10.1.10.115"
#define BROKER_PORT           1883
#define HEARTBEAT_MS_MANIFEST 300000

#define SUBSCRIBE_TOPICS      "MermaidsTale/MiniBarrels/command"
#define PUBLISH_TOPICS        "MermaidsTale/MiniBarrels/status, MermaidsTale/MiniBarrels/log, MermaidsTale/MiniBarrels/{Vanilla|Cloves|Molasses|SugarCane|Yeast}"
#define SUPPORTED_COMMANDS    "PING, STATUS, RESET, PUZZLE_RESET"

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

// Hardware — RFID reader RX pins (one UART per barrel)
#define PIN_CONFIG            "VANILLA_RX=4 (UART0), CLOVES_RX=5 (UART1), MOLASSES_RX=6 (UART2), SUGARCANE_RX=7 (SoftSerial), YEAST_RX=15 (SoftSerial)"
#define COMPONENTS            "5x serial RFID readers (STX/ETX framed, 9600 baud). Vanilla/Cloves/Molasses on hardware UARTs, SugarCane/Yeast on SoftwareSerial."
#define KNOWN_QUIRKS          "Vanilla on UART0 requires 'USB CDC On Boot = Enabled' so Serial does not steal UART0. Clear fires only after REMOVAL_TIMEOUT_MS (2000ms) of TOTAL reader silence, so garbled frames during WiFi/SoftSerial contention do not false-trigger removal. Tag IDs live in MANIFEST.h - edit there, not in the .ino."

#define REPO_URL              "https://github.com/Alchemy-Escape-Rooms-Inc/MiniBarrels"
