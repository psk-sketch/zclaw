/* ============================================================
 * tools_handlers.c
 *
 * This file implements the actual logic ("handlers") for a set
 * of tools that an AI assistant (running on an ESP32
 * microcontroller) can call at runtime.
 *
 * The four tools are:
 *   1. gpio_write  – set a GPIO pin HIGH or LOW
 *   2. gpio_read   – read the current level of one GPIO pin
 *   3. gpio_read_all – read every allowed GPIO pin at once
 *   4. delay       – pause execution for N milliseconds
 *
 * All handlers receive a cJSON object (parsed JSON from the
 * AI's tool call), write a human-readable result string, and
 * return true on success or false on failure.
 * ============================================================ */

/* --- Includes ---------------------------------------------- */
#include "tools_handlers.h"   // Declarations for the handler functions defined here
#include "tools_common.h"     // Shared helpers, e.g. tools_validate_allowed_gpio_pin()
#include "config.h"           // Board-level config: GPIO_MIN_PIN, GPIO_MAX_PIN, GPIO_ALLOWED_PINS_CSV, etc.
#include "gpio_policy.h"      // Policy layer: gpio_policy_pin_is_allowed(), gpio_input_enable(), etc.
#include "gpio_mapping.h"     // Persistent GPIO mapping module
#include "driver/gpio.h"      // ESP-IDF GPIO driver: gpio_set_direction(), gpio_set_level(), gpio_get_level()
#include "esp_log.h"          // ESP-IDF logging: ESP_LOGI() macro
#include "freertos/FreeRTOS.h"// FreeRTOS base header (required before task.h)
#include "freertos/task.h"    // FreeRTOS task API: vTaskDelay(), pdMS_TO_TICKS()
#include <limits.h>           // INT_MIN, INT_MAX – used when parsing CSV integers safely
#include <stdio.h>            // snprintf()
#include <stdlib.h>           // strtol() – string-to-long for parsing CSV pin numbers
#include <string.h>           // String utilities (included for general use)

/* --- Constants --------------------------------------------- */

// Safety cap on how long the AI can ask the firmware to sleep.
// Prevents accidental (or adversarial) infinite-style delays.
#define DELAY_MAX_MS 60000    // 60 000 ms = 60 seconds maximum delay

/* --- Module-level logging tag ------------------------------ */

// All ESP_LOG* calls from this file will appear with the tag "tools"
// in the serial monitor, making it easy to filter log output.
static const char *TAG = "tools";


/* ============================================================
 * STATIC HELPER: gpio_append_read_state()
 *
 * Reads one GPIO pin and appends  "pin=HIGH"  or  "pin=LOW"
 * to an in-progress output string.
 *
 * Parameters:
 *   cursor    – pointer to the current write position in the buffer
 *   remaining – pointer to the number of bytes still available
 *   pin       – the GPIO pin number to read
 *   first_pin – true if this is the first pin (skip leading ", ")
 *
 * Returns true on success, false if reading or formatting failed.
 * ============================================================ */
static bool gpio_append_read_state(char **cursor, size_t *remaining, int pin, bool first_pin)
{
    int level;    // Will hold 0 (LOW) or 1 (HIGH) after reading the pin
    int written;  // Bytes written by snprintf (used for error-checking)

    // Enable the input path on this pin without changing its output
    // drive mode or level – important if the pin is also being driven.
    if (gpio_input_enable(pin) != 0) {   // Non-zero return means failure
        return false;                     // Abort; caller will report the error
    }

    level = gpio_get_level(pin);  // Sample the electrical level: returns 0 or 1

    // Append to the caller's buffer.
    // Format: "pin=HIGH"  for the first pin,
    //         ", pin=LOW"  for every subsequent pin.
    written = snprintf(*cursor, *remaining, "%s%d=%s",
                       first_pin ? "" : ", ",   // No leading comma for the first entry
                       pin,                      // The GPIO number, e.g. 4
                       level ? "HIGH" : "LOW");  // Human-readable level

    // snprintf returns the number of chars it *would* write (excluding \0).
    // A negative value means an encoding error; a value >= remaining means
    // the output was truncated – both are treated as failures.
    if (written < 0 || (size_t)written >= *remaining) {
        return false;
    }

    // Advance the write cursor and shrink the remaining-space counter.
    *cursor    += (size_t)written;
    *remaining -= (size_t)written;

    return true;  // Successfully appended this pin's state
}


