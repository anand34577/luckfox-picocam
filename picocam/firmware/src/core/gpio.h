#pragma once

// Plain sysfs GPIO outputs (relay, IR LED, alarm lamp).
namespace gpio {
bool set(int pin, bool value);                                // exports the pin as an output on first use
void pulse(int pin, bool active_high, int ms);                // asserts the pin for `ms` on a short-lived thread
}  // namespace gpio
