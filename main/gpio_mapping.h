// Header guard: prevents this file from being included more than once during compilation
#ifndef GPIO_MAPPING_H
// Defines the guard macro so subsequent includes are skipped
#define GPIO_MAPPING_H

// Include ESP-IDF's error type (esp_err_t) used as return values to signal success or failure
#ifndef TEST_BUILD
#include "esp_err.h"
#else
typedef int esp_err_t;
#define ESP_OK 0
#endif
// Include standard bool type (true/false) for functions returning true/false
#include <stdbool.h>
// Include size_t type for buffer lengths
#include <stddef.h>

// Saves a persistent GPIO pin mapping for a device in persistent storage.
// If password protection is configured, requires a valid password.
// Normalizes the device name to lowercase, checks its length (1 to 10 characters),
// validates the GPIO pin via policy, and stores it in NVS.
// device   : the case-insensitive name of the device (e.g., "led", "relay")
// pin      : the GPIO pin number to be mapped
// password : the optional password for authentication (pass NULL if none)
// Returns ESP_OK on success, or an error code if validation, auth, or write fails
esp_err_t gpio_mapping_save(const char *device, int pin, const char *password);

// Retrieves the persistent GPIO pin mapping for a given device name
// Normalizes the device name to lowercase before checking
// device : the case-insensitive name of the device to lookup
// pin    : output pointer where the mapped pin number will be stored
// Returns true if the mapping was found and read, false otherwise
bool gpio_mapping_get_pin(const char *device, int *pin);

// Deletes the persistent GPIO mapping for a given device name
// If password protection is configured, requires a valid password.
// Normalizes the device name to lowercase, deletes the NVS entry, and updates the list
// device   : the case-insensitive name of the device to delete
// password : the optional password for authentication (pass NULL if none)
// Returns ESP_OK on success, or an error code if deletion or auth fails
esp_err_t gpio_mapping_delete(const char *device, const char *password);

// Sets, updates, or clears the password protection for GPIO mapping modifications.
// new_password     : the new password to set (or NULL/empty string to disable password protection)
// current_password : the current password for authentication (ignored if no password is set yet)
// Returns ESP_OK on success, or an error code on authentication or NVS storage failure
esp_err_t gpio_mapping_set_password(const char *new_password, const char *current_password);

// Checks if a persistent GPIO mapping exists for a given device name
// Normalizes the device name to lowercase before checking
// device : the case-insensitive name of the device
// Returns true if the mapping exists in persistent storage, false otherwise
bool gpio_mapping_exists(const char *device);

// Lists all registered persistent GPIO mappings as a JSON array string
// Writes the JSON array string into the caller-provided result buffer
// result     : output buffer to write the JSON string
// result_len : maximum size of the output buffer
// Returns ESP_OK on success, or an error code on failure (e.g., buffer too small)
esp_err_t gpio_mapping_list(char *result, size_t result_len);

#endif // GPIO_MAPPING_H
