#include "motion/column_mode.h"
#include "motion/fault_policy.h"

namespace swan {

const char* fault_cause_name(FaultCause c) {
    switch (c) {
        case FaultCause::None:       return "none";
        case FaultCause::NoHallEver: return "no_hall";
        case FaultCause::Slip:       return "slip";
        case FaultCause::Jam:        return "jam";
    }
    return "?";
}

const char* escalation_name(Escalation e) {
    switch (e) {
        case Escalation::ParkColumn: return "park_column";
        case Escalation::StopColumn: return "stop_column";
        case Escalation::DropEnable: return "drop_enable";
    }
    return "?";
}

Escalation escalate_fault(FaultCause cause, int faulted_columns, bool during_spin) {
    // Worst moment first.  A fault while five drums are whirling at alarm
    // speed is the one case where continuing to drive could turn a fixable
    // problem into a broken gear, and the display is worth less than the
    // mechanism.
    if (during_spin) return Escalation::DropEnable;

    // More than one column failing at the same time is not five independent
    // coincidences - it is power, a loose loom, or the frame.
    if (faulted_columns >= 2) return Escalation::DropEnable;

    if (cause == FaultCause::Jam) return Escalation::StopColumn;
    return Escalation::ParkColumn;
}

const char* column_mode_name(ColumnMode m) {
    switch (m) {
        case ColumnMode::Real:     return "real";
        case ColumnMode::Sim:      return "sim";
        case ColumnMode::Disabled: return "disabled";
    }
    return "?";
}

bool column_mode_from_name(std::string_view s, ColumnMode& out) {
    if (s == "real") out = ColumnMode::Real;
    else if (s == "sim") out = ColumnMode::Sim;
    else if (s == "disabled" || s == "off") out = ColumnMode::Disabled;
    else return false;
    return true;
}

}  // namespace swan
