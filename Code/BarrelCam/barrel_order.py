"""
barrel_order.py - read the order of the 5 MiniBarrels from the Ship camera.

Each barrel has a paint splotch on its lid. The barrels sit in a fixed tray, so
we look at 5 fixed "slots" in the camera picture, find the splotch on each lid,
and match its color against colors recorded during calibration.

  Slot 1 = the tray end nearest the ship wheel (top of the camera picture)
  Slot 5 = the tray end nearest the camera (bottom of the picture)

Usage
  python barrel_order.py --calibrate      # barrels on tray -> learn each splotch color
  python barrel_order.py --once           # print the current order once
  python barrel_order.py --once --debug   # also save barrel_debug.jpg (annotated)
  python barrel_order.py                  # run forever, publish order changes to MQTT

Calibration: put the barrels on the tray, run --calibrate, and it records the
color in each slot under the names in config "calibrate_names" (slot 1 first).
Recalibrate under the real game lighting (the room lights change the colors).

Camera password: env REOLINK_PASSWORD, or a one-line file .camera_password
next to this script (git-ignored).

MQTT (broker 10.1.10.115): this REPLACES the BarrelsRFIDs board and speaks
its exact protocol - see class BarrelBoard for every topic. Unplug the RFID
board. The lights (MiniBarrelsLights) follow system/<Spice> + status unchanged.
  MermaidsTale/MiniBarrels/order  JSON order, whenever it changes.
"""
import argparse
import json
import os
import sys
import threading
import time

os.environ.setdefault("OPENCV_FFMPEG_CAPTURE_OPTIONS", "rtsp_transport;tcp")
import cv2  # noqa: E402
import numpy as np  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(HERE, "barrel_cam_config.json")

DEFAULT_CONFIG = {
    "camera_ip": "10.1.10.67",
    "camera_user": "admin",
    "stream": "h264Preview_01_main",
    "mqtt_host": "10.1.10.115",
    "mqtt_topic": "MermaidsTale/MiniBarrels/order",
    # same solve message the RFID board sends (M3 event 26 "Mini Barrels Solved")
    "status_topic": "MermaidsTale/MiniBarrels/status",
    "log_topic": "MermaidsTale/MiniBarrels/log",
    # winning order of splotch colors, slot 1 first. Empty = never solves.
    "solution": [],
    # slot centers in the full-res 2560x1920 picture, slot 1 first
    "slots": [[1782, 469], [1695, 677], [1591, 897], [1487, 1140], [1383, 1429]],
    "lid_radius": 85,          # px around slot center that is searched
    "max_color_dist": 60.0,    # Lab distance above which a splotch is "unknown"
    "min_blob_px": 250,        # smaller than this -> no splotch / empty slot
    "calibrate_names": ["Red", "Yellow", "Green", "Blue", "Black"],
    # filled by --calibrate: name -> [L, a, b]
    "colors": {},
    # optional: splotch color -> barrel name, e.g. {"Red": "Molasses"}
    "barrel_names": {},
}


def load_config():
    cfg = dict(DEFAULT_CONFIG)
    if os.path.exists(CONFIG_PATH):
        with open(CONFIG_PATH, "r", encoding="utf-8") as f:
            cfg.update(json.load(f))
    return cfg


