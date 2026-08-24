"""Push a firmware image to a running display over the network (spec 10.4).

The Update page in the web UI does the same thing with a progress bar; this is
the scriptable version, and it is what docs/BRINGUP.md step 23 tells you to run
for the rollback survival test.  Stdlib only - no requests, no aiohttp.

    python tools/ota_upload.py lost.local build/lost_swan_firmware.bin
    python tools/ota_upload.py 192.168.1.42 build_xiao/lost_swan_firmware.bin --force

The display refuses an image built for the other board, and a release image
onto a display with simulated columns saved, unless --force is given.  Neither
refusal is arbitrary: the first drives STEP on the wrong GPIOs, and the second
comes back with five columns the new image cannot honour, faults them all and
drops EN.  A moving drum is refused regardless and cannot be forced.

Exit status is 0 only if the display accepted the image and said it was
rebooting.
"""
import argparse
import http.client
import json
import os
import sys
import time


def upload(host, path, force, timeout):
    size = os.path.getsize(path)
    print("%s -> %s  (%d bytes)" % (path, host, size))

    conn = http.client.HTTPConnection(host, 80, timeout=timeout)
    url = "/api/ota" + ("?force=1" if force else "")
    started = time.time()
    with open(path, "rb") as f:
        conn.putrequest("POST", url)
        conn.putheader("Content-Type", "application/octet-stream")
        conn.putheader("Content-Length", str(size))
        conn.endheaders()
        sent = 0
        while True:
            chunk = f.read(8192)
            if not chunk:
                break
            conn.send(chunk)
            sent += len(chunk)
            pct = (sent * 100) // size
            sys.stdout.write("\r  %3d%%  %d/%d bytes" % (pct, sent, size))
            sys.stdout.flush()
    print()

    resp = conn.getresponse()
    body = resp.read().decode("utf-8", "replace")
    conn.close()
    elapsed = time.time() - started

    try:
        doc = json.loads(body)
    except ValueError:
        print("HTTP %d, unparseable reply: %s" % (resp.status, body[:200]))
        return 1

    if doc.get("ok"):
        print("accepted in %.1f s: %d bytes to %s" % (elapsed, doc.get("bytes", 0),
                                                      doc.get("partition", "?")))
        print(doc.get("note", ""))
        return 0

    print("HTTP %d - refused (%s): %s" % (resp.status, doc.get("verdict", "?"),
                                          doc.get("err", "")))
    if doc.get("verdict") in ("wrong_board", "loses_simulation"):
        print("  (--force overrides this one; read why in the Update page first)")
    return 1


def wait_for(host, seconds):
    """Poll /api/state until the display answers again after its reboot."""
    print("waiting for the display to come back…")
    end = time.time() + seconds
    while time.time() < end:
        try:
            conn = http.client.HTTPConnection(host, 80, timeout=3)
            conn.request("GET", "/api/state")
            doc = json.loads(conn.getresponse().read().decode())
            conn.close()
            sys_ = doc.get("sys", {})
            print("back: %s on %s, uptime %ss, pending=%s"
                  % (sys_.get("version"), sys_.get("ota_partition"),
                     sys_.get("uptime_s"), sys_.get("ota_pending")))
            if sys_.get("ota_pending"):
                print("NOTE: it has not confirmed itself yet.  It will roll back if it "
                      "cannot reach the end of app_main, load config, tick both tasks and "
                      "start httpd within 120 s of boot.")
            return 0
        except Exception:                                  # noqa: BLE001
            time.sleep(2)
    print("it did not answer within %d s - check the console" % seconds)
    return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host", help="hostname or IP, e.g. lost.local")
    ap.add_argument("image", help="path to lost_swan_firmware.bin")
    ap.add_argument("--force", action="store_true",
                    help="override a wrong-board or loses-simulation refusal")
    ap.add_argument("--timeout", type=float, default=180.0)
    ap.add_argument("--no-wait", action="store_true",
                    help="do not wait for the display to come back")
    args = ap.parse_args()

    rc = upload(args.host, args.image, args.force, args.timeout)
    if rc != 0 or args.no_wait:
        return rc
    return wait_for(args.host, 90)


if __name__ == "__main__":
    sys.exit(main())
