// Header guard: prevents this file from being included more than once during compilation
#ifndef TOOLS_HANDLERS_H
// Defines the guard macro so subsequent includes are skipped
#define TOOLS_HANDLERS_H

// Include the cJSON library for parsing and building JSON objects (used as tool inputs)
#include "cJSON.h"
// Include standard bool type (true/false) for handler return values
#include <stdbool.h>
// Include size_t type used for buffer length parameters
#include <stddef.h>
// Include fixed-width integer types like uint8_t used in DHT sensor data
#include <stdint.h>

// Tool handler convention:
// - return true when the operation is handled (including benign "not found" states)
// - return false on validation or execution errors
// - always write a human-readable message to result.

// --- GPIO & Hardware Handlers ---

// Writes a value (high/low) to a specified GPIO pin
bool tools_gpio_write_handler(const cJSON *input, char *result, size_t result_len);
// Reads the current value of a specified GPIO pin
bool tools_gpio_read_handler(const cJSON *input, char *result, size_t result_len);
// Reads the current values of all GPIO pins at once
bool tools_gpio_read_all_handler(const cJSON *input, char *result, size_t result_len);
// Pauses execution for a specified duration (e.g. milliseconds)
bool tools_delay_handler(const cJSON *input, char *result, size_t result_len);
// Scans the I2C bus and reports which device addresses respond
bool tools_i2c_scan_handler(const cJSON *input, char *result, size_t result_len);
// Writes bytes to a device on the I2C bus at a given address
bool tools_i2c_write_handler(const cJSON *input, char *result, size_t result_len);
// Reads bytes from a device on the I2C bus at a given address
bool tools_i2c_read_handler(const cJSON *input, char *result, size_t result_len);
// Performs a combined I2C write-then-read (common for register-based devices)
bool tools_i2c_write_read_handler(const cJSON *input, char *result, size_t result_len);
// Reads temperature and humidity from a DHT sensor on a specified pin
bool tools_dht_read_handler(const cJSON *input, char *result, size_t result_len);

// --- Key-Value Memory & Persona Handlers ---

// Stores a named value in persistent memory
bool tools_memory_set_handler(const cJSON *input, char *result, size_t result_len);
// Retrieves a named value from persistent memory
bool tools_memory_get_handler(const cJSON *input, char *result, size_t result_len);
// Lists all keys currently stored in persistent memory
bool tools_memory_list_handler(const cJSON *input, char *result, size_t result_len);
// Deletes a named value from persistent memory
bool tools_memory_delete_handler(const cJSON *input, char *result, size_t result_len);
// Sets the assistant's persona (name, tone, behavior instructions, etc.)
bool tools_set_persona_handler(const cJSON *input, char *result, size_t result_len);
// Retrieves the currently active persona configuration
bool tools_get_persona_handler(const cJSON *input, char *result, size_t result_len);
// Resets the persona back to the default configuration
bool tools_reset_persona_handler(const cJSON *input, char *result, size_t result_len);

// --- Scheduler / Time Handlers ---

// Creates or updates a cron job to run a tool on a schedule
bool tools_cron_set_handler(const cJSON *input, char *result, size_t result_len);
// Lists all currently registered cron jobs
bool tools_cron_list_handler(const cJSON *input, char *result, size_t result_len);
// Deletes a cron job by its identifier
bool tools_cron_delete_handler(const cJSON *input, char *result, size_t result_len);
// Returns the current system time
bool tools_get_time_handler(const cJSON *input, char *result, size_t result_len);
// Sets the system timezone (e.g. "America/New_York")
bool tools_set_timezone_handler(const cJSON *input, char *result, size_t result_len);
// Returns the currently configured timezone
bool tools_get_timezone_handler(const cJSON *input, char *result, size_t result_len);

// --- System / User Tool Handlers ---

// Returns the firmware or application version string
bool tools_get_version_handler(const cJSON *input, char *result, size_t result_len);
// Returns a health status summary (uptime, memory, connectivity, etc.)
bool tools_get_health_handler(const cJSON *input, char *result, size_t result_len);
// Returns detailed diagnostic information for debugging
bool tools_get_diagnostics_handler(const cJSON *input, char *result, size_t result_len);
// Registers a new user-defined tool with its name, description, and logic
bool tools_create_tool_handler(const cJSON *input, char *result, size_t result_len);
// Lists all user-defined tools that have been created
bool tools_list_user_tools_handler(const cJSON *input, char *result, size_t result_len);
// Deletes a user-defined tool by name
bool tools_delete_user_tool_handler(const cJSON *input, char *result, size_t result_len);

// Only compiled in when running unit tests (TEST_BUILD flag must be defined)
#ifdef TEST_BUILD
// Test helper: decodes a raw 5-byte DHT payload and writes the interpreted result
// Used to verify the DHT decoding logic against known byte sequences
bool tools_dht_test_decode_bytes(const char *model_name,
                                 int pin,
                                 const uint8_t data[5],
                                 char *result,
                                 size_t result_len);
// Resets any mock state set up for DHT tests (call between test cases)
void tools_dht_test_reset(void);
// Configures the DHT mock to simulate a successful sensor read with the given 5 bytes
void tools_dht_test_set_mock_success(const uint8_t data[5]);
// Configures the DHT mock to simulate a sensor failure with the given error message
void tools_dht_test_set_mock_failure(const char *error_message);
#endif // End of TEST_BUILD-only section

#endif // TOOLS_HANDLERS_H — end of header guard
