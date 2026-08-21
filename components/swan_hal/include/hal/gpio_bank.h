// Single-bank GPIO access for the step ISR.  All five STEP pulses and all five
// Hall samples are one register write / one register read (spec 5.2).
//
// Every C5 GPIO is 0..28, so one 32-bit bank covers the whole map - pins.h
// static_asserts that.
#pragma once

#include <cstdint>

#include "hal/pins.h"
#include "soc/gpio_reg.h"

namespace swan {

constexpr uint32_t pin_mask(int gpio) { return 1u << gpio; }

constexpr uint32_t bank_mask(const int (&a)[N_COLUMNS]) {
    uint32_t m = 0;
    for (int i = 0; i < N_COLUMNS; ++i) m |= pin_mask(a[i]);
    return m;
}

inline constexpr uint32_t STEP_MASK_ALL = bank_mask(PIN_STEP);
inline constexpr uint32_t HALL_MASK_ALL = bank_mask(PIN_HALL);

// always_inline (not IRAM_ATTR): these fold into the caller, so they live in
// whatever section the caller does - which for the step ISR is IRAM.
__attribute__((always_inline)) static inline void gpio_bank_set(uint32_t mask) {
    REG_WRITE(GPIO_OUT_W1TS_REG, mask);
}
__attribute__((always_inline)) static inline void gpio_bank_clear(uint32_t mask) {
    REG_WRITE(GPIO_OUT_W1TC_REG, mask);
}
__attribute__((always_inline)) static inline uint32_t gpio_bank_read() {
    return REG_READ(GPIO_IN_REG);
}

}  // namespace swan
