#!/usr/bin/env python3
"""A minimal MQTT 3.1.1 broker, stdlib only, for bench-testing the display.

WHY THIS EXISTS.  Phase 4 makes MQTT the canonical external API (spec 10.2a,
10.3), and testing it needs a broker on the LAN.  Installing one on the dev
machine is a system change nobody asked for, and a public broker is not
somewhere to publish a countdown deadline.  This is ~350 lines of stdlib
Python that speaks enough MQTT to exercise everything the firmware does:
retained publishes, a wildcard subscription, Last Will and Testament, and
QoS 0/1.

It is a TEST FIXTURE, not a broker.  No TLS, no persistence, no QoS 2, no
session resumption, no flow control.  Do not point anything real at it.

    python tools/mqtt_broker.py [--port 1883] [--log broker.jsonl] [--quiet]

Every packet it handles is printed, and with --log every publish is appended
as one JSON object per line so a test can assert on the traffic:

    {"t": 12.34, "client": "swan-a8e8", "topic": "swan/state",
     "payload": "...", "qos": 0, "retain": true}
"""
import argparse
import json
import socket
import struct
import sys
import threading
import time

CONNECT, CONNACK, PUBLISH, PUBACK = 1, 2, 3, 4
SUBSCRIBE, SUBACK, UNSUBSCRIBE, UNSUBACK = 8, 9, 10, 11
PINGREQ, PINGRESP, DISCONNECT = 12, 13, 14

NAMES = {
    CONNECT: "CONNECT", CONNACK: "CONNACK", PUBLISH: "PUBLISH", PUBACK: "PUBACK",
    SUBSCRIBE: "SUBSCRIBE", SUBACK: "SUBACK", UNSUBSCRIBE: "UNSUBSCRIBE",
    UNSUBACK: "UNSUBACK", PINGREQ: "PINGREQ", PINGRESP: "PINGRESP",
    DISCONNECT: "DISCONNECT",
}


def topic_matches(filt, topic):
    """MQTT 3.1.1 4.7 wildcard matching: + is one level, # is the rest.

    '#' does not match a topic beginning with '$', but the display uses no
    $-topics so that rule is implemented for correctness rather than need.
    """
    if topic.startswith("$") and filt and filt[0] in "+#":
        return False
    f = filt.split("/")
    t = topic.split("/")
    for i, seg in enumerate(f):
        if seg == "#":
            return i == len(f) - 1
        if i >= len(t):
            return False
        if seg != "+" and seg != t[i]:
            return False
    return len(f) == len(t)


class Broker:
    def __init__(self, log_path=None, quiet=False):
        self.lock = threading.Lock()
        self.clients = {}          # client_id -> Client
        self.retained = {}         # topic -> (payload, qos)
        self.log_path = log_path
        self.quiet = quiet
        self.t0 = time.time()
        self.publishes = []        # every publish, for in-process assertions

    def say(self, *a):
        if not self.quiet:
            print("%7.3f " % (time.time() - self.t0), *a, flush=True)

    def record(self, client_id, topic, payload, qos, retain):
        rec = {
            "t": round(time.time() - self.t0, 3),
            "client": client_id,
            "topic": topic,
            "payload": payload.decode("utf-8", "replace"),
            "qos": qos,
            "retain": retain,
        }
        with self.lock:
            self.publishes.append(rec)
            if self.log_path:
                with open(self.log_path, "a", encoding="utf-8") as f:
                    f.write(json.dumps(rec) + "\n")
        short = rec["payload"]
        if len(short) > 110:
            short = short[:110] + "..."
        self.say("PUB  %-24s %s%s%s" % (topic, "[R] " if retain else "",
                                        "q%d " % qos if qos else "", short))

    def deliver(self, topic, payload, qos, retain, from_id):
        self.record(from_id, topic, payload, qos, retain)
        if retain:
            with self.lock:
                if payload:
                    self.retained[topic] = (payload, qos)
                else:
                    # A zero-length retained publish clears the retained value.
                    self.retained.pop(topic, None)
        with self.lock:
            targets = list(self.clients.values())
        for c in targets:
            for filt, sub_qos in list(c.subs.items()):
                if topic_matches(filt, topic):
                    c.send_publish(topic, payload, min(qos, sub_qos), False)
                    break


