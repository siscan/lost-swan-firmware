# Reference files — supplied by Nico, not generated here

Drop these in before the first Claude Code session:

| file | source | used for |
|---|---|---|
| `README.md` (BUILD) | Lost Clock chat, `BUILD/README.md` | mechanical v6 spec + decision log §7 — the "don't re-litigate" list |
| `BOM.md` | Lost Clock chat, `BUILD/BOM.md` | hardware facts: drivers standalone at 1/16, JST harness. **Its A3144, its 12 V and its 1.1-1.2 A are all superseded** — `HARDWARE_PLAN_2.md` (A1121LUA-T, 20 V USB-C PD) and spec §5.7a (0.7 A) win; the file carries a banner saying so |
| `manifest.json` | Lost Clock chat, inside `COLUMN5_PRINT.zip` | **ring order** — the ring table is generated from this, never typed |

Rename the mechanical README to `MECHANICAL_README.md` so it doesn't collide
with the repo README.
