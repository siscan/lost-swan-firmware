// Compiled ONLY by the negative tests in test/host/CMakeLists.txt, each with a
// pair of flavour defines that limits_policy.h must refuse (SWAN_UNLIMITED with
// SWAN_BENCH, and with SWAN_RELEASE).  A file that compiles cleanly here is the
// failure.  There is nothing else in it on purpose: the include IS the test.
#include "motion/limits_policy.h"