/* ============================================================
 * STATIC HELPER: resolve_pin_argument()
 *
 * Resolves the "pin" JSON field to a GPIO pin number.
 * The field can be either a numeric GPIO pin or a mapped device name.
 * If a device name is provided, it is looked up in the gpio_mapping module.
 *
 * Parameters:
 *   pin_json   - the cJSON item representing the pin argument
 *   pin_out    - pointer where the resolved GPIO number will be stored
 *   result     - buffer for error messages
 *   result_len - size of the result buffer
 *
 * Returns true if the pin is successfully resolved, false otherwise.
 * ============================================================ */
static bool resolve_pin_argument(const cJSON *pin_json, int *pin_out, char *result, size_t result_len)
{
    // Ensure the pin json element is not NULL
    if (!pin_json) {
        snprintf(result, result_len, "pin must be a GPIO number or mapped device name");
        return false;
    }

    // Check if the pin is a numeric value
    if (cJSON_IsNumber(pin_json)) {
        // Device-name resolution logic: backward compatibility with numeric pins
        *pin_out = pin_json->valueint;
        return true;
    } 
    // Check if the pin is a string representing a mapped device name
    else if (cJSON_IsString(pin_json)) {
        const char *device_name = pin_json->valuestring;
        if (!device_name || strlen(device_name) == 0) {
            snprintf(result, result_len, "pin must be a GPIO number or mapped device name");
            return false;
        }
        // Device-name resolution logic: call gpio_mapping_get_pin() to lookup the device name.
        // If the mapping exists, we use the returned GPIO number.
        // If the mapping does not exist, we return the error message "Unknown device '<name>'".
        if (!gpio_mapping_get_pin(device_name, pin_out)) {
            snprintf(result, result_len, "Unknown device '%s'", device_name);
            return false;
        }
        return true;
    }

    // If it is neither a number nor a string, return the updated error message
    snprintf(result, result_len, "pin must be a GPIO number or mapped device name");
    return false;
}


/* ============================================================
 * HANDLER: tools_gpio_write_handler()
 *
 * Tool: gpio_write
 * Purpose: Drive a GPIO pin HIGH (1) or LOW (0).
 *
 * Expected JSON input:
 *   { "pin": <number>, "state": <0 or 1> }
 *
 * On success writes e.g. "Pin 4 → HIGH" into result.
 * ============================================================ */
bool tools_gpio_write_handler(const cJSON *input, char *result, size_t result_len)
{
    // Extract the "pin" field from the JSON object
    cJSON *pin_json   = cJSON_GetObjectItem(input, "pin");
    // Extract the "state" field from the JSON object
    cJSON *state_json = cJSON_GetObjectItem(input, "state");

    int pin;
    // Resolve the "pin" argument to a physical GPIO number (either numeric pin or device name)
    if (!resolve_pin_argument(pin_json, &pin, result, result_len)) {
        return false;  // Return early – nothing else can proceed without a valid pin
    }

    // Validate that "state" exists and is a JSON number
    if (!state_json || !cJSON_IsNumber(state_json)) {
        snprintf(result, result_len, "Error: 'state' required (0 or 1)");
        return false;
    }

    int state = state_json->valueint;  // Desired level: 0 = LOW, anything else = HIGH

    // Check the pin against the board's allow-list (defined in config/policy).
    // If not allowed, the helper writes an error into result and returns false.
    if (!tools_validate_allowed_gpio_pin(pin, NULL, result, result_len)) {
        return false;
    }

    // Configure the pin as input+output (so it can both drive and be read back),
    // then set the electrical level.  Either ESP-IDF call returning non-zero = failure.
    if (gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT) != 0 ||
        gpio_set_level(pin, state ? 1 : 0) != 0) {
        snprintf(result, result_len, "Error: failed to configure/write pin %d", pin);
        return false;
    }

    // Success – report what was done
    snprintf(result, result_len, "Pin %d → %s", pin, state ? "HIGH" : "LOW");
    return true;
}


/* ============================================================
 * HANDLER: tools_gpio_read_handler()
 *
 * Tool: gpio_read
 * Purpose: Read the current level of a single GPIO pin.
 *
 * Expected JSON input:
 *   { "pin": <number> }
 *
 * On success writes e.g. "Pin 4 = HIGH" into result.
 * ============================================================ */
