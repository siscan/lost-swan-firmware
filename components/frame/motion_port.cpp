// The IDF-side MotionPort: adapts the scheduler's needs onto motion::.
// Target-only; host tests fake the port instead.

#include "frame/motion_port.h"

#include "motion/motion.h"

namespace swan {
namespace {

class IdfMotionPort final : public MotionPort {
public:
    Col col(int i) override {
        AxisInfo a;
        motion::info(i, a);  // seqlock-consistent state+index pair inside
        return Col{a.state, a.index, a.dest_index};
    }

    bool go(int i, int index) override { return motion::go(i, index) == ESP_OK; }

    bool spin(int i, int32_t flaps_s, int seconds) override {
        const int64_t usteps =
            (static_cast<int64_t>(flaps_s) * seconds * USTEPS_PER_FLAP_NUM) / USTEPS_PER_FLAP_DEN;
        return motion::step_open_loop(i, usteps, flaps_s) == ESP_OK;
    }
};

IdfMotionPort g_port;

}  // namespace

MotionPort& motion_port() { return g_port; }

}  // namespace swan
