// The Flame chess engine's RULES, against the real chess.js.
//
// A game that permits an illegal move is worse than no game, and checkmate
// detection is the only thing that opens the Flame's menu, so the move generator
// is what this guards: perft against published counts (below), the module's own
// self-test, castling, en passant, promotion, game-end detection, and bookkeeping
// invariants over random playouts.  The engine's STRENGTH is deliberately not
// asserted here - see the report that came with this file: it is a design
// question about who the opponent is meant to be, not a defect.
"use strict";
const { loadChess } = require("./chess_world");
const { ctx, eng, api } = loadChess({ console });
// deterministic engine randomness inside the sandbox
require("vm").runInContext(
  "(function(){ var s = 987654321; Math.random = function () { s = (Math.imul(s, 1664525) + 1013904223) >>> 0; return s / 4294967296; }; })();",
  ctx);

let fails = 0, passes = 0;
function ok(cond, label, extra) {
  if (cond) { passes++; } else { fails++; console.log("FAIL " + label + (extra ? "  " + extra : "")); }
}
const E = (fen) => eng.fromFen(fen);
// promo letters are the real board characters (upper for white, lower for black);
// normalise to lower for UCI-style comparison and check piece case via the board.
const uci = (m) => eng.sqName(m.from) + eng.sqName(m.to) + (m.promo || "").toLowerCase();
const movesOf = (st) => eng.legal(st).map(uci);
function play(st, u) {
  const m = eng.legal(st).find((x) => uci(x) === u);
  if (!m) throw new Error("illegal in test: " + u);
  return eng.make(st, m);
}
const castleMoves = (st) => eng.legal(st).filter((m) => m.flag === "castle").map(uci).sort();

