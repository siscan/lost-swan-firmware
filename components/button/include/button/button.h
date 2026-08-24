// The physical button (spec 2, 10.2a, Q6): press = Execute, hold = re-home all.
//
// The gesture logic is pure and lives in gesture.h; this is the two-line shell
// interface.  Call after the dispatcher Context exists - the button drives the
// same `api::handle_command` every network path uses, with `Origin::Button`.
#pragma once

namespace swan {
namespace api {
struct Context;
}

namespace button {

// Configures GPIO28 as a pulled-up input and starts the poll task.  Never
// aborts: a pin that will not configure disables the button for this boot and
// says so, because this is a convenience control on a wall-mounted display.
void init(api::Context& ctx);

}  // namespace button
}  // namespace swan
