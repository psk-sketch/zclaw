// Header guard: prevents this file from being included more than once during compilation
#ifndef MEMORY_H
// Defines the guard macro so subsequent includes are skipped
#define MEMORY_H

// Include ESP-IDF's error type (esp_err_t) used as return values to signal success or failure
// Common values: ESP_OK (0) = success, ESP_ERR_* = various failure codes
#include "esp_err.h"
// Include standard bool type (true/false) for memory_get's return value
#include <stdbool.h>

// Initializes the NVS (Non-Volatile Storage) flash partition
// Must be called once at startup before any other memory_* functions are used
// Returns ESP_OK on success, or an error code if NVS is unavailable or corrupted
esp_err_t memory_init(void);

// Stores a string value under the given key in NVS flash
// Data persists across reboots — survives power loss and resets
// key   : a short identifier string for the value (NVS keys are max 15 characters)
// value : the null-terminated string to store
// Returns ESP_OK on success, or an error code if the write fails
esp_err_t memory_set(const char *key, const char *value);

// Retrieves the string value stored under the given key from NVS flash
// key     : the identifier to look up
// value   : output buffer where the retrieved string will be written
// max_len : size of the output buffer to prevent overflow
// Returns true if the key was found and value was written, false if the key does not exist
bool memory_get(const char *key, char *value, size_t max_len);

// Deletes the entry for the given key from NVS flash
// If the key does not exist, behaviour depends on the underlying NVS implementation
// Returns ESP_OK on success, or an error code if deletion fails
esp_err_t memory_delete(const char *key);

// Erases ALL persisted key-value pairs in the configured NVS storage partition
// This is a destructive, irreversible operation — equivalent to a factory reset
// Returns ESP_OK on success, or an error code if the erase fails
esp_err_t memory_factory_reset(void);

#endif // MEMORY_H — end of header guard
