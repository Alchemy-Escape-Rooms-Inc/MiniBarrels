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

MQTT (broker 10.1.10.115):
  MermaidsTale/MiniBarrels/order   {"order":["Red","Yellow",...], "slots":[...]}
  published (not retained) whenever the stable order changes, and every 30 s.
  MermaidsTale/MiniBarrels/status  SOLVED (retained) when the order matches
  config "solution" - the exact message the RFID board sends on a solve, which
  M3 event 26 "Mini Barrels Solved" listens for. ONLINE (retained) if it is
  broken again. Also a line on MermaidsTale/MiniBarrels/log.
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
    "max_color_dist": 45.0,    # Lab distance above which a splotch is "unknown"
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


def run_forever(cfg, interval):
    import paho.mqtt.client as mqtt
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="BarrelCam")
    client.connect_async(cfg["mqtt_host"], 1883, 30)
    client.loop_start()
    g = FrameGrabber(rtsp_url(cfg))
    last_sent, last_sent_at, candidate, streak = None, 0.0, None, 0
    solved = False
    solution = tuple(cfg["solution"])
    if not solution:
        print("NOTE: no 'solution' in config - will report order but never SOLVED", flush=True)
    print("watching barrels; publishing to", cfg["mqtt_topic"], flush=True)
    while True:
        frame = g.latest()
        if frame is None:
            time.sleep(interval)
            continue
        names = classify(read_slots(frame, cfg), cfg)
        order = tuple(n for n, _ in names)
        streak = streak + 1 if order == candidate else 1
        candidate = order
        # need 2 matching reads in a row so a hand over a barrel doesn't count
        if streak >= 2 and (order != last_sent or time.time() - last_sent_at > 30):
            msg = json.dumps(payload(names, cfg))
            client.publish(cfg["mqtt_topic"], msg)
            if order != last_sent:
                print(time.strftime("%H:%M:%S"), msg, flush=True)
            last_sent, last_sent_at = order, time.time()
            now_solved = bool(solution) and order == solution
            if now_solved and not solved:
                client.publish(cfg["status_topic"], "SOLVED", retain=True)
                client.publish(cfg["log_topic"], "MiniBarrels SOLVED - camera saw barrel order " + ",".join(order))
                print(time.strftime("%H:%M:%S"), "SOLVED", flush=True)
            elif solved and not now_solved:
                client.publish(cfg["status_topic"], "ONLINE", retain=True)
                client.publish(cfg["log_topic"], "MiniBarrels unsolved - a barrel moved")
                print(time.strftime("%H:%M:%S"), "unsolved", flush=True)
            solved = now_solved
        time.sleep(interval)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--calibrate", action="store_true")
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--debug", action="store_true", help="save annotated barrel_debug.jpg")
    ap.add_argument("--interval", type=float, default=1.0)
    a = ap.parse_args()
    cfg = load_config()
    if a.calibrate:
        calibrate(cfg, a.debug)
    elif a.once:
        run_once(cfg, a.debug)
    else:
        run_forever(cfg, a.interval)


if __name__ == "__main__":
    main()
