// The /ws transport, shared by the control UI (app.js) and the presentation
// terminal (terminal.js).
//
// One implementation on purpose: both pages speak the same §10.2a command set
// and consume the same event stream, and a second copy of the reconnect logic
// is a second place for it to be subtly wrong.  Commands fall back to
// POST /api/cmd whenever the socket is down, so a control never silently does
// nothing.
"use strict";

(function (global) {
  class SwanBus {
    constructor(opts) {
      this.opts = opts || {};
      this.handlers = {};
      this.sock = null;
      this.nextId = 1;
      this.retryMs = 1500;
      this.closed = false;
    }

    // kind is the "e" field: state, go, spin, mode, cue, result.
    on(kind, fn) {
      (this.handlers[kind] = this.handlers[kind] || []).push(fn);
      return this;
    }

    emit(kind, e) {
      const list = this.handlers[kind];
      if (list) list.forEach((fn) => fn(e));
    }

    get connected() {
      return !!this.sock && this.sock.readyState === 1;
    }

    connect() {
      if (this.closed) return;
      const proto = location.protocol === "https:" ? "wss:" : "ws:";
      let sock;
      try {
        sock = new WebSocket(proto + "//" + location.host + "/ws");
      } catch (_) {
        setTimeout(() => this.connect(), this.retryMs);
        return;
      }
      this.sock = sock;
      sock.onopen = () => {
        if (this.opts.onstatus) this.opts.onstatus(true);
      };
      sock.onclose = () => {
        this.sock = null;
        if (this.opts.onstatus) this.opts.onstatus(false);
        if (!this.closed) setTimeout(() => this.connect(), this.retryMs);
      };
      sock.onerror = () => {
        if (sock.readyState !== 3) sock.close();
      };
      sock.onmessage = (m) => {
        let e;
        try {
          e = JSON.parse(m.data);
        } catch (_) {
          return;
        }
        if (e && e.e) this.emit(e.e, e);
      };
    }

    close() {
      this.closed = true;
      if (this.sock) this.sock.close();
    }

    // Returns a promise only for the REST fallback; over the socket the result
    // arrives as an {"e":"result"} event carrying the same id.
    send(cmd, payload) {
      const body = { cmd, id: this.nextId++ };
      if (payload !== undefined) body.payload = payload;
      const text = JSON.stringify(body);
      if (this.connected) {
        this.sock.send(text);
        return Promise.resolve(null);
      }
      return fetch("/api/cmd", { method: "POST", body: text })
        .then((r) => r.json())
        .then((res) => {
          if (res && res.ok === false) this.emit("result", { e: "result", id: body.id, res });
          return res;
        })
        .catch(() => {
          const res = { ok: false, err: "not connected" };
          this.emit("result", { e: "result", id: body.id, res });
          return res;
        });
    }
  }

  global.SwanBus = SwanBus;
})(window);
