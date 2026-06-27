/* GPIO policy public API: gpio_policy_pin_is_allowed, gpio_policy_pin_forbidden_hint, etc. */
#include "gpio_policy.h"

/* Project-wide configuration constants: GPIO_ALLOWED_PINS_CSV, GPIO_MIN_PIN, GPIO_MAX_PIN */
#include "config.h"
/* ESP-IDF GPIO driver: gpio_num_t type and GPIO_IS_VALID_GPIO() macro */
#include "driver/gpio.h"

/* Standard library: strtol() for parsing integers from the CSV allowlist */
#include <stdlib.h>
/* Standard I/O: snprintf() for writing the forbidden-pin hint message */
#include <stdio.h>
/* INT_MAX — used as an unbounded upper limit in the runtime safety check */
#include <limits.h>

/*
 * GPIO_IS_VALID_GPIO() fallback definition.
 * The ESP-IDF GPIO driver normally provides this macro, which evaluates to
 * true when the pin number is a valid GPIO on the target SoC. If the macro
 * is not available (e.g. during unit-test builds without the full IDF),
 * this fallback accepts any non-negative pin number.
 */
#ifndef GPIO_IS_VALID_GPIO
#define GPIO_IS_VALID_GPIO(pin) ((pin) >= 0)
#endif

/* --------------------------------------------------------------------------
 * pin_in_allowlist() — checks whether a pin number appears in a
 * comma-separated list of integers (e.g. "2,4,5,18,19").
 *
 * The parser is deliberately permissive:
 *   - Leading/trailing spaces and tabs around entries are skipped.
 *   - Consecutive commas (empty fields) are skipped.
 *   - Any token that cannot be parsed as a number is skipped rather than
 *     causing a hard error, so a malformed config string degrades gracefully.
 *
 * Returns true as soon as a matching entry is found; false if the list is
 * NULL, empty, or contains no entry equal to pin.
 * -------------------------------------------------------------------------- */
static bool pin_in_allowlist(int pin, const char *csv)
{
    const char *cursor; /* Pointer that walks forward through the CSV string */

    /* Treat a NULL or empty CSV as an empty list — pin is not allowed */
    if (!csv || csv[0] == '\0') {
        return false;
    }

    cursor = csv;
    while (*cursor != '\0') {
        char *endptr = NULL; /* strtol sets this to the first unconsumed character */
        long value;

        /* Skip any leading whitespace characters and commas (delimiters) */
        while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') {
            cursor++;
        }
        /* Reached end of string after skipping delimiters — stop */
        if (*cursor == '\0') {
            break;
        }

        /* Attempt to parse the next token as a base-10 integer */
        value = strtol(cursor, &endptr, 10);
        if (endptr == cursor) {
            /* strtol consumed no characters — token is not a number.
               Skip forward to the next comma (or end of string) and continue. */
            while (*cursor != '\0' && *cursor != ',') {
                cursor++;
            }
            continue;
        }

        /* Successfully parsed a number — check if it matches the target pin */
        if ((int)value == pin) {
            return true;
        }
        /* Advance cursor past the consumed number token */
        cursor = endptr;
    }

    /* Exhausted the entire list without finding a match */
    return false;
}

/* --------------------------------------------------------------------------
 * pin_is_allowed_impl() — core policy evaluation function. All public
 * gpio_policy_* functions delegate here.
 *
 * Parameters:
 *   pin                    — GPIO pin number to evaluate (must be >= 0)
 *   allowlist_csv          — if non-NULL and non-empty, the pin must appear
 *                            in this CSV string; otherwise the min/max range
 *                            is used instead
 *   min_pin / max_pin      — inclusive range used when allowlist_csv is not
 *                            provided
 *   block_esp32_flash_pins — if true, GPIO6–11 are always rejected regardless
 *                            of the allowlist or range (ESP32 classic only;
 *                            those pins are wired to SPI flash/PSRAM)
 *   require_valid_gpio     — if true, the pin must also pass
 *                            GPIO_IS_VALID_GPIO() as a final hardware check
 *
 * Returns true only if the pin passes every applicable check.
 * -------------------------------------------------------------------------- */
static bool pin_is_allowed_impl(int pin,
                                const char *allowlist_csv,
                                int min_pin,
                                int max_pin,
                                bool block_esp32_flash_pins,
                                bool require_valid_gpio)
{
    bool in_policy; /* Tracks whether the pin satisfies the allowlist/range check */

    /* Negative pin numbers are never valid on any supported target */
    if (pin < 0) {
        return false;
    }

    /* On ESP32 classic, GPIO6–11 are connected to the internal SPI flash
       (and optionally PSRAM). Driving them from user code corrupts flash
       access, so they are unconditionally blocked when the flag is set. */
    if (block_esp32_flash_pins && pin >= 6 && pin <= 11) {
        return false;
    }

    /* Determine whether the pin is within the configured policy:
       - If an explicit allowlist CSV is provided, use it.
       - Otherwise, accept any pin within the [min_pin, max_pin] range. */
    if (allowlist_csv && allowlist_csv[0] != '\0') {
        in_policy = pin_in_allowlist(pin, allowlist_csv);
    } else {
        in_policy = pin >= min_pin && pin <= max_pin;
    }

    /* Reject the pin if it does not satisfy the policy */
    if (!in_policy) {
        return false;
    }

    /* Optional final hardware validity check via the IDF macro */
    if (require_valid_gpio) {
        return GPIO_IS_VALID_GPIO((gpio_num_t)pin);
    }

    return true;
}