def save_config(cfg):
    with open(CONFIG_PATH, "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=2)


def camera_password():
    pw = os.environ.get("REOLINK_PASSWORD")
    if pw:
        return pw
    p = os.path.join(HERE, ".camera_password")
    if os.path.exists(p):
        with open(p, "r", encoding="utf-8") as f:
            return f.read().strip()
    sys.exit("No camera password: set REOLINK_PASSWORD or create .camera_password")


def rtsp_url(cfg):
    return "rtsp://%s:%s@%s:554/%s" % (
        cfg["camera_user"], camera_password(), cfg["camera_ip"], cfg["stream"])


class FrameGrabber:
    """Keeps reading the stream in the background so we always get the newest frame."""

    def __init__(self, url):
        self.url = url
        self.frame = None
        self.stamp = 0.0
        self.lock = threading.Lock()
        self.running = True
        threading.Thread(target=self._loop, daemon=True).start()

    def _loop(self):
        while self.running:
            cap = cv2.VideoCapture(self.url, cv2.CAP_FFMPEG)
            if not cap.isOpened():
                print("[cam] cannot open stream, retrying in 5 s", flush=True)
                time.sleep(5)
                continue
            while self.running:
                ok, frame = cap.read()
                if not ok:
                    print("[cam] stream dropped, reconnecting", flush=True)
                    break
                with self.lock:
                    self.frame, self.stamp = frame, time.time()
            cap.release()
            time.sleep(1)

    def latest(self, max_age=3.0, timeout=15.0):
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                if self.frame is not None and time.time() - self.stamp < max_age:
                    return self.frame.copy()
            time.sleep(0.1)
        return None


def splotch_color(lab, center, radius, min_px):
    """Find the splotch inside one lid; return (mean Lab, blob mask, pixel count)."""
    h, w = lab.shape[:2]
    cx, cy = center
    disk = np.zeros((h, w), np.uint8)
    cv2.circle(disk, (cx, cy), radius, 255, -1)
    inner = np.zeros((h, w), np.uint8)
    cv2.circle(inner, (cx, cy), int(radius * 0.75), 255, -1)

    # lid color = median of the whole disk (the splotch is the minority)
    lid = np.median(lab[disk > 0].astype(np.float32), axis=0)
    labf = lab.astype(np.float32)
    diff = np.linalg.norm(labf - lid, axis=2)
    chroma = np.hypot(labf[..., 1] - 128, labf[..., 2] - 128)
    # glare = brighter than the lid and colorless -> never part of a splotch
    glare = (labf[..., 0] > lid[0] + 8) & (chroma < 22)
    cand = ((diff > 28) & (inner > 0) & ~glare).astype(np.uint8) * 255
    cand = cv2.morphologyEx(cand, cv2.MORPH_OPEN, np.ones((5, 5), np.uint8))

    n, labels, stats, cents = cv2.connectedComponentsWithStats(cand)
    if n <= 1:
        return None, None, 0
    # prefer big blobs close to the slot center
    best, best_score = 0, -1e9
    for i in range(1, n):
        area = stats[i, cv2.CC_STAT_AREA]
        d = np.hypot(cents[i][0] - cx, cents[i][1] - cy)
        score = area - 8 * d
        if score > best_score:
            best, best_score = i, score
    area = int(stats[best, cv2.CC_STAT_AREA])
    if area < min_px:
        return None, None, area
    mask = labels == best
    px = labf[mask]
    keep = px[px[:, 0] < 235]          # drop blown-out highlights on the glossy paint
    if len(keep) >= min_px // 2:
        px = keep
    return np.median(px, axis=0), mask, area


def read_slots(frame, cfg):
    lab = cv2.cvtColor(cv2.GaussianBlur(frame, (5, 5), 0), cv2.COLOR_BGR2LAB)
    results = []
    for (x, y) in cfg["slots"]:
        color, mask, area = splotch_color(lab, (int(x), int(y)), cfg["lid_radius"], cfg["min_blob_px"])
        results.append({"lab": color, "mask": mask, "area": area})
    return results


def classify(results, cfg):
    names = []
    refs = {k: np.array(v, np.float32) for k, v in cfg["colors"].items()}
    for r in results:
        if r["lab"] is None:
            names.append(("empty", None))
            continue
        if not refs:
            names.append(("uncalibrated", None))
            continue
        dists = {k: float(np.linalg.norm(r["lab"] - v)) for k, v in refs.items()}
        best = min(dists, key=dists.get)
        if dists[best] > cfg["max_color_dist"]:
            names.append(("unknown", dists[best]))
        else:
            names.append((best, dists[best]))
    # same color claimed twice -> keep the closer one, mark the other unknown
    seen = {}
    for i, (n, d) in enumerate(names):
        if d is None or n in ("unknown",):
            continue
        if n in seen:
            j = seen[n]
            loser = i if d > names[j][1] else j
            names[loser] = ("unknown", names[loser][1])
            if loser == j:
                seen[n] = i
        else:
            seen[n] = i
    return names


def draw_debug(frame, cfg, results, names, path):
    img = frame.copy()
    for i, ((x, y), r, (n, d)) in enumerate(zip(cfg["slots"], results, names)):
        if r["mask"] is not None:
            img[r["mask"]] = (0.5 * img[r["mask"]] + [0, 127, 0]).astype(np.uint8)
        cv2.circle(img, (int(x), int(y)), cfg["lid_radius"], (255, 255, 255), 3)
        label = "%d: %s" % (i + 1, n) + ("" if d is None else " (%.0f)" % d)
        cv2.putText(img, label, (int(x) - 420, int(y) + 10), cv2.FONT_HERSHEY_SIMPLEX,
                    1.4, (0, 0, 0), 6)
        cv2.putText(img, label, (int(x) - 420, int(y) + 10), cv2.FONT_HERSHEY_SIMPLEX,
                    1.4, (255, 255, 255), 2)
    cv2.imwrite(path, img)
    print("debug image:", path)


def grab_one(cfg):
    g = FrameGrabber(rtsp_url(cfg))
    frame = g.latest()
    g.running = False
    if frame is None:
        sys.exit("Could not get a frame from the camera")
    return frame


def calibrate(cfg, debug):
    frame = grab_one(cfg)
    results = read_slots(frame, cfg)
    colors = {}
    for name, r in zip(cfg["calibrate_names"], results):
        if r["lab"] is None:
            print("slot for %s: NO SPLOTCH FOUND (area %d) - not saved" % (name, r["area"]))
            continue
        colors[name] = [round(float(v), 1) for v in r["lab"]]
        print("slot for %-8s Lab=%s  (%d px)" % (name, colors[name], r["area"]))
    names = list(colors)
    for i in range(len(names)):
        for j in range(i + 1, len(names)):
            d = np.linalg.norm(np.array(colors[names[i]]) - np.array(colors[names[j]]))
            if d < 2 * cfg["max_color_dist"] * 0.5:
                print("WARNING: %s and %s look alike (dist %.0f)" % (names[i], names[j], d))
    cfg["colors"] = colors
    save_config(cfg)
    print("saved", CONFIG_PATH)
    if debug:
        draw_debug(frame, cfg, results, classify(results, cfg), os.path.join(HERE, "barrel_debug.jpg"))


def payload(names, cfg):
    order = [n for n, _ in names]
    return {
        "order": order,
        "barrels": [cfg["barrel_names"].get(n, n) for n in order],
        "complete": all(n in cfg["colors"] for n in order),
        "ts": int(time.time()),
    }


def run_once(cfg, debug):
    frame = grab_one(cfg)
    results = read_slots(frame, cfg)
    names = classify(results, cfg)
    for i, (n, d) in enumerate(names):
        print("slot %d: %-12s %s" % (i + 1, n, "" if d is None else "dist %.1f" % d))
    print(json.dumps(payload(names, cfg)))
    if debug:
        draw_debug(frame, cfg, results, names, os.path.join(HERE, "barrel_debug.jpg"))


VERSION = "2.0.0"
PULSE_S = 2.0          # same 2 s True/False pulse the RFID board sent
HEARTBEAT_S = 15.0
STATUS_REASSERT_S = 60.0


class BarrelBoard:
    """Speaks the MQTT protocol of the old BarrelsRFIDs board, so M3, the AI
    characters, the MiniBarrelsLights board and WatchTower keep working with
    the camera in its place.

      MermaidsTale/MiniBarrels/<Spice>          True/False pulse when a barrel lands, Clear (retained) 2 s later (M3 sounds)
      MermaidsTale/MiniBarrels/system/<Spice>   retained Clear/False/True  (AI watchers + lights: True = yellow)
      MermaidsTale/MiniBarrels/status           retained ONLINE/SOLVED     (M3 event 26, lights all green)
      MermaidsTale/MiniBarrels/command          PING->PONG, STATUS, PUZZLE_RESET/RESET
      MermaidsTale/MiniBarrels/vision/status    retained ONLINE, LWT OFFLINE (WatchTower: script alive)
      MermaidsTale/MiniBarrels/vision/heartbeat JSON every 15 s           (WatchTower: camera seeing)
    """

    def __init__(self, cfg):
        import paho.mqtt.client as mqtt
        self.cfg = cfg
        self.root = cfg.get("_root", "MermaidsTale/MiniBarrels/")
        self.first_update = True   # no placement pulses for barrels already sitting there at start
        self.solution = tuple(cfg["solution"])
        self.color_to_spice = dict(cfg["barrel_names"])
        self.spices = [self.color_to_spice[c] for c in self.solution]
        self.sys = {sp: "Clear" for sp in self.spices}
        self.pulse_clear_at = {}
        self.solved = False
        # armed=False while the barrels sit in the winning order since start or
        # since a reset: guests must break the order and rebuild it to solve,
        # so a forgotten-solved tray can't fire the finale on its own.
        self.armed = False
        self.order = None
        self.lock = threading.Lock()
        self.pending = []
        self.last_status = None
        self.started = time.time()
        self._hb = 0.0
        self._sa = time.time()
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="BarrelCam")
        c.will_set(self.root + "vision/status", "OFFLINE", qos=1, retain=True)
        c.on_connect = self._on_connect
        c.on_message = self._on_message
        c.reconnect_delay_set(1, 10)
        c.connect_async(cfg["mqtt_host"], 1883, 30)
        c.loop_start()
        self.client = c

    # ---------- MQTT plumbing ----------
    def _on_connect(self, client, userdata, flags, rc, props=None):
        print("[mqtt] connected rc=%s" % rc, flush=True)
        client.subscribe(self.root + "command")
        if "_root" not in self.cfg:
            client.subscribe("MermaidsTale/BalancingScale/command")
        client.subscribe(self.root + "status")
        with self.lock:
            self.pending.append(("__connected__", ""))

    def _on_message(self, client, userdata, msg):
        with self.lock:
            self.pending.append((msg.topic, msg.payload.decode("utf-8", "replace").strip()))

    def pub(self, sub, payload, retain=False):
        self.client.publish(self.root + sub, payload, qos=1 if retain else 0, retain=retain)

    def log(self, text):
        print(time.strftime("%H:%M:%S"), text, flush=True)
        self.pub("log", "BarrelCam: " + text)

    def _publish_status(self):
        self.last_status = "SOLVED" if self.solved else "ONLINE"
        self.pub("status", self.last_status, retain=True)

    def _republish_all(self):
        self.pub("vision/status", "ONLINE", retain=True)
        for sp in self.spices:
            self.pub("system/" + sp, self.sys[sp], retain=True)
            if sp not in self.pulse_clear_at:
                self.pub(sp, "Clear", retain=True)
        self._publish_status()

    # ---------- commands (run on the main loop thread) ----------
    def handle_commands(self):
        with self.lock:
            msgs, self.pending = self.pending, []
        for topic, payload in msgs:
            up = payload.upper()
            if topic == "__connected__":
                self._republish_all()
            elif topic == self.root + "status":
                # e.g. the unplugged RFID board's last-will OFFLINE landing on
                # our status -> put the truth back
                if self.last_status and payload != self.last_status:
                    self._publish_status()
            elif topic.endswith("BalancingScale/command"):
                if up == "PUZZLE_RESET":
                    self.reset("scale PUZZLE_RESET")
            elif up == "PING":
                self.pub("command", "PONG")
            elif up == "STATUS":
                n = sum(1 for v in self.sys.values() if v == "True")
                self.pub("command", "%s|%d/%d|UP:%ds|CAMERA|V%s" % (
                    "SOLVED" if self.solved else "PLAYING", n, len(self.spices),
                    int(time.time() - self.started), VERSION))
            elif up in ("PUZZLE_RESET", "RESET"):
                self.reset(up)

    def reset(self, why):
        self.solved = False
        self.armed = self.order != self.solution
        self.pulse_clear_at.clear()
        self._republish_all()
        self.log("%s -> unsolved%s" % (why, "" if self.armed else
                 " (barrels still in the winning order - one must move before it can solve)"))

    # ---------- state from the camera ----------
    def update(self, order):
        """order = tuple of color names per slot ('empty'/'unknown' allowed)."""
        self.order = order
        now = time.time()
        where = {c: i for i, c in enumerate(order) if c in self.color_to_spice}
        for color, sp in self.color_to_spice.items():
            if color in where:
                word = "True" if self.solution[where[color]] == color else "False"
            else:
                word = "Clear"
            if word == self.sys[sp]:
                continue
            old, self.sys[sp] = self.sys[sp], word
            self.pub("system/" + sp, word, retain=True)
            if word in ("True", "False") and not self.first_update:
                self.pub(sp, word)                     # placement pulse, like the reader
                self.pulse_clear_at[sp] = now + PULSE_S
            self.log("%s %s -> %s" % (sp, old, word))
        self.first_update = False
        is_win = order == self.solution
        if not is_win:
            self.armed = True
        if is_win and self.armed and not self.solved:
            self.solved = True
            self._publish_status()
            self.log("MiniBarrels SOLVED - all 5 barrels in order " + ",".join(self.spices))
        elif self.solved and not is_win:
            self.solved = False
            self._publish_status()
            self.log("MiniBarrels unsolved - a barrel moved")

    def service(self, cam_ok):
        now = time.time()
        for sp, t in list(self.pulse_clear_at.items()):
            if now >= t:
                self.pub(sp, "Clear", retain=True)
                del self.pulse_clear_at[sp]
        if now - self._hb >= HEARTBEAT_S:
            self._hb = now
            self.pub("vision/heartbeat", json.dumps({
                "camera": "ok" if cam_ok else "no_frame",
                "order": list(self.order or ()),
                "barrels": [self.color_to_spice.get(c, c) for c in (self.order or ())],
                "solved": self.solved, "armed": self.armed, "v": VERSION}))
        if now - self._sa >= STATUS_REASSERT_S:
            self._sa = now
            self.pub("vision/status", "ONLINE", retain=True)


