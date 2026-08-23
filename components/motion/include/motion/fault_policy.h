// Fault classification and escalation (spec 5.4, 5.8).  Pure - host-tested.
//
// Fault causes want OPPOSITE responses, and treating them alike is how a
// stepper ends up grinding into printed gear teeth for 7.5 s at a time.  The
// discriminator is whether the edge ARRIVED:
//
//   NoHallEver - a cold home that never saw a single edge.  Sensor, magnet or
//                wiring.  The drum is almost certainly spinning freely, so
//                retrying costs nothing but time.  RETRY.
//
//   Slip       - the edge arrived, in the wrong place by more than a flap.
//                The drum turned; it just lost registration against the motor
//                - a card catching, a pinion skipping a tooth.  A re-home
//                recovers it, and driving one homing pass into a free drum is
//                harmless.  RETRY.  (Phase 1.5 models and pins this.)
//
//   Jam        - an expected edge never came at all.  The drum has STOPPED
//                while the motor is still stepping.  That is mechanical
//                resistance, and another 7.5 s homing pass drives the motor
//                straight into it.  STOP AT ONCE.
//
// Nico asked for two causes and two responses; there are two RESPONSES, and
// slip earns its own cause because it is retryable for a different reason
// than a dead sensor is.  Folding it into Jam would have regressed the
// slip-recovery behaviour the simulated-axis suite has pinned since Phase 1.5.
//
// Escalation is a whole-machine judgement, not a per-axis one: one column
// misbehaving is a column problem, several at once is a structural one, and a
// fault during a high-speed spin is the worst moment to keep driving.
#pragma once

#include <cstdint>

namespace swan {

enum class FaultCause : uint8_t { None = 0, NoHallEver = 1, Slip = 2, Jam = 3 };
const char* fault_cause_name(FaultCause c);

enum class Escalation : uint8_t {
    ParkColumn,   // park this column, the rest of the display carries on
    StopColumn,   // stop this column immediately, no retry (jam)
    DropEnable,   // release EN for ALL five - something structural may be wrong
};
const char* escalation_name(Escalation e);

// Everything except a jam may be retried: those are the causes where the drum
// is known to still turn.
constexpr bool fault_retry_allowed(FaultCause c) {
    return c == FaultCause::NoHallEver || c == FaultCause::Slip;
}

// `faulted_columns` counts the columns in fault INCLUDING the one just raised.
// `during_spin` is true when any column is mid open-loop move at alarm speed -
// the zero choreography - where a mechanical problem is most likely to do
// damage and least likely to be noticed.
//
// NOTE ON THE HARDWARE (spec 2.2): EN is ganged across all five drivers and
// the pin map has exactly one spare non-strapping GPIO, so per-column
// de-energize is not possible.  ParkColumn and StopColumn stop STEPPING a
// column; its coils still hold standstill current.  DropEnable is the only
// true de-energize, and it necessarily takes the whole display with it.
Escalation escalate_fault(FaultCause cause, int faulted_columns, bool during_spin);

}  // namespace swan