/* --------------------------------------------------------------------------
 * gpio_policy_pin_is_allowed() — public API for checking whether a pin may
 * be used by a tool (gpio_write, gpio_read, etc.).
 *
 * Uses the project's configured allowlist (GPIO_ALLOWED_PINS_CSV) or the
 * GPIO_MIN_PIN / GPIO_MAX_PIN range from config.h. Flash pins (GPIO6–11) are
 * blocked on the ESP32 classic target but not on other targets (ESP32-C3/C6/S3
 * have different flash pin assignments handled by the IDF).
 * -------------------------------------------------------------------------- */
bool gpio_policy_pin_is_allowed(int pin)
{
#if defined(CONFIG_IDF_TARGET_ESP32)
    /* ESP32 classic: apply the configured policy AND block flash pins 6–11 */
    return pin_is_allowed_impl(pin, GPIO_ALLOWED_PINS_CSV, GPIO_MIN_PIN, GPIO_MAX_PIN, true, true);
#else
    /* All other targets: apply the configured policy; no flash-pin block needed */
    return pin_is_allowed_impl(pin, GPIO_ALLOWED_PINS_CSV, GPIO_MIN_PIN, GPIO_MAX_PIN, false, true);
#endif
}

/* --------------------------------------------------------------------------
 * gpio_policy_runtime_input_pin_is_safe() — broader safety check used when
 * a pin number is supplied at runtime (e.g. as an I2C SDA/SCL argument)
 * rather than being drawn from the configured allowlist.
 *
 * Accepts any non-negative pin that passes the hardware validity check.
 * No allowlist or max-pin ceiling is applied (INT_MAX as upper bound),
 * but flash pins are still blocked on the ESP32 classic target.
 * -------------------------------------------------------------------------- */
bool gpio_policy_runtime_input_pin_is_safe(int pin)
{
#if defined(CONFIG_IDF_TARGET_ESP32)
    /* ESP32 classic: allow any valid GPIO except flash pins 6–11 */
    return pin_is_allowed_impl(pin, NULL, 0, INT_MAX, true, true);
#else
    /* Other targets: allow any non-negative pin that the IDF considers valid */
    return pin_is_allowed_impl(pin, NULL, 0, INT_MAX, false, true);
#endif
}

/* --------------------------------------------------------------------------
 * gpio_policy_pin_forbidden_hint() — writes a human-readable explanation
 * into result when a pin is rejected for a well-known reason, so the caller
 * can return a helpful error message to the user/agent rather than a generic
 * "pin not allowed".
 *
 * Currently handles only the ESP32 classic flash-pin range (GPIO6–11).
 * On other targets the parameters are suppressed to avoid compiler warnings.
 *
 * Returns true if a hint was written (and result was populated), false if no
 * specific hint applies (caller should use a generic error message instead).
 * -------------------------------------------------------------------------- */
bool gpio_policy_pin_forbidden_hint(int pin, char *result, size_t result_len)
{
#if defined(CONFIG_IDF_TARGET_ESP32)
    /* GPIO6–11 are wired to the ESP32's SPI flash/PSRAM — give a specific message */
    if (pin >= 6 && pin <= 11) {
        snprintf(result, result_len,
                 "Error: pin %d is reserved for ESP32 flash/PSRAM (GPIO6-11); choose a different pin",
                 pin);
        return true; /* Hint was written */
    }
#else
    /* No special forbidden-pin hints on non-ESP32-classic targets */
    (void)pin;
    (void)result;
    (void)result_len;
#endif

    return false; /* No specific hint available for this pin */
}

/*
 * TEST_BUILD section — only compiled when running unit tests.
 *
 * These thin wrappers expose pin_is_allowed_impl() with fully parameterised
 * arguments so test cases can exercise every code path (allowlist, range,
 * flash-pin blocking, IDF validity check) independently of the compile-time
 * config.h constants.
 */
#ifdef TEST_BUILD

/* gpio_policy_test_pin_is_allowed() — test wrapper that forwards all
   pin_is_allowed_impl() parameters directly, bypassing config.h defaults. */
bool gpio_policy_test_pin_is_allowed(int pin,
                                     const char *csv,
                                     int min_pin,
                                     int max_pin,
                                     bool block_esp32_flash_pins,
                                     bool require_valid_gpio)
{
    return pin_is_allowed_impl(pin, csv, min_pin, max_pin, block_esp32_flash_pins, require_valid_gpio);
}

/* gpio_policy_test_runtime_input_pin_is_safe() — test wrapper for the
   runtime safety check with configurable flash-pin and validity flags,
   mirroring the public API but without the compile-time target guard. */
bool gpio_policy_test_runtime_input_pin_is_safe(int pin,
                                                bool block_esp32_flash_pins,
                                                bool require_valid_gpio)
{
    return pin_is_allowed_impl(pin, NULL, 0, INT_MAX, block_esp32_flash_pins, require_valid_gpio);
}

#endif /* TEST_BUILD */