def run_forever(cfg, interval):
    if not cfg["solution"] or not cfg["barrel_names"]:
        sys.exit("config needs 'solution' and 'barrel_names' to run as the puzzle")
    board = BarrelBoard(cfg)
    g = FrameGrabber(rtsp_url(cfg))
    from collections import Counter, deque
    nslots = len(cfg["slots"])
    history = [deque(maxlen=5) for _ in range(nslots)]
    # None = slot not decided yet. Nothing is reported until EVERY slot has a
    # majority, so the first report is the real starting layout (2026-11-07:
    # a half-settled first read looked like "order broken" -> armed -> the next
    # read "solved" it and M3 fired the barrel finale at startup).
    stable = [None] * nslots
    print("BarrelCam v%s watching barrels (solution %s)" % (VERSION, ",".join(cfg["solution"])), flush=True)
    while True:
        board.handle_commands()
        frame = g.latest(timeout=interval)
        cam_ok = frame is not None
        if cam_ok:
            reads = [n for n, _ in classify(read_slots(frame, cfg), cfg)]
            # per-slot majority vote over the last 5 reads (~5 s): a hand
            # passing over, or a glare flicker, never changes a slot
            for i, r in enumerate(reads):
                history[i].append(r)
                top, cnt = Counter(history[i]).most_common(1)[0]
                if cnt >= 3:
                    stable[i] = top
            order = tuple(stable)
            if None not in order and order != board.order:
                board.update(order)
                board.pub("order", json.dumps(payload([(n, None) for n in order], cfg)))
        board.service(cam_ok)
        time.sleep(interval)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--calibrate", action="store_true")
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--debug", action="store_true", help="save annotated barrel_debug.jpg")
    ap.add_argument("--interval", type=float, default=1.0)
    ap.add_argument("--test-root", help="publish under this root instead of MermaidsTale/MiniBarrels/ (bench test)")
    a = ap.parse_args()
    cfg = load_config()
    if a.test_root:
        cfg["_root"] = a.test_root.rstrip("/") + "/"
    if a.calibrate:
        calibrate(cfg, a.debug)
    elif a.once:
        run_once(cfg, a.debug)
    else:
        run_forever(cfg, a.interval)


if __name__ == "__main__":
    main()