// ---------------------------------------------------------------- castling
console.log("== castling");
ok(castleMoves(E("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1")).join() === "e1c1,e1g1", "white both sides");
ok(castleMoves(E("r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 0 1")).join() === "e8c8,e8g8", "black both sides");
ok(castleMoves(E("5r1k/8/8/8/8/8/8/R3K2R w KQ - 0 1")).join() === "e1c1", "f1 attacked -> no O-O, O-O-O ok (through check)");
ok(castleMoves(E("4r2k/8/8/8/8/8/8/R3K2R w KQ - 0 1")).length === 0, "king in check -> no castling (out of check)");
ok(castleMoves(E("6rk/8/8/8/8/8/8/R3K2R w KQ - 0 1")).join() === "e1c1", "g1 attacked -> no O-O (into check)");
ok(castleMoves(E("2r4k/8/8/8/8/8/8/R3K2R w KQ - 0 1")).join() === "e1g1", "c1 attacked -> no O-O-O (into check)");
ok(castleMoves(E("1r5k/8/8/8/8/8/8/R3K2R w KQ - 0 1")).join() === "e1c1,e1g1", "b1 attacked does NOT forbid O-O-O");
ok(castleMoves(E("r3k2r/8/8/8/8/8/8/RN2K2R w KQkq - 0 1")).join() === "e1g1", "knight on b1 blocks O-O-O");
ok(castleMoves(E("r3k2r/8/8/8/8/8/8/4K2R w KQkq - 0 1")).join() === "e1g1", "Q right set but rook absent -> no O-O-O");
// rights lost
{
  let s = E("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
  s = play(s, "a1b1"); s = play(s, "a8b8"); s = play(s, "b1a1"); s = play(s, "b8a8");
  ok(castleMoves(s).join() === "e1g1", "rook went away and back: Q right stays lost", castleMoves(s).join());
  s = play(s, "e1g1");
  ok(s.b[eng.nameSq("f1")] === "R" && s.b[eng.nameSq("g1")] === "K" && s.b[eng.nameSq("h1")] === "", "O-O relocates rook h1->f1");
  let t = E("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
  t = play(t, "e1c1");
  ok(t.b[eng.nameSq("d1")] === "R" && t.b[eng.nameSq("c1")] === "K" && t.b[eng.nameSq("a1")] === "", "O-O-O relocates rook a1->d1");
  t = play(t, "e8g8");
  ok(t.b[eng.nameSq("f8")] === "r" && t.b[eng.nameSq("g8")] === "k" && t.b[eng.nameSq("h8")] === "", "black O-O relocates h8->f8");
  let u = E("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
  u = play(u, "e1e2"); u = play(u, "e8e7"); u = play(u, "e2e1"); u = play(u, "e7e8");
  ok(castleMoves(u).length === 0, "king moved and returned: no castling for either side");
  let v = E("r3k2r/8/8/8/8/8/6b1/R3K2R b KQkq - 0 1");
  v = play(v, "g2h1");
  ok(v.cast.K === false && v.cast.Q === true, "rook CAPTURED on h1 kills K right only", JSON.stringify(v.cast));
  ok(castleMoves(E("r3k2r/8/8/8/8/8/6b1/R3K2R w KQkq - 0 1")).join() === "e1c1", "g2 bishop attacks f1/h1: (white to move) f1 attacked -> no O-O");
}

// ----------------------------------------------------------- en passant
console.log("== en passant");
ok(movesOf(E("8/8/8/8/k2Pp2Q/8/8/3K4 b - d3 0 1")).indexOf("e4d3") < 0, "ep with horizontal pin (rank discovered check) is illegal");
ok(movesOf(E("8/8/8/2k5/3Pp3/8/8/4K3 b - d3 0 1")).indexOf("e4d3") >= 0, "ep capturing the CHECKING pawn is legal");
{
  let s = E(eng.START_FEN);
  s = play(s, "e2e4"); ok(eng.sqName(s.ep) === "e3", "double push sets ep square e3");
  s = play(s, "a7a6"); ok(s.ep === -1, "ep square cleared by next move");
  let t = E("rnbqkbnr/ppp1pppp/8/8/3pP3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 3");
  ok(movesOf(t).indexOf("d4e3") >= 0, "black ep capture available");
  const after = play(t, "d4e3");
  ok(after.b[eng.nameSq("e4")] === "" && after.b[eng.nameSq("e3")] === "p", "ep removes the e4 pawn and lands on e3");
  ok(after.half === 0, "ep resets the halfmove clock (capture)");
}

// ------------------------------------------------------------- promotion
console.log("== promotion");
{
  const w = movesOf(E("1n6/P7/8/8/8/8/8/K6k w - - 0 1")).filter((u) => u.startsWith("a7"));
  ok(w.slice().sort().join() === ["a7a8b", "a7a8n", "a7a8q", "a7a8r", "a7b8b", "a7b8n", "a7b8q", "a7b8r"].join(), "white a7: 4 push + 4 capture promos", w.join());
  const b = movesOf(E("K6k/8/8/8/8/8/p7/1N6 b - - 0 1")).filter((u) => u.startsWith("a2"));
  ok(b.slice().sort().join() === ["a2a1b", "a2a1n", "a2a1q", "a2a1r", "a2b1b", "a2b1n", "a2b1q", "a2b1r"].join(), "black a2: promos are lowercase", b.join());
  const s = play(E("1n6/P7/8/8/8/8/8/K6k w - - 0 1"), "a7b8r");
  ok(s.b[eng.nameSq("b8")] === "R" && s.b[eng.nameSq("a7")] === "", "promotion capture puts the chosen piece (rook) on b8");
  const t = play(E("K6k/8/8/8/8/8/p7/1N6 b - - 0 1"), "a2a1n");
  ok(t.b[eng.nameSq("a1")] === "n", "black underpromotion to knight is a black knight");
  // promotion capturing a rook on its home square kills the right
  const u = play(E("r3k2r/1P6/8/8/8/8/8/4K3 w kq - 0 1"), "b7a8q");
  ok(u.cast.q === false && u.cast.k === true, "capturing the a8 rook by promotion kills q right only");
  // moveText for promotion/castle (UI strings)
  const pre = E("1n6/P7/8/8/8/8/8/K6k w - - 0 1");
  const mv = eng.legal(pre).find((m) => uci(m) === "a7b8q");
  ok(eng.moveText(pre, mv, eng.make(pre, mv)) === "a7xb8=Q", "moveText promotion capture", eng.moveText(pre, mv, eng.make(pre, mv)));
}

// ------------------------------------------------------- game-end states
console.log("== game end detection");
{
  let s = E(eng.START_FEN);
  for (const u of ["f2f3", "e7e5", "g2g4", "d8h4"]) s = play(s, u);
  ok(eng.statusOf(s) === "checkmate" && s.turn === "w", "fool's mate -> checkmate, white to move");
  const mv = null;
  ok(eng.statusOf(E("6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1")) === "play", "back rank pre-mate is just play");
  // 50-move rule
  ok(eng.statusOf(E("4k3/8/8/8/8/8/8/R3K3 w - - 100 80")) === "draw50", "halfmove 100 -> draw50");
  ok(eng.statusOf(E("4k3/8/8/8/8/8/8/R3K3 w - - 99 80")) === "play", "halfmove 99 -> play");
  const n = play(E("4k3/8/8/8/8/8/8/R3K3 w - - 99 80"), "a1a2");
  ok(eng.statusOf(n) === "draw50", "a quiet move at 99 reaches 100 -> draw50");
  ok(eng.statusOf(E("R5k1/5ppp/8/8/8/8/8/6K1 b - - 100 90")) === "checkmate", "mate on the 100th halfmove is MATE, not draw (FIDE)");
  // insufficient material
  const mat = (fen) => eng.statusOf(E(fen));
  ok(mat("4k3/8/8/8/8/8/8/4K3 w - - 0 1") === "material", "K v K");
  ok(mat("4k3/8/8/8/8/8/8/3BK3 w - - 0 1") === "material", "K+B v K");
  ok(mat("4k3/8/8/8/8/8/8/3NK3 w - - 0 1") === "material", "K+N v K");
  ok(mat("4k3/8/8/8/8/8/8/2NNK3 w - - 0 1") === "play", "K+N+N v K is not flagged (FIDE agrees: mate possible)");
  ok(mat("4k3/8/8/8/8/8/P7/4K3 w - - 0 1") === "play", "K+P v K is play");
  const kbkb = mat("4kb2/8/8/8/8/8/8/3BK3 w - - 0 1");
  console.log("   note: K+B v K+B (same colour bishops) status = " + kbkb + " (FIDE: automatic draw)");
  // threefold repetition: not implemented - shuffle knights 3x
  let r = E(eng.START_FEN), seen = 0;
  const shuffle = ["g1f3", "g8f6", "f3g1", "f6g8"];
  let status = "play";
  for (let i = 0; i < 3; i++) for (const u of shuffle) { r = play(r, u); status = eng.statusOf(r); }
  console.log("   threefold: after 3x shuffle the start position has occurred 4 times; statusOf = " + status + " (no repetition rule exists)");
  // stalemate with the priority over material
  ok(eng.statusOf(E("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1")) === "stalemate", "stalemate");
}

// ----------------------------------------------------- random-play invariants
console.log("== invariants over random playouts");
{
  let games = 0, positions = 0, errors = 0;
  let rs = 24680;
  const rnd = () => { rs = (Math.imul(rs, 1103515245) + 12345) >>> 0; return rs / 4294967296; };
  const cnt = (b) => { const c = {}; for (const p of b) if (p) c[p] = (c[p] || 0) + 1; return c; };
  for (let g = 0; g < 300; g++) {
    let st = E(eng.START_FEN);
    for (let ply = 0; ply < 500; ply++) {
      const ms = eng.legal(st);
      positions++;
      const stat = eng.statusOf(st);
      // one king each
      const bk = st.b.filter((p) => p === "k").length, wk = st.b.filter((p) => p === "K").length;
      if (bk !== 1 || wk !== 1) { errors++; console.log("king count", wk, bk); break; }
      if (ms.length === 0 && stat !== "checkmate" && stat !== "stalemate") { errors++; console.log("no moves but status", stat); }
      if (ms.length > 0 && (stat === "checkmate" || stat === "stalemate")) { errors++; console.log("moves but status", stat); }
      // castling rights consistent with piece placement
      const c = st.cast, b = st.b;
      if ((c.K && !(b[60] === "K" && b[63] === "R")) || (c.Q && !(b[60] === "K" && b[56] === "R")) ||
          (c.k && !(b[4] === "k" && b[7] === "r")) || (c.q && !(b[4] === "k" && b[0] === "r"))) {
        errors++; console.log("castling right set without king/rook at home", JSON.stringify(c)); break;
      }
      if (stat === "checkmate" || stat === "stalemate" || stat === "draw50" || stat === "material") break;
      const m = ms[Math.floor(rnd() * ms.length)];
      const before = cnt(st.b);
      const nx = eng.make(st, m);
      const after = cnt(nx.b);
      const total = (o) => Object.values(o).reduce((a, x) => a + x, 0);
      const expect = total(before) - (m.cap ? 1 : 0);
      if (total(after) !== expect) { errors++; console.log("piece count off", m.flag, uci(m)); break; }
      if (m.flag === "dbl" ? nx.ep !== (m.from + m.to) / 2 : nx.ep !== -1) { errors++; console.log("ep bookkeeping", uci(m)); break; }
      const reset = m.piece === "P" || m.piece === "p" || !!m.cap;
      if (nx.half !== (reset ? 0 : st.half + 1)) { errors++; console.log("halfmove bookkeeping", uci(m)); break; }
      if (nx.full !== st.full + (st.turn === "b" ? 1 : 0)) { errors++; console.log("fullmove bookkeeping", uci(m)); break; }
      st = nx;
    }
    games++;
  }
  ok(errors === 0, "random-play invariants: " + games + " games, " + positions + " positions", "errors=" + errors);
}

// ---- perft against the PUBLISHED counts --------------------------------------
// (chessprogramming.org's perft results).  Depth is capped so the whole file stays
// a few seconds; the engine was checked to depth 5 on the start position and
// position 3 and to depth 4 on the rest when this was written, and matched every
// count.  A move generator that matches these is not wrong in a way this game
// could expose.
{
  const REQ = [
    ["start", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", [20, 400, 8902, 197281]],
    ["kiwipete (castling, en passant, pins)", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", [48, 2039, 97862]],
    ["position 3 (the pawn/rook endgame)", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", [14, 191, 2812, 43238]],
    ["position 4 (promotions)", "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", [6, 264, 9467, 422333]],
    ["position 5 (castling rights)", "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", [44, 1486, 62379]],
    ["position 6", "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", [46, 2079, 89890]],
  ];
  for (const [name, fen, want] of REQ) {
    const s0 = eng.fromFen(fen);
    for (let d = 0; d < want.length; d++) {
      const got = eng.perft(s0, d + 1);
      ok(got === want[d], "perft " + name + " d" + (d + 1), "expected " + want[d] + ", got " + got);
    }
  }
  // the module's own self-test (what BRINGUP calls 27/27): until now nothing ran it
  const st = api._selftest();
  ok(st.fail === 0 && st.pass >= 27, "SwanChess._selftest(): " + st.pass + " passed, " + st.fail + " failed",
     st.notes.filter((n) => n.startsWith("FAIL")).join("; "));
}

console.log("\ntest_chess_engine: " + passes + " passed, " + fails + " FAILED");
if (fails) console.log(fails + " failure(s)");
else console.log("all checks passed");
process.exitCode = fails ? 1 : 0;