bool tools_gpio_read_handler(const cJSON *input, char *result, size_t result_len)
{
    // Extract the "pin" field from the JSON object
    cJSON *pin_json = cJSON_GetObjectItem(input, "pin");

    int pin;
    // Resolve the "pin" argument to a physical GPIO number (either numeric pin or device name)
    if (!resolve_pin_argument(pin_json, &pin, result, result_len)) {
        return false;
    }

    // Ensure the pin is on the board's allow-list
    if (!tools_validate_allowed_gpio_pin(pin, NULL, result, result_len)) {
        return false;
    }

    // Enable the input path on this pin (does not change drive mode/level)
    if (gpio_input_enable(pin) != 0) {
        snprintf(result, result_len, "Error: failed to configure/read pin %d", pin);
        return false;
    }

    int level = gpio_get_level(pin);  // Sample the electrical level: 0 or 1

    // Format the result string, e.g. "Pin 4 = LOW"
    snprintf(result, result_len, "Pin %d = %s", pin, level ? "HIGH" : "LOW");
    return true;
}


/* ============================================================
 * HANDLER: tools_gpio_read_all_handler()
 *
 * Tool: gpio_read_all
 * Purpose: Read every allowed GPIO pin and return all levels
 *          in a single comma-separated string.
 *
 * No JSON input fields are required.
 *
 * Example output: "GPIO states: 4=HIGH, 5=LOW, 18=HIGH"
 *
 * Two operating modes:
 *   A) If GPIO_ALLOWED_PINS_CSV is a non-empty string, only the
 *      pins listed there are read (in CSV order).
 *   B) Otherwise every pin from GPIO_MIN_PIN to GPIO_MAX_PIN
 *      that passes the policy check is read.
 * ============================================================ */
bool tools_gpio_read_all_handler(const cJSON *input, char *result, size_t result_len)
{
    char   *cursor    = result;      // Moving write pointer into the output buffer
    size_t  remaining = result_len;  // Bytes left in the output buffer
    int     written;                 // Bytes written by a single snprintf call
    int     count     = 0;           // Number of pins successfully read so far

    (void)input;  // This handler takes no input parameters; silence the unused-variable warning

    // Guard: result buffer must be valid and non-empty
    if (!result || result_len == 0) {
        return false;
    }

    // Write the fixed prefix that starts the output string
    written = snprintf(cursor, remaining, "GPIO states: ");
    if (written < 0 || (size_t)written >= remaining) {
        result[0] = '\0';  // Ensure the buffer is at least an empty string
        return false;
    }
    cursor    += (size_t)written;  // Advance past the prefix
    remaining -= (size_t)written;  // Account for the bytes just written

    /* ----------------------------------------------------------
     * MODE A: A specific CSV list of pins was configured
     * ---------------------------------------------------------- */
    if (GPIO_ALLOWED_PINS_CSV[0] != '\0') {   // Non-empty CSV string
        const char *csv_cursor = GPIO_ALLOWED_PINS_CSV;  // Walk through the CSV character by character

        while (*csv_cursor != '\0') {   // Until end of the CSV string
            char *endptr = NULL;  // strtol sets this to the first non-numeric character
            long  value;

            // Skip whitespace and comma separators between numbers
            while (*csv_cursor == ' ' || *csv_cursor == '\t' || *csv_cursor == ',') {
                csv_cursor++;
            }
            if (*csv_cursor == '\0') {  // Reached the end of the string after skipping separators
                break;
            }

            // Parse the next integer (the pin number) from the CSV
            value = strtol(csv_cursor, &endptr, 10);  // base 10

            if (endptr == csv_cursor) {
                // strtol made no progress → not a valid number; skip to the next comma
                while (*csv_cursor != '\0' && *csv_cursor != ',') {
                    csv_cursor++;
                }
                continue;
            }

            // Reject values that can't fit in a plain int (overflow safety)
            if (value < INT_MIN || value > INT_MAX) {
                csv_cursor = endptr;  // Skip past this token
                continue;
            }

            // Double-check via the runtime policy (handles strapping pins,
            // input-only pins, etc.) – even if it's in the CSV, it must be allowed
            if (!gpio_policy_pin_is_allowed((int)value)) {
                csv_cursor = endptr;  // Skip disallowed pin silently
                continue;
            }

            // Read the pin and append its state to the output buffer
            if (!gpio_append_read_state(&cursor, &remaining, (int)value, count == 0)) {
                snprintf(result, result_len, "Error: failed to read allowed GPIO pin state");
                return false;
            }
            count++;               // One more pin successfully appended
            csv_cursor = endptr;   // Advance past the number just consumed
        }

    /* ----------------------------------------------------------
     * MODE B: No CSV configured – iterate the full pin range
     * ---------------------------------------------------------- */
    } else {
        int pin;
        for (pin = GPIO_MIN_PIN; pin <= GPIO_MAX_PIN; pin++) {  // e.g. 0 to 39 on ESP32

            // Skip pins the policy says are off-limits
            if (!gpio_policy_pin_is_allowed(pin)) {
                continue;
            }

            // Read and append this pin's state
            if (!gpio_append_read_state(&cursor, &remaining, pin, count == 0)) {
                snprintf(result, result_len, "Error: failed to read allowed GPIO pin state");
                return false;
            }
            count++;  // Track how many pins we've appended
        }
    }

    // If we never appended a single pin, the configuration is broken
    if (count == 0) {
        snprintf(result, result_len, "Error: no allowed GPIO pins configured");
        return false;
    }

    return true;  // result now contains the full "GPIO states: x=HIGH, y=LOW, ..." string
}


