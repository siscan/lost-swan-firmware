# Living with the display

For the person the display belongs to, rather than the person building it. If
you want to know *why* something is the way it is, that is
[`FIRMWARE_SPEC.md`](FIRMWARE_SPEC.md) §17; if you are bringing up hardware,
that is [`BRINGUP.md`](BRINGUP.md).

---

## What it is

A five-column split-flap display that spends about 99% of its life as a clock,
shows a five-glyph message when you ask, and runs the Swan station's
108-minute countdown on demand. At zero it spins and lands on a reveal frame
with the alarm sounds.

Everything is controlled from **its own web page** — no hub, no cloud, no
phone app. Point a browser at `http://lost.local/` on the same network.

There is a second page at **`http://lost.local/terminal.html`**: the Swan
terminal, meant for a screen of its own (a Pi in kiosk mode, a spare monitor).
It shows a large countdown, an on-screen keypad, and the flap display docked
small in a corner. It is a *view*, not a second brain — it renders the same
deadline the display holds.

## The light on the board

One RGB LED, and it is the fastest way to know what is happening:

| colour | meaning |
|---|---|
| dim violet, blinking once a second | powered, hasn't homed yet |
| amber, blinking about four times a second | homing — the columns are hunting for their sensors |
| **solid green** | homed, clock right, everything normal |
| blue, blinking twice a second | homed, but the clock hasn't synced yet — see *No network* |
| **red, blinking about ten times a second** | at least one column has faulted — see *When a column faults* |

A healthy boot is roughly ten seconds of amber before green: a homing pass takes
about 7.5 seconds and the five columns are staggered a quarter of a second apart
on purpose, to keep the current draw down. If a column *cannot* find its sensor
it tries three times, so a genuinely broken one shows amber for around half a
minute before the light goes red — during which the display looks idle and is
actually searching.

## The pages

**Terminal** — the Numbers and EXECUTE, and the time remaining.
**Modes** — clock, message or countdown, the message glyph picker, presets.
**Calibrate** — the per-column nudges you use when a card doesn't hang right,
the index walk, a test spin, and the speed sliders.
**Diagnostics** — per-column counters, WiFi, heap, uptime, the running image,
MQTT and the cue inventory.
**Settings** — clock and countdown behaviour, the reveal frame, the ring table,
WiFi, MQTT, audio.
**Update** — firmware over the network, with a rollback if it goes wrong.

## Things worth knowing before they happen

**A power cut loses nothing that matters.** The countdown is a *deadline*, not
a ticking timer: it is written to flash when you set it, so the display comes
back and carries on from where the clock says it should be. Column settings and
calibration survive too. This is tested at twelve points through a run,
including during the alarm itself (BRINGUP §28).

**If the power goes out after the countdown has already hit zero**, the display
wakes up showing the reveal frame — silently. It will not replay the alarm.
That is deliberate: the alarm belongs to the moment it happened.

**No network is a supported state.** The display keeps time on its own — it
drifts by seconds a day, not minutes — and re-syncs when the network returns.
It will **never** put itself into setup mode because the router rebooted: a wall
display that drops off the network to fix a problem that fixes itself is worse
than the outage. If it has *never* had credentials, it shows the WiFi glyph on
the middle column after 15 seconds and waits.

**When a column faults**, the other four keep running and the display tells you
which one and why, on a banner visible from every page. There are three causes
and they are not the same:

- *no_hall* and *slip* — the column re-homes itself, up to three times.
- *jam* — the drum stopped while the motor kept going. It is **not** retried,
  because another attempt drives the motor into whatever is in the way for
  7.5 seconds. **Clear the obstruction first**, then press REHOME.

If **two** columns fault at once, or one faults during the alarm spin, the
display cuts power to all five motors on purpose — two failures together is
usually power, wiring or the frame, and the mechanism is worth more than the
display. Diagnostics will say `step timer`/`drivers` are off; re-energise from
the Calibrate page once you have looked.

**Before you put your hands in the mechanism**, turn on **maintenance**
(Settings → Maintenance). Nothing schedules, nothing re-homes itself, and the
motors are released. It survives a reboot on purpose, so pulling the power
mid-repair cannot restart a countdown on top of your hands. Leaving maintenance
re-homes everything, because the drums have been moved by hand.

## MQTT and Home Assistant

Optional, off until you configure it, and the display never waits on it.

**Read this before you turn it on:** there is deliberately **no password in the
firmware**. Anyone who can publish to your broker can drive the display —
including rebooting it, putting it in maintenance and starting a firmware
update. That is what makes Home Assistant discovery work for the controls worth
having. **Scope the broker's ACL, not the firmware.** If your broker is open to
your whole network, so is your display.

## Sounds

Five cues: two warnings, the failure alarm, and two UI clicks. The ones that
ship are synthesized placeholders, not the show's sounds — replace any of them
from Settings → Audio with a 16-bit mono WAV, no reflash needed. **Check the
duration the page shows afterwards**: a cue that is present and valid can still
be a fraction of a second long, and nothing else will tell you.

Volume, mute and quiet hours are on the same page. Quiet hours silence the
countdown cues; pressing PLAY yourself always plays.

## What it remembers

**The log** (`http://lost.local/api/log`) is the last few thousand lines the
firmware printed, in memory, lost at power-off. Useful when something has just
gone wrong. It opens in a browser tab; there is no button for it yet.

**The journal** (`http://lost.local/api/journal`) is permanent and much
shorter: every countdown you started and how, every zero, every fault and
recovery, every mode change, and every boot with the reason it restarted. It
survives power loss and is the display's actual history. Also a URL rather than
a button for now.

## Updating

Update page → choose the `.bin` → UPLOAD AND REBOOT. The display writes it to
the spare slot and restarts into it, and if the new firmware cannot get itself
running within two minutes the bootloader puts the old one back by itself.

While an image is *pending*, the page says so and offers CONFIRM and ROLL BACK.
Rebooting while it is pending is what *triggers* the rollback, so the REBOOT
button asks first.

## If it is wrong and you want to start over

- **Wrong time**: check Settings → Clock has your POSIX TZ string. A wrong
  timezone looks exactly like a broken clock.
- **The columns show the wrong characters**: the ring table describes what is
  printed on the drums. Settings → Ring, upload the right one.
- **A card doesn't sit flat**: Calibrate, nudge that column ±1 until it hangs
  right against the lip, then SAVE. The nudge moves the drum as you press it.
- **The display is on the wrong network**: Settings → WiFi. It keeps working on
  the old network until the new one accepts it, so a typo costs a retry, not
  access. If it cannot reach any network, `wifi.provision` (or the console)
  raises an open access point called `LOST-Swan-xxxx` with a setup page.