class Client(threading.Thread):
    def __init__(self, broker, sock, addr):
        super().__init__(daemon=True)
        self.b = broker
        self.sock = sock
        self.addr = addr
        self.buf = b""
        self.client_id = "?"
        self.subs = {}
        self.will = None           # (topic, payload, qos, retain)
        self.alive = True
        # Keepalive enforcement. Without it the will never fires for the case
        # it exists for: a device that vanishes without closing the TCP
        # connection - a power cut, a reset, a router that drops the flow.
        # Nothing arrives, nothing errors, and the socket sits open for ever.
        self.keepalive = 0
        self.last_seen = time.time()
        self.wlock = threading.Lock()
        self.next_id = 1

    # -- wire helpers -------------------------------------------------------
    def send(self, data):
        with self.wlock:
            try:
                self.sock.sendall(data)
            except OSError:
                self.alive = False

    @staticmethod
    def varint(n):
        out = b""
        while True:
            byte = n % 128
            n //= 128
            if n:
                byte |= 0x80
            out += bytes([byte])
            if not n:
                return out

    @staticmethod
    def utf8(s):
        raw = s.encode("utf-8") if isinstance(s, str) else s
        return struct.pack("!H", len(raw)) + raw

    def send_publish(self, topic, payload, qos, retain):
        flags = (qos << 1) | (1 if retain else 0)
        body = self.utf8(topic)
        if qos:
            body += struct.pack("!H", self.next_id)
            self.next_id = self.next_id % 65535 + 1
        body += payload
        self.send(bytes([PUBLISH << 4 | flags]) + self.varint(len(body)) + body)

    # -- reading ------------------------------------------------------------
    def recv_exact(self, n):
        while len(self.buf) < n:
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                # MQTT 3.1.1 3.1.2.10: if the keepalive is non-zero the server
                # MUST disconnect a client that sends nothing within 1.5x it.
                if self.keepalive and time.time() - self.last_seen > self.keepalive * 1.5:
                    raise ConnectionError("keepalive expired")
                if not self.alive:
                    raise ConnectionError("closed by the broker")
                continue
            if not chunk:
                raise ConnectionError("closed")
            self.buf += chunk
            self.last_seen = time.time()
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def read_packet(self):
        head = self.recv_exact(1)[0]
        mult, length = 1, 0
        while True:
            b = self.recv_exact(1)[0]
            length += (b & 127) * mult
            if not (b & 0x80):
                break
            mult *= 128
            if mult > 128 ** 3:
                raise ValueError("malformed remaining length")
        return head >> 4, head & 0x0F, self.recv_exact(length)

    @staticmethod
    def take_str(body, i):
        (n,) = struct.unpack_from("!H", body, i)
        i += 2
        return body[i:i + n].decode("utf-8", "replace"), i + n

    # -- the loop -----------------------------------------------------------
    def run(self):
        # A short read timeout so the loop can notice an expired keepalive; a
        # blocking recv would sit there for ever and never reap anyone.
        self.sock.settimeout(1.0)
        try:
            self.serve()
        except (ConnectionError, OSError, ValueError, struct.error) as e:
            self.b.say("client %s ended: %s" % (self.client_id, e))
        finally:
            self.cleanup()

    def serve(self):
        kind, flags, body = self.read_packet()
        if kind != CONNECT:
            raise ValueError("first packet was %s, not CONNECT" % NAMES.get(kind, kind))

        proto, i = self.take_str(body, 0)
        level = body[i]
        cflags = body[i + 1]
        keepalive = struct.unpack_from("!H", body, i + 2)[0]
        self.keepalive = keepalive
        self.last_seen = time.time()
        i += 4
        self.client_id, i = self.take_str(body, i)
        if cflags & 0x04:                                   # will
            wt, i = self.take_str(body, i)
            (wl,) = struct.unpack_from("!H", body, i)
            i += 2
            wp = body[i:i + wl]
            i += wl
            self.will = (wt, wp, (cflags >> 3) & 3, bool(cflags & 0x20))
        user = pwd = None
        if cflags & 0x80:
            user, i = self.take_str(body, i)
        if cflags & 0x40:
            (pl,) = struct.unpack_from("!H", body, i)
            i += 2
            pwd = body[i:i + pl]
            i += pl

        self.b.say("CONNECT  id=%r proto=%s/%d keepalive=%ds clean=%d user=%r%s"
                   % (self.client_id, proto, level, keepalive, (cflags >> 1) & 1, user,
                      "" if not self.will else "  will=%s%s" % (
                          self.will[0], " [R]" if self.will[3] else "")))
        if pwd is not None:
            self.b.say("         password supplied (%d bytes, not checked - test fixture)"
                       % len(pwd))

        with self.b.lock:
            old = self.b.clients.get(self.client_id)
            self.b.clients[self.client_id] = self
        if old is not None and old is not self:
            # Same client id reconnecting - typically after a reset, before the
            # keepalive on the stale session has expired. Drop the OLD will:
            # publishing "offline" moments after the device said "online" would
            # leave the retained value wrong until the next change.
            self.b.say("         (replacing an existing session with the same id; "
                       "its will is discarded)")
            old.will = None
            old.alive = False
            try:
                old.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                old.sock.close()
            except OSError:
                pass

        self.send(bytes([CONNACK << 4, 2, 0, 0]))

        while self.alive:
            kind, flags, body = self.read_packet()
            if kind == PUBLISH:
                qos = (flags >> 1) & 3
                retain = bool(flags & 1)
                topic, i = self.take_str(body, 0)
                if qos:
                    (pid,) = struct.unpack_from("!H", body, i)
                    i += 2
                payload = body[i:]
                self.b.deliver(topic, payload, qos, retain, self.client_id)
                if qos == 1:
                    self.send(bytes([PUBACK << 4, 2]) + struct.pack("!H", pid))
            elif kind == SUBSCRIBE:
                (pid,) = struct.unpack_from("!H", body, 0)
                i = 2
                granted = []
                while i < len(body):
                    filt, i = self.take_str(body, i)
                    q = body[i] & 3
                    i += 1
                    self.subs[filt] = q
                    granted.append(q)
                    self.b.say("SUB      %s -> %s (qos %d)" % (self.client_id, filt, q))
                self.send(bytes([SUBACK << 4]) + self.varint(2 + len(granted))
                          + struct.pack("!H", pid) + bytes(granted))
                # Retained messages matching a new subscription go out at once.
                with self.b.lock:
                    keep = list(self.b.retained.items())
                for topic, (payload, q) in keep:
                    for filt, sq in self.subs.items():
                        if topic_matches(filt, topic):
                            self.b.say("         -> retained %s to %s" % (topic, self.client_id))
                            self.send_publish(topic, payload, min(q, sq), True)
                            break
            elif kind == UNSUBSCRIBE:
                (pid,) = struct.unpack_from("!H", body, 0)
                i = 2
                while i < len(body):
                    filt, i = self.take_str(body, i)
                    self.subs.pop(filt, None)
                self.send(bytes([UNSUBACK << 4, 2]) + struct.pack("!H", pid))
            elif kind == PINGREQ:
                self.send(bytes([PINGRESP << 4, 0]))
            elif kind == DISCONNECT:
                # A clean disconnect discards the will (MQTT 3.1.1 3.14.4).
                self.b.say("DISCONNECT %s (clean - will discarded)" % self.client_id)
                self.will = None
                self.alive = False
            else:
                self.b.say("ignoring %s" % NAMES.get(kind, kind))

    def cleanup(self):
        with self.b.lock:
            if self.b.clients.get(self.client_id) is self:
                del self.b.clients[self.client_id]
        if self.will:
            wt, wp, wq, wr = self.will
            self.b.say("WILL     %s disconnected ungracefully -> publishing %s" %
                       (self.client_id, wt))
            self.b.deliver(wt, wp, wq, wr, self.client_id + " (will)")
        try:
            self.sock.close()
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--log", default=None, help="append every publish as JSONL")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--seconds", type=float, default=0, help="exit after N seconds")
    args = ap.parse_args()

    b = Broker(args.log, args.quiet)
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.bind, args.port))
    srv.listen(8)
    srv.settimeout(0.5)
    print("mqtt test broker on %s:%d  (fixture only - no TLS, no QoS 2)"
          % (args.bind, args.port), flush=True)

    end = time.time() + args.seconds if args.seconds else None
    try:
        while end is None or time.time() < end:
            try:
                sock, addr = srv.accept()
            except socket.timeout:
                continue
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            b.say("connection from %s:%d" % addr)
            Client(b, sock, addr).start()
    except KeyboardInterrupt:
        pass
    finally:
        srv.close()
        print("\n%d publishes seen" % len(b.publishes), flush=True)


if __name__ == "__main__":
    main()