/* ============================================================
 * HANDLER: tools_delay_handler()
 *
 * Tool: delay
 * Purpose: Pause firmware execution for a given number of ms.
 *          The FreeRTOS scheduler keeps running; only this task
 *          sleeps.
 *
 * Expected JSON input:
 *   { "milliseconds": <positive number, max 60000> }
 *
 * On success writes e.g. "Waited 500 ms" into result.
 * ============================================================ */
bool tools_delay_handler(const cJSON *input, char *result, size_t result_len)
{
    // Extract the "milliseconds" field from the JSON object
    cJSON *ms_json = cJSON_GetObjectItem(input, "milliseconds");

    // Validate: must be present and numeric
    if (!ms_json || !cJSON_IsNumber(ms_json)) {
        snprintf(result, result_len, "Error: 'milliseconds' required (number)");
        return false;
    }

    int ms = ms_json->valueint;  // The requested delay in milliseconds

    // Reject zero or negative values – they make no sense as a delay
    if (ms <= 0) {
        snprintf(result, result_len, "Error: milliseconds must be positive");
        return false;
    }

    // Enforce the safety cap to prevent lockups
    if (ms > DELAY_MAX_MS) {
        snprintf(result, result_len, "Error: max delay is %d ms (got %d)", DELAY_MAX_MS, ms);
        return false;
    }

    // Log the delay to the serial console so developers can see it happening
    ESP_LOGI(TAG, "Delaying %d ms...", ms);

    // Actually sleep:
    //   pdMS_TO_TICKS(ms) converts milliseconds to FreeRTOS tick counts.
    //   vTaskDelay() suspends only THIS task; the rest of the RTOS keeps running.
    vTaskDelay(pdMS_TO_TICKS(ms));

    // Report success
    snprintf(result, result_len, "Waited %d ms", ms);
    return true;
}


/* ============================================================
 * HANDLER: tools_gpio_mapping_save_handler()
 *
 * Tool: gpio_mapping_save
 * Purpose: Save a persistent, case-insensitive mapping of a device name to a GPIO pin.
 *
 * Expected JSON input:
 *   { "device": "<name>", "pin": <number>, "password": "<optional_password>" }
 *
 * On success writes "Successfully saved mapping: <device> -> pin <number>" into result.
 * ============================================================ */
bool tools_gpio_mapping_save_handler(const cJSON *input, char *result, size_t result_len)
{
    cJSON *device_json = cJSON_GetObjectItem(input, "device");
    cJSON *pin_json    = cJSON_GetObjectItem(input, "pin");
    cJSON *pwd_json    = cJSON_GetObjectItem(input, "password");

    // Validate that "device" exists and is a JSON string
    if (!device_json || !cJSON_IsString(device_json)) {
        snprintf(result, result_len, "Error: 'device' name is required (string)");
        return false;
    }

    // Validate that "pin" exists and is a JSON number
    if (!pin_json || !cJSON_IsNumber(pin_json)) {
        snprintf(result, result_len, "Error: 'pin' is required (number)");
        return false;
    }

    const char *device   = device_json->valuestring;
    int pin              = pin_json->valueint;
    const char *password = (pwd_json && cJSON_IsString(pwd_json)) ? pwd_json->valuestring : NULL;

    // Safety checks: We must validate the pin via tools_validate_allowed_gpio_pin()
    if (!tools_validate_allowed_gpio_pin(pin, NULL, result, result_len)) {
        return false;
    }

    // Call public API to save the persistent mapping
    esp_err_t err = gpio_mapping_save(device, pin, password);
    if (err == ESP_OK) {
        snprintf(result, result_len, "Successfully saved mapping: %s -> pin %d", device, pin);
        return true;
    } else {
        if (err == ESP_ERR_INVALID_ARG) {
            snprintf(result, result_len, "Error: invalid argument or incorrect password");
        } else if (err == ESP_ERR_INVALID_SIZE) {
            snprintf(result, result_len, "Error: device name too long (max 10 characters)");
        } else {
            snprintf(result, result_len, "Error: failed to save mapping (code %d)", err);
        }
        return false;
    }
}


