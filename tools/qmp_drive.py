"""QMP driver for the gufile closed loop: screendump frames + sendkey input.

Usage:
  qmp_drive.py --port 4555 --shot-dir snapshots --interval 1 --max-shots 60
               --script "t2 ret t3 up up up t2 screendump main"
  t<N>      sleep N seconds
  <key>     sendkey (QEMU qcode name: up/down/left/right/ret/esc/spc/backspace/f2/f5/ctrl_r/...)
  <key>xN   sendkey N times with 300ms spacing (no typematic on QEMU PS/2)
  screendump NAME   capture framebuffer to <shot-dir>/NAME.ppm (+ .png)
Exit code 0 on clean completion, 1 on QMP error.
"""
import argparse, json, socket, struct, sys, time, os
from PIL import Image  # noqa: E402  (Pillow available on this machine)

def qmp_call(sock, cmd, args=None):
    payload = json.dumps({"execute": cmd, "arguments": args or {}})
    sock.sendall(payload.encode() + b"\n")
    while True:
        line = sock.makefile().readline()
        if not line:
            raise ConnectionError("QMP connection closed")
        msg = json.loads(line)
        if msg.get("event"):
            continue
        if msg.get("error"):
            raise RuntimeError(f"QMP {cmd} error: {msg['error']}")
        return msg.get("return")

def sendkey(sock, name, n=1):
    # name may be a chord "ctrl_r+c": a SINGLE send-key call with a key
    # array presses all keys down together, waits hold_time, then releases
    # them together - the guest sees a true chord (Ctrl held while 'c'
    # arrives). Sequential single-key calls would press+release ctrl_r
    # before 'c' arrives, losing the modifier (Task 7 closed loop).
    keys = [{"type": "qcode", "data": k} for k in name.split("+")]
    for i in range(n):
        qmp_call(sock, "send-key", {"keys": keys, "hold-time": 80})
        if i < n - 1:
            time.sleep(0.3)

def shot(sock, path_ppm):
    qmp_call(sock, "screendump", {"filename": path_ppm})
    # QEMU (>= 9.x) performs screendump asynchronously: the QMP reply
    # arrives before the PPM file is written. Poll briefly for it.
    for _ in range(100):
        if os.path.exists(path_ppm):
            break
        time.sleep(0.1)
    if not os.path.exists(path_ppm):
        raise FileNotFoundError(f"screendump did not produce {path_ppm}")
    png = path_ppm.replace(".ppm", ".png")
    Image.open(path_ppm).convert("RGB").save(png)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=4555)
    ap.add_argument("--shot-dir", default="snapshot")
    ap.add_argument("--interval", type=float, default=2.0)
    ap.add_argument("--max-shots", type=int, default=40)
    ap.add_argument("--script", default="")
    ap.add_argument("--qemu-pid", type=int, default=0)
    a = ap.parse_args()
    os.makedirs(a.shot_dir, exist_ok=True)
    time.sleep(1)
    for _ in range(30):
        try:
            sock = socket.create_connection(("127.0.0.1", a.port), timeout=1)
            break
        except OSError:
            time.sleep(1)
    else:
        sys.exit("cannot connect to QMP")
    qmp_call(sock, "qmp_capabilities")
    shots = 0
    tokens = a.script.split() if a.script else []
    i = 0
    stamp = time.strftime("%Y%m%d_%H%M%S")
    while True:
        if tokens and i < len(tokens):
            t = tokens[i]; i += 1
            if t.startswith("t"):
                try:
                    time.sleep(float(t[1:])); continue
                except ValueError:
                    pass
            if t == "screendump":
                name = tokens[i]; i += 1
                p = os.path.join(a.shot_dir, f"{stamp}_{name}.ppm")
                shot(sock, p); shots += 1; continue
            if t == "vmstop":
                qmp_call(sock, "stop"); continue
            if t == "vmcont":
                qmp_call(sock, "cont"); continue
            # key with optional xN ("downx3" = 3 presses); a bare "x" token
            # (e.g. the letter x) must not crash the driver (int("") ValueError
            # or an empty qcode sent to QEMU)
            parts = t.split("x")
            name = parts[0] if parts[0] else t
            n = 1
            if len(parts) > 1 and parts[1].isdigit():
                n = int(parts[1])
            sendkey(sock, name, n)
        else:
            time.sleep(a.interval)
            p = os.path.join(a.shot_dir, f"{stamp}_{(shots+1):03d}.ppm")
            shot(sock, p); shots += 1
        if a.qemu_pid:
            try:
                os.kill(a.qemu_pid, 0)
            except OSError:
                break
        if shots >= a.max_shots:
            break
    return 0

if __name__ == "__main__":
    sys.exit(main())
