// Header guard: prevents this file from being included more than once during compilation
#ifndef GPIO_POLICY_H
// Defines the guard macro so subsequent includes are skipped
#define GPIO_POLICY_H

// Include standard bool type (true/false) for function return values
#include <stdbool.h>
// Include size_t type used for the result buffer length parameter
#include <stddef.h>

// Checks if a given GPIO pin number is permitted by the current policy configuration
// Returns true if the pin is allowed, false if it is blocked or out of range
bool gpio_policy_pin_is_allowed(int pin);

// If the given pin is forbidden, writes a human-readable explanation into the result buffer
// Returns true if a hint was written (pin is forbidden), false if pin is actually allowed
bool gpio_policy_pin_forbidden_hint(int pin, char *result, size_t result_len);

// Checks whether a pin is safe to use as a runtime input (e.g. reading a button or sensor)
// Applies stricter checks than gpio_policy_pin_is_allowed — blocks flash/boot-critical pins
// Returns true if the pin is safe to read from at runtime, false otherwise
bool gpio_policy_runtime_input_pin_is_safe(int pin);

// Only compiled in when running unit tests (TEST_BUILD flag must be defined)
#ifdef TEST_BUILD

// Test variant of gpio_policy_pin_is_allowed that accepts explicit policy parameters
// instead of reading from global config — allows testing different policy combinations
// pin                   : the GPIO pin number to check
// csv                   : comma-separated list of explicitly allowed pin numbers (e.g. "2,4,5")
// min_pin               : the minimum valid GPIO pin number (inclusive)
// max_pin               : the maximum valid GPIO pin number (inclusive)
// block_esp32_flash_pins: if true, pins used by ESP32 flash (e.g. 6-11) are always blocked
// require_valid_gpio    : if true, only pins valid on the ESP32 GPIO matrix are accepted
bool gpio_policy_test_pin_is_allowed(int pin,
                                     const char *csv,
                                     int min_pin,
                                     int max_pin,
                                     bool block_esp32_flash_pins,
                                     bool require_valid_gpio);

// Test variant of gpio_policy_runtime_input_pin_is_safe with explicit policy parameters
// pin                   : the GPIO pin number to check
// block_esp32_flash_pins: if true, flash-related pins are blocked regardless of csv
// require_valid_gpio    : if true, enforces ESP32 GPIO validity rules
bool gpio_policy_test_runtime_input_pin_is_safe(int pin,
                                                bool block_esp32_flash_pins,
                                                bool require_valid_gpio);
#endif // End of TEST_BUILD-only section

#endif  // GPIO_POLICY_H — end of header guard