/* ============================================================
 * HANDLER: tools_gpio_mapping_delete_handler()
 *
 * Tool: gpio_mapping_delete
 * Purpose: Delete a persistent GPIO mapping for a device name (case-insensitive).
 *
 * Expected JSON input:
 *   { "device": "<name>", "password": "<optional_password>" }
 *
 * On success writes "Successfully deleted mapping for device: <device>" into result.
 * ============================================================ */
bool tools_gpio_mapping_delete_handler(const cJSON *input, char *result, size_t result_len)
{
    cJSON *device_json = cJSON_GetObjectItem(input, "device");
    cJSON *pwd_json    = cJSON_GetObjectItem(input, "password");

    // Validate that "device" exists and is a JSON string
    if (!device_json || !cJSON_IsString(device_json)) {
        snprintf(result, result_len, "Error: 'device' name is required (string)");
        return false;
    }

    const char *device   = device_json->valuestring;
    const char *password = (pwd_json && cJSON_IsString(pwd_json)) ? pwd_json->valuestring : NULL;

    // Call public API to delete the persistent mapping
    esp_err_t err = gpio_mapping_delete(device, password);
    if (err == ESP_OK) {
        snprintf(result, result_len, "Successfully deleted mapping for device: %s", device);
        return true;
    } else {
        if (err == ESP_ERR_INVALID_ARG) {
            snprintf(result, result_len, "Error: incorrect password or invalid device");
        } else if (err == ESP_ERR_NVS_NOT_FOUND) {
            snprintf(result, result_len, "Error: mapping not found for device '%s'", device);
        } else {
            snprintf(result, result_len, "Error: failed to delete mapping (code %d)", err);
        }
        return false;
    }
}


/* ============================================================
 * HANDLER: tools_gpio_mapping_list_handler()
 *
 * Tool: gpio_mapping_list
 * Purpose: List all registered persistent GPIO mappings.
 *
 * No input parameters expected.
 *
 * On success writes JSON array of mappings into result.
 * ============================================================ */
bool tools_gpio_mapping_list_handler(const cJSON *input, char *result, size_t result_len)
{
    (void)input; // Unused parameter

    esp_err_t err = gpio_mapping_list(result, result_len);
    if (err == ESP_OK) {
        return true;
    } else {
        snprintf(result, result_len, "Error: failed to list mappings (code %d)", err);
        return false;
    }
}


/* ============================================================
 * TEST-BUILD ONLY: thin wrappers for unit-testing the pin
 * policy logic without needing real ESP32 hardware.
 *
 * These functions are compiled in only when TEST_BUILD is
 * defined (e.g. during host-side unit tests).
 * ============================================================ */
#ifdef TEST_BUILD

// Test whether a pin is allowed, using a custom CSV and pin range,
// without applying ESP32-specific hardware constraints.
bool tools_gpio_test_pin_is_allowed(int pin, const char *csv, int min_pin, int max_pin)
{
    // last two booleans: esp32_target=false, verbose=true
    return gpio_policy_test_pin_is_allowed(pin, csv, min_pin, max_pin, false, true);
}

// Same as above but also applies ESP32-specific hardware constraints
// (e.g. strapping pins, input-only pins) during the test.
bool tools_gpio_test_pin_is_allowed_for_esp32_target(int pin, const char *csv, int min_pin, int max_pin)
{
    // last two booleans: esp32_target=true, verbose=true
    return gpio_policy_test_pin_is_allowed(pin, csv, min_pin, max_pin, true, true);
}

#endif  // TEST_BUILD
