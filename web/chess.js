// Flame chess - Phase 7, the show-accuracy pack.  A stretch goal: cut it and
// nothing else in the pack notices.
//
// Typing CHESS at the idle prompt opens a board against a deliberately weak
// engine.  Checkmating it opens the Flame's command menu (24/32/38/77); code
// 77 runs a screen-only incursion effect and hands off to the boot animation.
//
// SCREEN-SIDE ONLY.  No dispatcher command, no fetch, nothing that reaches the
// flaps - the whole module is a picture on a CRT.  It is behind
// SwanTerm.prefs.egg, which defaults off, and it draws nothing until opened.
//
// Where the care went: the MOVE GENERATOR, not the search.  Checkmate
// detection is load-bearing - it is the only thing that opens the menu - and a
// game that permits an illegal move is worse than no game, so legality is
// decided by make-the-move-and-look-at-the-king rather than by anything clever.
// The opponent is ~2 ply with a material evaluation and is supposed to lose.
// SwanChess._selftest() runs perft against five published positions plus a mate
// and a stalemate; run it in a console before touching the generator.
"use strict";

(function (root) {

  const doc = typeof document !== "undefined" ? document : null;

  // =========================================================================
  // Board model
  //
  // 64-entry array, index 0 = a8 through index 63 = h1, so the array reads in
  // the order the board is drawn and a FEN loads with no transform.  Pieces are
  // FEN characters: uppercase white, lowercase black, "" empty.  That is also
  // what gets rendered - see renderBoard for why letters and not figurines.
  // =========================================================================
  const WHITE_PIECES = "PNBRQK";
  const START_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

  const KNIGHT_D = [[1, 2], [2, 1], [2, -1], [1, -2], [-1, -2], [-2, -1], [-2, 1], [-1, 2]];
  const KING_D = [[0, 1], [1, 1], [1, 0], [1, -1], [0, -1], [-1, -1], [-1, 0], [-1, 1]];
  const DIAG_D = [[1, 1], [1, -1], [-1, -1], [-1, 1]];
  const ORTH_D = [[0, 1], [1, 0], [0, -1], [-1, 0]];
  const QUEEN_D = DIAG_D.concat(ORTH_D);

  function colorOf(p) {
    if (!p) return "";
    return WHITE_PIECES.indexOf(p) >= 0 ? "w" : "b";
  }
  function other(side) { return side === "w" ? "b" : "w"; }

  function sqName(i) { return "abcdefgh".charAt(i & 7) + (8 - (i >> 3)); }
  function nameSq(s) {
    const f = "abcdefgh".indexOf(String(s).charAt(0));
    const r = parseInt(String(s).charAt(1), 10);
    if (f < 0 || !(r >= 1 && r <= 8)) return -1;
    return (8 - r) * 8 + f;
  }

  // df counts files to the right, dr counts ranks UP the board - which is one
  // row earlier in the array, hence the subtraction.  Returns -1 off-board, so
  // every generator gets its edge test for free.
  function off(sq, df, dr) {
    const f = (sq & 7) + df;
    const r = (sq >> 3) - dr;
    if (f < 0 || f > 7 || r < 0 || r > 7) return -1;
    return r * 8 + f;
  }

  function fromFen(fen) {
    const p = String(fen).trim().split(/\s+/);
    const b = new Array(64);
    for (let i = 0; i < 64; i++) b[i] = "";
    let at = 0;
    for (let k = 0; k < p[0].length && at < 64; k++) {
      const c = p[0].charAt(k);
      if (c === "/") continue;
      if (c >= "1" && c <= "8") at += c.charCodeAt(0) - 48;
      else b[at++] = c;
    }
    const rights = p[2] || "-";
    return {
      b: b,
      turn: p[1] === "b" ? "b" : "w",
      cast: {
        K: rights.indexOf("K") >= 0, Q: rights.indexOf("Q") >= 0,
        k: rights.indexOf("k") >= 0, q: rights.indexOf("q") >= 0
      },
      ep: p[3] && p[3] !== "-" ? nameSq(p[3]) : -1,
      half: p[4] ? parseInt(p[4], 10) : 0,
      full: p[5] ? parseInt(p[5], 10) : 1
    };
  }

  // =========================================================================
  // Attack detection
  //
  // Scanned OUTWARD from the square rather than by walking every enemy piece:
  // one function answers "is this square attacked", which is all that check,
  // castling and legality need, and it is the same code in all three.
  // =========================================================================
  function attacked(b, sq, by) {
    const up = by === "w" ? 1 : -1;
    // A pawn of `by` attacking sq stands one rank behind sq in ITS direction of
    // travel, so the offset is negated.
    const pw = by === "w" ? "P" : "p";
    let s = off(sq, -1, -up);
    if (s >= 0 && b[s] === pw) return true;
    s = off(sq, 1, -up);
    if (s >= 0 && b[s] === pw) return true;

    const kn = by === "w" ? "N" : "n";
    const kg = by === "w" ? "K" : "k";
    for (let i = 0; i < 8; i++) {
      s = off(sq, KNIGHT_D[i][0], KNIGHT_D[i][1]);
      if (s >= 0 && b[s] === kn) return true;
      s = off(sq, KING_D[i][0], KING_D[i][1]);
      if (s >= 0 && b[s] === kg) return true;
    }

    const diag = by === "w" ? "BQ" : "bq";
    const orth = by === "w" ? "RQ" : "rq";
    for (let i = 0; i < 4; i++) {
      let t = sq;
      for (;;) {
        t = off(t, DIAG_D[i][0], DIAG_D[i][1]);
        if (t < 0) break;
        if (b[t]) { if (diag.indexOf(b[t]) >= 0) return true; break; }
      }
      t = sq;
      for (;;) {
        t = off(t, ORTH_D[i][0], ORTH_D[i][1]);
        if (t < 0) break;
        if (b[t]) { if (orth.indexOf(b[t]) >= 0) return true; break; }
      }
    }
    return false;
  }

  function kingSq(b, side) {
    const k = side === "w" ? "K" : "k";
    for (let i = 0; i < 64; i++) if (b[i] === k) return i;
    return -1;
  }

  function inCheck(st) {
    const ks = kingSq(st.b, st.turn);
    return ks >= 0 && attacked(st.b, ks, other(st.turn));
  }

  // =========================================================================
  // Move generation
  // =========================================================================
  function mk(st, from, to, flag, promo) {
    return {
      from: from, to: to, piece: st.b[from],
      // An en-passant capture takes a pawn that is NOT on the destination.
      cap: flag === "ep" ? (st.turn === "w" ? "p" : "P") : st.b[to],
      flag: flag || "", promo: promo || ""
    };
  }

  function addPawn(list, st, from, to, flag) {
    const r = to >> 3;
    if (r === 0 || r === 7) {
      const set = st.turn === "w" ? "QRBN" : "qrbn";
      for (let i = 0; i < 4; i++) list.push(mk(st, from, to, flag, set.charAt(i)));
    } else {
      list.push(mk(st, from, to, flag, ""));
    }
  }

  function addCastles(st, list) {
    const b = st.b, foe = other(st.turn);
    // Squares between empty, and the king's start, transit and landing squares
    // all unattacked.  The rest of the rights bookkeeping is in make().
    if (st.turn === "w") {
      if (st.cast.K && b[60] === "K" && b[63] === "R" && !b[61] && !b[62] &&
          !attacked(b, 60, foe) && !attacked(b, 61, foe) && !attacked(b, 62, foe)) {
        list.push(mk(st, 60, 62, "castle", ""));
      }
      if (st.cast.Q && b[60] === "K" && b[56] === "R" && !b[59] && !b[58] && !b[57] &&
          !attacked(b, 60, foe) && !attacked(b, 59, foe) && !attacked(b, 58, foe)) {
        list.push(mk(st, 60, 58, "castle", ""));
      }
    } else {
      if (st.cast.k && b[4] === "k" && b[7] === "r" && !b[5] && !b[6] &&
          !attacked(b, 4, foe) && !attacked(b, 5, foe) && !attacked(b, 6, foe)) {
        list.push(mk(st, 4, 6, "castle", ""));
      }
      if (st.cast.q && b[4] === "k" && b[0] === "r" && !b[3] && !b[2] && !b[1] &&
          !attacked(b, 4, foe) && !attacked(b, 3, foe) && !attacked(b, 2, foe)) {
        list.push(mk(st, 4, 2, "castle", ""));
      }
    }
  }

  function pseudo(st) {
    const b = st.b, side = st.turn, list = [];
    const dir = side === "w" ? 1 : -1;
    const home = side === "w" ? 6 : 1;   // array row holding that side's pawn rank
    for (let i = 0; i < 64; i++) {
      const p = b[i];
      if (!p || colorOf(p) !== side) continue;
      const t = p.toUpperCase();
      if (t === "P") {
        const s = off(i, 0, dir);
        if (s >= 0 && !b[s]) {
          addPawn(list, st, i, s, "");
          if ((i >> 3) === home) {
            const s2 = off(i, 0, dir * 2);
            if (s2 >= 0 && !b[s2]) list.push(mk(st, i, s2, "dbl", ""));
          }
        }
        for (let d = -1; d <= 1; d += 2) {
          const c = off(i, d, dir);
          if (c < 0) continue;
          if (b[c]) {
            if (colorOf(b[c]) !== side) addPawn(list, st, i, c, "");
          } else if (c === st.ep) {
            list.push(mk(st, i, c, "ep", ""));
          }
        }
      } else if (t === "N" || t === "K") {
        const D = t === "N" ? KNIGHT_D : KING_D;
        for (let k = 0; k < 8; k++) {
          const s = off(i, D[k][0], D[k][1]);
          if (s < 0) continue;
          if (!b[s] || colorOf(b[s]) !== side) list.push(mk(st, i, s, "", ""));
        }
      } else {
        const D = t === "B" ? DIAG_D : t === "R" ? ORTH_D : QUEEN_D;
        for (let k = 0; k < D.length; k++) {
          let s = i;
          for (;;) {
            s = off(s, D[k][0], D[k][1]);
            if (s < 0) break;
            if (!b[s]) { list.push(mk(st, i, s, "", "")); continue; }
            if (colorOf(b[s]) !== side) list.push(mk(st, i, s, "", ""));
            break;
          }
        }
      }
    }
    addCastles(st, list);
    return list;
  }

  // A new state per move: 64 slots is nothing to copy, and an unmake that has
  // to restore castling rights, the en-passant square and the halfmove clock is
  // the classic place for a generator to go quietly wrong.
  function make(st, m) {
    const b = st.b.slice();
    const cast = { K: st.cast.K, Q: st.cast.Q, k: st.cast.k, q: st.cast.q };
    const p = b[m.from];
    let ep = -1;

    b[m.from] = "";
    b[m.to] = m.promo ? m.promo : p;

    if (m.flag === "ep") {
      // The captured pawn sits on the mover's rank, on the destination file.
      b[(m.from - (m.from & 7)) + (m.to & 7)] = "";
    } else if (m.flag === "dbl") {
      ep = (m.from + m.to) / 2;
    } else if (m.flag === "castle") {
      if (m.to === 62) { b[63] = ""; b[61] = "R"; }
      else if (m.to === 58) { b[56] = ""; b[59] = "R"; }
      else if (m.to === 6) { b[7] = ""; b[5] = "r"; }
      else if (m.to === 2) { b[0] = ""; b[3] = "r"; }
    }

    if (p === "K") { cast.K = false; cast.Q = false; }
    if (p === "k") { cast.k = false; cast.q = false; }
    // A rook leaving OR being captured on its home square kills that right.
    if (m.from === 63 || m.to === 63) cast.K = false;
    if (m.from === 56 || m.to === 56) cast.Q = false;
    if (m.from === 7 || m.to === 7) cast.k = false;
    if (m.from === 0 || m.to === 0) cast.q = false;

    return {
      b: b,
      turn: other(st.turn),
      cast: cast,
      ep: ep,
      half: (p === "P" || p === "p" || m.cap) ? 0 : st.half + 1,
      full: st.full + (st.turn === "b" ? 1 : 0)
    };
  }

  function legal(st) {
    const out = [], side = st.turn, foe = other(side);
    const ms = pseudo(st);
    for (let i = 0; i < ms.length; i++) {
      const n = make(st, ms[i]);
      const ks = kingSq(n.b, side);
      // A position with no king of that colour only occurs in a hand-made test
      // board; treat it as safe rather than crashing.
      if (ks < 0 || !attacked(n.b, ks, foe)) out.push(ms[i]);
    }
    return out;
  }

  function insufficient(b) {
    let minor = 0;
    for (let i = 0; i < 64; i++) {
      const p = b[i];
      if (!p) continue;
      const t = p.toUpperCase();
      if (t === "K") continue;
      if (t === "N" || t === "B") { minor++; continue; }
      return false;                    // a pawn, rook or queen can still mate
    }
    return minor <= 1;
  }

  // The one function the menu hangs off.  "checkmate" here means the side to
  // move is mated, so after the player's move it means the engine is mated.
  function statusOf(st) {
    if (legal(st).length === 0) return inCheck(st) ? "checkmate" : "stalemate";
    if (st.half >= 100) return "draw50";
    if (insufficient(st.b)) return "material";
    return inCheck(st) ? "check" : "play";
  }

  function perft(st, depth) {
    const ms = legal(st);
    if (depth <= 1) return depth <= 0 ? 1 : ms.length;
    let n = 0;
    for (let i = 0; i < ms.length; i++) n += perft(make(st, ms[i]), depth - 1);
    return n;
  }

  // =========================================================================
  // The opponent
  //
  // Negamax, 2 ply, material plus a whisper of centre so it does not shuffle in
  // the opening.  It hangs pieces to any three-move idea, which is the point:
  // the menu is behind a checkmate and the checkmate has to be reachable by
  // somebody who does not play chess.
  // =========================================================================
  const VAL = { P: 100, N: 320, B: 330, R: 500, Q: 900, K: 0 };
  const MATE = 100000;

  // Computed, not tabulated - a hand-typed 64-entry table is a typo waiting to
  // change how it plays for no reason anyone could later explain.
  const CENTRE = new Array(64);
  for (let i = 0; i < 64; i++) {
    const df = Math.abs(3.5 - (i & 7)), dr = Math.abs(3.5 - (i >> 3));
    CENTRE[i] = Math.round((7 - (df + dr)) * 3);
  }

  function evalMat(st) {
    let score = 0;
    for (let i = 0; i < 64; i++) {
      const p = st.b[i];
      if (!p) continue;
      const t = p.toUpperCase();
      let v = VAL[t];
      if (t === "P" || t === "N" || t === "B") v += CENTRE[i];
      score += colorOf(p) === st.turn ? v : -v;
    }
    return score;
  }

  function search(st, depth, alpha, beta, ply) {
    if (depth <= 0) {
      // Only a checked side can be mated, and the attack scan is far cheaper
      // than generating moves at every leaf.  A leaf stalemate is scored as
      // material; it costs strength this engine does not have.
      if (inCheck(st) && legal(st).length === 0) return -MATE + ply;
      return evalMat(st);
    }
    const ms = legal(st);
    if (ms.length === 0) return inCheck(st) ? -MATE + ply : 0;
    let best = -MATE * 2;
    for (let i = 0; i < ms.length; i++) {
      const sc = -search(make(st, ms[i]), depth - 1, -beta, -alpha, ply + 1);
      if (sc > best) best = sc;
      if (best > alpha) alpha = best;
      if (alpha >= beta) break;
    }
    return best;
  }

  function pickMove(st) {
    const ms = legal(st);
    if (!ms.length) return null;
    let best = -MATE * 2;
    let ties = [];
    for (let i = 0; i < ms.length; i++) {
      // Full window per root move: the tie list has to be honest, and 30 extra
      // leaf evaluations are free at this depth.
      const sc = -search(make(st, ms[i]), 1, -MATE * 2, MATE * 2, 1);
      if (sc > best) { best = sc; ties = [ms[i]]; }
      else if (sc === best) ties.push(ms[i]);
    }
    return ties[Math.floor(Math.random() * ties.length)];
  }

  // =========================================================================
  // Self-test.  DOM-free on purpose, so it runs in node, in a browser console
  // and from a page that never opened the board.
  // =========================================================================
  function _selftest() {
    const notes = [];
    let pass = 0, fail = 0;

    function eq(label, got, want) {
      if (got === want) { pass++; notes.push("ok   " + label + " = " + got); }
      else { fail++; notes.push("FAIL " + label + ": got " + got + ", want " + want); }
    }

    // Published perft counts.  Position 2 is Kiwipete (castling, en passant
    // under pins), 3 is the pawn/rook endgame, 4 has promotions, 5 is the
    // castling-rights position.  A generator that passes all of these is not
    // wrong in a way this game could expose.
    const P = [
      ["initial", START_FEN, [20, 400, 8902]],
      ["kiwipete", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", [48, 2039]],
      ["endgame", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", [14, 191, 2812]],
      ["promo", "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", [6, 264]],
      ["rights", "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", [44, 1486]]
    ];
    for (let i = 0; i < P.length; i++) {
      const st = fromFen(P[i][1]);
      for (let d = 0; d < P[i][2].length; d++) {
        eq("perft " + P[i][0] + " d" + (d + 1), perft(st, d + 1), P[i][2][d]);
      }
    }

    eq("initial status", statusOf(fromFen(START_FEN)), "play");

    // Back-rank mate: black king boxed in by its own pawns, Ra8 delivering.
    const mate = fromFen("R5k1/5ppp/8/8/8/8/8/6K1 b - - 0 1");
    eq("back-rank status", statusOf(mate), "checkmate");
    eq("back-rank legal moves", legal(mate).length, 0);
    eq("back-rank in check", inCheck(mate), true);

    // The textbook stalemate: not in check, and nowhere to go.
    const stale = fromFen("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1");
    eq("stalemate status", statusOf(stale), "stalemate");
    eq("stalemate not check", inCheck(stale), false);
    eq("stalemate legal moves", legal(stale).length, 0);

    // Castling through an attacked square is refused; the other side is not.
    const cast = fromFen("4k3/8/8/8/8/8/6r1/R3K2R w KQ - 0 1");
    const cms = legal(cast).filter(function (m) { return m.flag === "castle"; });
    eq("castle count with g-file rook", cms.length, 1);
    eq("castle is queenside", cms.length === 1 ? sqName(cms[0].to) : "-", "c1");

    // En passant, including that the captured pawn leaves the board.
    const epst = fromFen("8/8/8/3pP3/8/8/8/K6k w - d6 0 1");
    const eps = legal(epst).filter(function (m) { return m.flag === "ep"; });
    eq("en passant available", eps.length, 1);
    if (eps.length === 1) {
      const after = make(epst, eps[0]);
      eq("en passant square", sqName(eps[0].to), "d6");
      eq("captured pawn removed", after.b[nameSq("d5")], "");
    }

    // Promotion offers all four pieces and nothing else.
    const pr = fromFen("8/P7/8/8/8/8/8/K6k w - - 0 1");
    const prs = legal(pr).filter(function (m) { return m.from === nameSq("a7"); });
    eq("promotion move count", prs.length, 4);
    eq("promotion pieces", prs.map(function (m) { return m.promo; }).sort().join(""), "BNQR");

    // The engine must produce a move it is allowed to play.
    const st0 = fromFen(START_FEN);
    const mv = pickMove(st0);
    const ok = !!mv && legal(st0).some(function (m) {
      return m.from === mv.from && m.to === mv.to && m.promo === mv.promo;
    });
    eq("engine move is legal", ok, true);

    return { pass: pass, fail: fail, notes: notes };
  }

  // =========================================================================
  // Presentation
  //
  // Everything below needs a DOM.  It is all injected on the first open() and
  // costs the page nothing before that.
  // =========================================================================
  const CSS = [
    "#swan-chess{position:fixed;inset:0;z-index:60;display:none;",
    "  background:var(--p-bg,#04120a);color:var(--p,#43c25e);",
    "  font-family:inherit;overflow:auto;overscroll-behavior:contain;}",
    "#swan-chess.on{display:block;}",
    "#swan-chess *{box-sizing:border-box;}",
    ".sc-wrap{min-height:100%;display:flex;flex-direction:column;",
    "  padding:clamp(8px,1.8vmin,26px);gap:clamp(6px,1.4vmin,18px);}",
    ".sc-head{display:flex;justify-content:space-between;align-items:center;gap:10px;",
    "  flex:0 0 auto;flex-wrap:wrap;}",
    ".sc-title{font-size:clamp(10px,1.6vmin,20px);letter-spacing:.3em;",
    "  color:var(--p-dim,#2f7a3a);text-transform:uppercase;}",
    ".sc-btn{background:none;border:1px solid var(--p-dim,#2f7a3a);color:var(--p-dim,#2f7a3a);",
    "  font:inherit;font-size:clamp(9px,1.3vmin,15px);letter-spacing:.22em;padding:4px 12px;",
    "  cursor:pointer;text-transform:uppercase;}",
    ".sc-btn:hover{color:var(--p-hot,#7CFF9B);border-color:var(--p-hot,#7CFF9B);}",
    ".sc-main{flex:1 1 auto;display:flex;flex-direction:column;align-items:center;",
    "  justify-content:center;gap:clamp(8px,2vmin,26px);min-height:0;}",
    // The board is sized off the SHORT axis of whichever viewport it lands in:
    // a 375x812 phone has height to spare and no width, a 1920x1080 kiosk the
    // reverse, and one vmin rule serves neither well.
    ".sc-board{--sq:calc(var(--sc-sz)/8);width:var(--sc-sz);height:var(--sc-sz);",
    "  display:grid;grid-template-columns:repeat(8,1fr);grid-template-rows:repeat(8,1fr);",
    "  border:1px solid var(--p-dim,#2f7a3a);}",
    "#swan-chess{--sc-sz:min(92vw,52vh,620px);}",
    "@media (min-width:900px){#swan-chess{--sc-sz:min(46vw,72vh,660px);}",
    "  .sc-main{flex-direction:row;align-items:center;justify-content:center;}}",
    // Squares are the phosphor tinting its own background rather than two new
    // colours: the palette is the palette.
    ".sc-sq{position:relative;background:rgba(124,255,155,.03);border:0;padding:0;margin:0;",
    "  font:inherit;font-weight:bold;font-size:calc(var(--sq)*.62);line-height:1;",
    "  color:var(--p-dim,#2f7a3a);cursor:pointer;display:flex;align-items:center;",
    "  justify-content:center;-webkit-tap-highlight-color:transparent;}",
    ".sc-sq.lt{background:rgba(124,255,155,.10);}",
    ".sc-sq.w{color:var(--p-hot,#7CFF9B);}",
    ".sc-sq.b{color:var(--p-mid,#43c25e);opacity:.82;}",
    ".sc-sq.sel{outline:2px solid var(--p-hot,#7CFF9B);outline-offset:-2px;}",
    ".sc-sq.last{background:rgba(124,255,155,.16);}",
    ".sc-sq.chk{outline:2px solid var(--red,#d8584a);outline-offset:-2px;}",
    ".sc-sq[data-f]::after{content:attr(data-f);position:absolute;right:.16em;bottom:.02em;",
    "  font-size:.3em;font-weight:normal;color:var(--p-dim,#2f7a3a);opacity:.75;}",
    ".sc-sq[data-r]::before{content:attr(data-r);position:absolute;left:.16em;top:.02em;",
    "  font-size:.3em;font-weight:normal;color:var(--p-dim,#2f7a3a);opacity:.75;}",
    ".sc-dot{position:absolute;width:26%;height:26%;border-radius:50%;",
    "  background:var(--p-mid,#43c25e);opacity:.5;pointer-events:none;}",
    ".sc-dot.cap{width:86%;height:86%;border-radius:50%;background:none;",
    "  border:.09em solid var(--p-mid,#43c25e);opacity:.6;}",
    ".sc-side{width:min(92vw,var(--sc-sz));display:flex;flex-direction:column;",
    "  gap:clamp(4px,1vmin,12px);}",
    "@media (min-width:900px){.sc-side{width:clamp(220px,22vw,340px);}}",
    ".sc-status{font-size:clamp(12px,1.9vmin,22px);letter-spacing:.2em;",
    "  color:var(--p-hot,#7CFF9B);text-transform:uppercase;min-height:1.4em;}",
    ".sc-status.warn{color:var(--amber,#d8b04a);}",
    ".sc-status.err{color:var(--red,#d8584a);}",
    ".sc-log{flex:0 1 auto;max-height:clamp(70px,22vh,300px);overflow:auto;",
    "  font-size:clamp(10px,1.4vmin,16px);line-height:1.5;color:var(--p-dim,#2f7a3a);",
    "  border:1px solid rgba(124,255,155,.12);padding:6px 8px;white-space:pre-wrap;}",
    ".sc-row{display:flex;gap:6px;flex-wrap:wrap;}",
    ".sc-promo{display:none;gap:6px;align-items:center;font-size:clamp(10px,1.4vmin,16px);",
    "  letter-spacing:.2em;color:var(--amber,#d8b04a);}",
    ".sc-promo.on{display:flex;}",
    // The Flame menu takes the whole screen: it is a different machine talking.
    ".sc-menu{display:none;position:absolute;inset:0;background:var(--p-bg,#04120a);",
    "  padding:clamp(14px,4vmin,60px);flex-direction:column;gap:clamp(6px,1.4vmin,16px);",
    "  overflow:auto;}",
    ".sc-menu.on{display:flex;}",
    ".sc-menu .sc-btn{align-self:flex-start;}",
    ".sc-mtitle{font-size:clamp(13px,2.4vmin,30px);letter-spacing:.3em;",
    "  color:var(--p-hot,#7CFF9B);text-transform:uppercase;}",
    ".sc-mopt{display:block;width:100%;text-align:left;background:none;border:0;padding:.25em 0;",
    "  font:inherit;font-size:clamp(12px,2.1vmin,26px);letter-spacing:.16em;",
    "  color:var(--p,#43c25e);cursor:pointer;text-transform:uppercase;}",
    ".sc-mopt:hover{color:var(--p-hot,#7CFF9B);}",
    ".sc-mcode{color:var(--p-dim,#2f7a3a);margin-right:1em;}",
    ".sc-out{margin-top:.6em;font-size:clamp(10px,1.6vmin,19px);line-height:1.7;",
    "  color:var(--p-dim,#2f7a3a);white-space:pre-wrap;min-height:6em;}",
    ".sc-entry{font-size:clamp(13px,2.4vmin,30px);letter-spacing:.22em;",
    "  color:var(--p-hot,#7CFF9B);margin-top:auto;}",
    ".sc-fx{display:none;position:fixed;inset:0;z-index:70;background:#000;}",
    ".sc-fx.on{display:block;}",
    ".sc-fx canvas{width:100%;height:100%;image-rendering:pixelated;}",
    ".sc-flash{position:fixed;inset:0;z-index:71;background:var(--p-hot,#7CFF9B);",
    "  opacity:0;display:none;pointer-events:none;transition:opacity 260ms linear;}"
  ].join("\n");

  const MENU = [
    ["24", "PALLET DROP", [
      "REQUEST QUEUED WITH DHARMA LOGISTICS.",
      "MANIFEST: RATIONS, FILM STOCK, BEER.",
      "THE DROP IS AUTOMATED. NO FURTHER ACTION REQUIRED."
    ]],
    ["32", "STATION UPLINK", [
      "STATION 3 - THE SWAN ......... CARRIER PRESENT",
      "STATION 4 - THE FLAME ........ THIS TERMINAL",
      "STATION 5 - THE PEARL ........ OBSERVING",
      "ALL STATIONS NOMINAL. NOTHING FURTHER TO REPORT."
    ]],
    ["38", "MAINLAND COMMUNICATION", [
      "ROUTING VIA SUBMARINE RELAY ...",
      "THE RELAY DID NOT ANSWER. THIS IS EXPECTED.",
      "YOUR MESSAGE IS HELD FOR THE NEXT WINDOW."
    ]],
    ["77", "HOSTILE INCURSION", [
      "CODE 77 ACCEPTED.",
      "SEALING THE STATION.",
      "PURGING SYSTEM ..."
    ]]
  ];

  let el = null;          // the built DOM, or null before the first open
  let opened = false;
  let menuOn = false;
  let game = null;        // { st, sel, legal, last, over, hist, log, promo }
  let entry = "";
  let timers = [];

  function reduced() {
    return !!(root.matchMedia && root.matchMedia("(prefers-reduced-motion: reduce)").matches);
  }
  function term() { return root.SwanTerm || null; }
  // CHESS BELONGS TO THE FLAME (2026-08-25).  It used to hang off the CHAT
  // toggle, which put two unrelated features behind one switch and let a Swan
  // prompt answer a Flame command.  It is the Flame station's now, and only
  // reachable from there - in both content modes, so selecting FLAME on the
  // strip means the same thing whether or not the station screen is up.
  function eggOn() {
    const t = term();
    return !!(t && t.station && t.station() === "flame");
  }

  // In protocol mode the station screen owns the keyboard and runs CHESS
  // itself, so this sniffer stands down rather than racing it.  Two handlers
  // reading the same keystroke is the shape of the double-execute defect.
  function protocolOwnsKeys() {
    const p = root.SwanProtocol;
    return !!(p && p.isOn && p.isOn());
  }
  // The host's kinds are "key" and "go" (terminal.js clickSound); anything else
  // falls through to the light click, so an unknown kind is harmless.
  function click(kind) {
    const t = term();
    if (t && t.clickSound) { try { t.clickSound(kind); } catch (e) { /* no audio */ } }
  }
  // A dramatic pause collapses to nothing under prefers-reduced-motion; the end
  // state still arrives, it just arrives at once.
  function later(fn, ms) { return timer(fn, reduced() ? 0 : ms); }
  function timer(fn, ms) {
    const id = root.setTimeout(function () { fn(); }, ms);
    timers.push(id);
    return id;
  }
  function clearTimers() {
    for (let i = 0; i < timers.length; i++) root.clearTimeout(timers[i]);
    timers = [];
  }

  function build() {
    const style = doc.createElement("style");
    style.id = "swan-chess-css";
    style.textContent = CSS;
    doc.head.appendChild(style);

    const box = doc.createElement("div");
    box.id = "swan-chess";
    box.setAttribute("role", "dialog");
    box.setAttribute("aria-label", "Flame chess");

    const wrap = doc.createElement("div");
    wrap.className = "sc-wrap";

    const head = doc.createElement("div");
    head.className = "sc-head";
    const title = doc.createElement("span");
    title.className = "sc-title";
    title.textContent = "DHARMA INITIATIVE · STATION 4 · THE FLAME";
    const close = doc.createElement("button");
    close.className = "sc-btn";
    close.type = "button";
    close.textContent = "CLOSE · ESC";
    close.onclick = function () { click("key"); api.close(); };
    head.appendChild(title);
    head.appendChild(close);

    const main = doc.createElement("div");
    main.className = "sc-main";

    const board = doc.createElement("div");
    board.className = "sc-board";
    const squares = [];
    for (let i = 0; i < 64; i++) {
      const sq = doc.createElement("button");
      sq.type = "button";
      sq.className = "sc-sq";
      sq.dataset.i = String(i);
      // Coordinates on the edge squares only - a full frame of labels costs two
      // more grid tracks and the board is the thing worth the pixels.
      if ((i >> 3) === 7) sq.dataset.f = "abcdefgh".charAt(i & 7);
      if ((i & 7) === 0) sq.dataset.r = String(8 - (i >> 3));
      board.appendChild(sq);
      squares.push(sq);
    }
    board.addEventListener("click", onSquare);

    const side = doc.createElement("div");
    side.className = "sc-side";
    const status = doc.createElement("div");
    status.className = "sc-status";
    const promo = doc.createElement("div");
    promo.className = "sc-promo";
    const log = doc.createElement("div");
    log.className = "sc-log";
    const row = doc.createElement("div");
    row.className = "sc-row";
    const bNew = doc.createElement("button");
    bNew.className = "sc-btn";
    bNew.type = "button";
    bNew.textContent = "NEW GAME";
    bNew.onclick = function () { click("key"); newGame(); };
    const bUndo = doc.createElement("button");
    bUndo.className = "sc-btn";
    bUndo.type = "button";
    bUndo.textContent = "TAKE BACK";
    bUndo.onclick = function () { click("key"); undo(); };
    row.appendChild(bNew);
    row.appendChild(bUndo);
    side.appendChild(status);
    side.appendChild(promo);
    side.appendChild(log);
    side.appendChild(row);

    main.appendChild(board);
    main.appendChild(side);
    wrap.appendChild(head);
    wrap.appendChild(main);

    const menu = doc.createElement("div");
    menu.className = "sc-menu";
    const mtitle = doc.createElement("div");
    mtitle.className = "sc-mtitle";
    mtitle.textContent = "THE FLAME · COMMAND MENU";
    menu.appendChild(mtitle);
    const mhint = doc.createElement("div");
    mhint.className = "sc-title";
    mhint.textContent = "ENTER A CODE";
    menu.appendChild(mhint);
    for (let i = 0; i < MENU.length; i++) {
      const opt = doc.createElement("button");
      opt.type = "button";
      opt.className = "sc-mopt";
      const code = doc.createElement("span");
      code.className = "sc-mcode";
      code.textContent = MENU[i][0];
      opt.appendChild(code);
      opt.appendChild(doc.createTextNode(MENU[i][1]));
      opt.dataset.code = MENU[i][0];
      opt.onclick = function (ev) { runCode(ev.currentTarget.dataset.code); };
      menu.appendChild(opt);
    }
    const out = doc.createElement("div");
    out.className = "sc-out";
    menu.appendChild(out);
    const ent = doc.createElement("div");
    ent.className = "sc-entry";
    menu.appendChild(ent);
    const mclose = doc.createElement("button");
    mclose.className = "sc-btn";
    mclose.type = "button";
    mclose.textContent = "CLOSE · ESC";
    mclose.onclick = function () { click("key"); api.close(); };
    menu.appendChild(mclose);

    const fx = doc.createElement("div");
    fx.className = "sc-fx";
    const canvas = doc.createElement("canvas");
    fx.appendChild(canvas);
    const flash = doc.createElement("div");
    flash.className = "sc-flash";

    box.appendChild(wrap);
    box.appendChild(menu);
    box.appendChild(fx);
    box.appendChild(flash);
    doc.body.appendChild(box);

    el = {
      box: box, squares: squares, status: status, log: log, promo: promo,
      menu: menu, out: out, entry: ent, fx: fx, canvas: canvas, flash: flash
    };
  }

  // -------------------------------------------------------------------------
  // Game flow.  The player is white and moves first; the engine is black.
  // -------------------------------------------------------------------------
  function newGame() {
    game = {
      st: fromFen(START_FEN), sel: -1, dests: [], last: null,
      over: false, hist: [], log: [], promo: null, thinking: false
    };
    menuOn = false;
    el.menu.classList.remove("on");
    el.out.textContent = "";
    entry = "";
    render();
    setStatus("YOUR MOVE - WHITE", "");
  }

  function setStatus(text, cls) {
    el.status.textContent = text;
    el.status.className = "sc-status" + (cls ? " " + cls : "");
  }

  function render() {
    const b = game.st.b;
    const chk = inCheck(game.st) ? kingSq(b, game.st.turn) : -1;
    for (let i = 0; i < 64; i++) {
      const sq = el.squares[i];
      const p = b[i];
      let cls = "sc-sq";
      if ((((i >> 3) + (i & 7)) & 1) === 0) cls += " lt";
      if (p) cls += colorOf(p) === "w" ? " w" : " b";
      if (i === game.sel) cls += " sel";
      if (game.last && (i === game.last.from || i === game.last.to)) cls += " last";
      if (i === chk) cls += " chk";
      sq.className = cls;
      // textContent also drops last frame's move dot, which is a child element.
      sq.textContent = p || "";
      sq.setAttribute("aria-label", sqName(i) + (p ? " " + pieceName(p) : " empty"));
    }
    // By destination, not by move: the four promotions share one square and
    // would otherwise stack four identical markers on it.
    const marked = {};
    for (let k = 0; k < game.dests.length; k++) {
      const m = game.dests[k];
      if (marked[m.to]) continue;
      marked[m.to] = true;
      const dot = doc.createElement("i");
      dot.className = "sc-dot" + (m.cap ? " cap" : "");
      el.squares[m.to].appendChild(dot);
    }
    // Pairs, one full move per line, so a long game stays readable in a column
    // narrower than the board.
    const lines = [];
    for (let i = 0; i < game.log.length; i += 2) {
      lines.push((i / 2 + 1) + ". " + game.log[i] + (game.log[i + 1] ? "  " + game.log[i + 1] : ""));
    }
    el.log.textContent = lines.join("\n");
    el.log.scrollTop = el.log.scrollHeight;
  }

  function pieceName(p) {
    const n = { P: "pawn", N: "knight", B: "bishop", R: "rook", Q: "queen", K: "king" };
    return (colorOf(p) === "w" ? "white " : "black ") + n[p.toUpperCase()];
  }

  // Long algebraic, not SAN.  SAN needs disambiguation rules that are a whole
  // second correctness surface for a move list nobody replays.
  function moveText(st, m, after) {
    if (m.flag === "castle") return (m.to === 62 || m.to === 6) ? "O-O" : "O-O-O";
    const t = m.piece.toUpperCase();
    let s = (t === "P" ? "" : t) + sqName(m.from) + (m.cap ? "x" : "-") + sqName(m.to);
    if (m.promo) s += "=" + m.promo.toUpperCase();
    const stt = statusOf(after);
    if (stt === "checkmate") s += "#";
    else if (stt === "check") s += "+";
    return s;
  }

  function onSquare(ev) {
    const t = ev.target.closest ? ev.target.closest(".sc-sq") : null;
    if (!t || !game || game.over || game.thinking || game.promo) return;
    if (game.st.turn !== "w") return;
    const i = parseInt(t.dataset.i, 10);

    for (let k = 0; k < game.dests.length; k++) {
      if (game.dests[k].to === i) {
        const cands = game.dests.filter(function (m) { return m.to === i; });
        if (cands.length > 1 && cands[0].promo) { askPromo(cands); return; }
        click("key");
        applyPlayer(cands[0]);
        return;
      }
    }
    const p = game.st.b[i];
    if (p && colorOf(p) === "w") {
      game.sel = i;
      game.dests = legal(game.st).filter(function (m) { return m.from === i; });
      click("key");
    } else {
      game.sel = -1;
      game.dests = [];
    }
    render();
  }

  function askPromo(cands) {
    game.promo = cands;
    el.promo.textContent = "";
    el.promo.classList.add("on");
    const label = doc.createElement("span");
    label.textContent = "PROMOTE TO";
    el.promo.appendChild(label);
    for (let i = 0; i < cands.length; i++) {
      const b = doc.createElement("button");
      b.className = "sc-btn";
      b.type = "button";
      b.textContent = cands[i].promo.toUpperCase();
      b.dataset.k = String(i);
      b.onclick = function (ev) {
        const m = game.promo[parseInt(ev.currentTarget.dataset.k, 10)];
        game.promo = null;
        el.promo.classList.remove("on");
        el.promo.textContent = "";
        click("key");
        applyPlayer(m);
      };
      el.promo.appendChild(b);
    }
    setStatus("CHOOSE A PIECE", "warn");
  }

  function applyPlayer(m) {
    game.hist.push({ st: game.st, log: game.log.slice(), last: game.last });
    const after = make(game.st, m);
    game.log.push(moveText(game.st, m, after));
    game.st = after;
    game.last = m;
    game.sel = -1;
    game.dests = [];
    render();
    if (finished()) return;
    setStatus("THE FLAME IS THINKING", "");
    game.thinking = true;
    later(engineMove, 280);
  }

  function engineMove() {
    game.thinking = false;
    const m = pickMove(game.st);
    if (!m) { finished(); return; }
    const after = make(game.st, m);
    game.log.push(moveText(game.st, m, after));
    game.st = after;
    game.last = m;
    render();
    click("key");
    if (finished()) return;
    setStatus(inCheck(game.st) ? "CHECK - YOUR MOVE" : "YOUR MOVE - WHITE",
              inCheck(game.st) ? "warn" : "");
  }

  // Returns true when the game is over, and is the ONLY place the menu opens.
  function finished() {
    const s = statusOf(game.st);
    if (s === "checkmate") {
      game.over = true;
      if (game.st.turn === "b") {
        setStatus("CHECKMATE - THE FLAME CONCEDES", "");
        click("go");
        later(openMenu, 1400);
      } else {
        setStatus("CHECKMATE - YOU LOSE", "err");
      }
      return true;
    }
    if (s === "stalemate") { game.over = true; setStatus("STALEMATE - DRAWN", "warn"); return true; }
    if (s === "draw50") { game.over = true; setStatus("DRAWN - FIFTY MOVE RULE", "warn"); return true; }
    if (s === "material") { game.over = true; setStatus("DRAWN - INSUFFICIENT MATERIAL", "warn"); return true; }
    return false;
  }

  function undo() {
    if (!game || game.thinking || !game.hist.length) return;
    const h = game.hist.pop();
    game.st = h.st;
    game.log = h.log;
    game.last = h.last;
    game.sel = -1;
    game.dests = [];
    game.over = false;
    game.promo = null;
    el.promo.classList.remove("on");
    render();
    setStatus("YOUR MOVE - WHITE", "");
  }

  // -------------------------------------------------------------------------
  // The Flame menu
  // -------------------------------------------------------------------------
  function openMenu() {
    menuOn = true;
    entry = "";
    el.out.textContent = "";
    el.menu.classList.add("on");
    drawEntry();
  }

  function drawEntry() {
    el.entry.textContent = ">: " + entry + (entry.length < 2 ? "_" : "");
  }

  function menuKey(k) {
    if (k === "Backspace") { entry = entry.slice(0, -1); drawEntry(); return; }
    if (k === "Enter") { if (entry.length) runCode(entry); return; }
    if (entry.length >= 2) entry = "";
    entry += k;
    click("key");
    drawEntry();
    if (entry.length === 2) later(function () { runCode(entry); }, 220);
  }

  function runCode(code) {
    entry = "";
    drawEntry();
    let found = null;
    for (let i = 0; i < MENU.length; i++) if (MENU[i][0] === code) found = MENU[i];
    if (!found) {
      el.out.textContent = "COMMAND " + code + " NOT RECOGNISED.";
      click("key");
      return;
    }
    click(code === "77" ? "go" : "key");
    typeOut(found[2], code === "77" ? incursion : null);
  }

  // One line at a time, because a terminal that answers instantly is a web page.
  function typeOut(lines, done) {
    el.out.textContent = "";
    if (reduced()) {
      el.out.textContent = lines.join("\n");
      if (done) later(done, 0);
      return;
    }
    let i = 0;
    const step = function () {
      el.out.textContent += (i ? "\n" : "") + lines[i];
      i++;
      if (i < lines.length) later(step, 420);
      else if (done) later(done, 700);
    };
    step();
  }

  // -------------------------------------------------------------------------
  // Code 77.  Static, a flash, and out - screen-only, like everything else
  // here.  The handoff is to SwanBoot if that module is loaded; if it is not,
  // the screen simply clears, because a missing sibling module must not leave
  // the terminal covered by a dead overlay.
  // -------------------------------------------------------------------------
  function incursion() {
    if (reduced()) { handoff(); return; }
    el.fx.classList.add("on");
    const cv = el.canvas;
    const w = 160, h = 120;
    cv.width = w;
    cv.height = h;
    const ctx = cv.getContext ? cv.getContext("2d") : null;
    if (!ctx) { handoff(); return; }
    const img = ctx.createImageData(w, h);
    let frames = 0;
    // setTimeout, not requestAnimationFrame: rAF does not run in a background
    // tab, and this sequence ENDS in a handoff to the boot animation - an
    // effect that can stall forever leaves the terminal behind a dead overlay.
    // A counted number of frames also means it cannot outlive its own budget.
    const step = function () {
      const d = img.data;
      for (let i = 0; i < d.length; i += 4) {
        const v = (Math.random() * 255) | 0;
        d[i] = (v * 0.34) | 0;
        d[i + 1] = v;
        d[i + 2] = (v * 0.44) | 0;
        d[i + 3] = 255;
      }
      ctx.putImageData(img, 0, 0);
      if (++frames < 28) timer(step, 40);
      else flashOut();
    };
    click("go");
    step();
  }

  function flashOut() {
    el.fx.classList.remove("on");
    el.flash.style.display = "block";
    el.flash.style.opacity = "1";
    later(function () {
      el.flash.style.opacity = "0";
      later(function () {
        el.flash.style.display = "none";
        handoff();
      }, 300);
    }, 90);
  }

  function handoff() {
    api.close();
    if (root.SwanBoot && typeof root.SwanBoot.play === "function") {
      try { root.SwanBoot.play({ skipable: true }); } catch (e) { /* boot module said no */ }
    }
  }

  // -------------------------------------------------------------------------
  // Keys
  // -------------------------------------------------------------------------
  // The trigger listens in the CAPTURE phase because terminal.js binds keydown
  // on window in the BUBBLE phase, where "c" is the CANCEL shortcut - so a
  // player typing CHESS would publish countdown.cancel before the second letter
  // landed.  An easter egg must not be able to end somebody's countdown.
  const WORD = "CHESS";
  let typed = "";
  let typedAt = 0;

  // The pack's own definition when protocol.js is loaded, this page's otherwise.
  function atIdlePrompt() {
    const p = root.SwanProtocol;
    if (p && p.idle) return !!p.idle();
    const t = term();
    return !t || !t.phase || t.phase() === "idle";
  }

  // Another egg already owns the screen: stay off its keyboard entirely.
  function eggBusy() {
    const p = root.SwanPearl, c = root.SwanChat;
    if (p && p.isOpen && p.isOpen()) return true;
    if (c && c.isOpen && c.isOpen()) return true;
    return false;
  }

  function onKey(ev) {
    if (ev.metaKey || ev.ctrlKey || ev.altKey) return;

    if (opened) {
      if (ev.key === "Tab") return;                 // let focus move inside the board
      ev.stopImmediatePropagation();                // the host page must not see these
      if (ev.key === "Escape") { ev.preventDefault(); api.close(); return; }
      if (menuOn) {
        if (ev.key >= "0" && ev.key <= "9") { ev.preventDefault(); menuKey(ev.key); }
        else if (ev.key === "Backspace" || ev.key === "Enter") { ev.preventDefault(); menuKey(ev.key); }
      }
      return;                                       // Enter/Space still activate a focused button
    }

    if (ev.repeat || !eggOn() || protocolOwnsKeys() || eggBusy()) return;
    if (!atIdlePrompt()) { typed = ""; return; }
    if (!ev.key || ev.key.length !== 1) return;
    const c = ev.key.toUpperCase();
    if (c < "A" || c > "Z") { typed = ""; return; }

    const now = Date.now();
    if (now - typedAt > 2000) typed = "";
    typedAt = now;
    const next = typed + c;
    typed = WORD.indexOf(next) === 0 ? next : (WORD.indexOf(c) === 0 ? c : "");

    // Exactly ONE key is swallowed - the leading C - because it is the only
    // letter terminal.js acts on.  H, E and S are let through so chat.js's mash
    // detector and bootanim's skip still count them; the cost is that a lone C
    // no longer cancels from the keyboard while the egg is on, which the CANCEL
    // key on the pad covers.
    if (typed === "C") { ev.preventDefault(); ev.stopImmediatePropagation(); }
    if (typed === WORD) { typed = ""; api.open(); }
  }

  // -------------------------------------------------------------------------
  const api = {
    open: function () {
      if (!doc || opened) return false;
      if (!eggOn()) return false;      // the toggle is the gate, always
      if (!el) build();
      opened = true;
      el.box.classList.add("on");
      newGame();
      return true;
    },
    close: function () {
      if (!opened) return;
      opened = false;
      menuOn = false;
      clearTimers();
      el.box.classList.remove("on");
      el.menu.classList.remove("on");
      el.fx.classList.remove("on");
      el.flash.style.display = "none";
      el.promo.classList.remove("on");
    },
    isOpen: function () { return opened; },
    _selftest: _selftest
  };

  if (doc) root.addEventListener("keydown", onKey, true);
  root.SwanChess = api;

})(typeof window !== "undefined" ? window : globalThis);
