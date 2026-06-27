// Include the public memory API header (memory_init, memory_get, memory_set, etc.)
#include "memory.h"
// Project-wide configuration constants (NVS_NAMESPACE and feature flags)
#include "config.h"
// Security helpers — used to redact sensitive keys like WiFi passwords from logs
#include "security.h"
// ESP-IDF NVS flash initialisation API (nvs_flash_init, nvs_flash_erase, etc.)
#include "nvs_flash.h"
// ESP-IDF NVS read/write API (nvs_open, nvs_set_str, nvs_get_str, nvs_commit, etc.)
#include "nvs.h"
// ESP-IDF logging macros (ESP_LOGI, ESP_LOGE, ESP_LOGW)
#include "esp_log.h"
// Flash encryption detection — esp_flash_encryption_enabled()
#include "esp_flash_encrypt.h"
// Flash partition API — used to locate the NVS keys partition for encrypted NVS
#include "esp_partition.h"

// Tag used to prefix all log output from this file (appears as "[memory]" in console)
static const char *TAG = "memory";

// Returns the string to log for a given key's value
// If the key is marked as sensitive (e.g. passwords, tokens), returns "<redacted>" instead
// Prevents secrets from appearing in serial log output
static const char *log_value_for_key(const char *key, const char *value)
{
    if (security_key_is_sensitive(key)) {
        return "<redacted>";
    }
    // If value is NULL, return an empty string to avoid logging a null pointer
    return value ? value : "";
}

// Attempts to initialise NVS with hardware flash encryption enabled
// Locates the dedicated NVS keys partition, reads or generates encryption keys,
// then initialises NVS in secure (encrypted) mode
static esp_err_t init_encrypted_nvs(void)
{
    // Search the partition table for the NVS keys partition
    // This is a special partition that stores the AES keys used to encrypt NVS data
    const esp_partition_t *nvs_key_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS, NULL);

    // If no NVS keys partition exists in the partition table, encrypted NVS is not possible
    if (!nvs_key_part) {
        ESP_LOGW(TAG, "NVS keys partition not found, using unencrypted NVS");
        return ESP_ERR_NOT_FOUND;
    }

    // Structure to hold the NVS encryption key material read from or written to the partition
    nvs_sec_cfg_t nvs_sec_cfg;
    // Try to read existing encryption keys from the NVS keys partition
    esp_err_t err = nvs_flash_read_security_cfg(nvs_key_part, &nvs_sec_cfg);

    // If the keys partition exists but has never been written to, generate fresh keys now
    if (err == ESP_ERR_NVS_KEYS_NOT_INITIALIZED) {
        ESP_LOGI(TAG, "Generating NVS encryption keys");
        // Generate and write new AES encryption keys into the NVS keys partition
        err = nvs_flash_generate_keys(nvs_key_part, &nvs_sec_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to generate NVS keys: %s", esp_err_to_name(err));
            return err;
        }
    } else if (err != ESP_OK) {
        // Any other error reading the security config is unexpected — bail out
        ESP_LOGE(TAG, "Failed to read NVS security cfg: %s", esp_err_to_name(err));
        return err;
    }

    // Initialise the NVS subsystem in encrypted mode using the loaded/generated keys
    err = nvs_flash_secure_init(&nvs_sec_cfg);

    // If NVS flash has no free pages or the format version has changed, erase and retry
    // This can happen after a firmware update that changes the NVS layout
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash");
        // Erase the entire NVS partition — all stored data is lost
        ESP_ERROR_CHECK(nvs_flash_erase());
        // Retry secure init after erasing
        err = nvs_flash_secure_init(&nvs_sec_cfg);
    }

    return err;
}

// Public function: initialises NVS flash storage
// Automatically selects encrypted or unencrypted NVS based on whether flash encryption is active
esp_err_t memory_init(void)
{
    esp_err_t err;

    // Check whether the ESP32's hardware flash encryption feature is active
    if (esp_flash_encryption_enabled()) {
        ESP_LOGI(TAG, "Flash encryption enabled, using encrypted NVS");
        err = init_encrypted_nvs();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Encrypted NVS initialized");
            return ESP_OK;
        }

        // Encrypted NVS init failed — check if we're allowed to fall back to unencrypted
#if CONFIG_ZCLAW_ALLOW_UNENCRYPTED_NVS_FALLBACK
        // Development-only escape hatch: allows unencrypted NVS even when flash encryption is on
        // Should never be enabled in production builds
        ESP_LOGW(TAG, "Encrypted NVS init failed, falling back to unencrypted (override enabled)");
#else
        // Production behaviour: refuse to fall back to unencrypted NVS while encryption is active
        // Falling back would expose sensitive data (WiFi passwords, tokens) in plaintext flash
        ESP_LOGE(TAG, "Encrypted NVS init failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "Refusing unencrypted NVS fallback while flash encryption is active");
        return err != ESP_OK ? err : ESP_FAIL;
#endif
    }

    // Standard unencrypted NVS initialisation (used when flash encryption is not enabled)
    err = nvs_flash_init();

    // If NVS has no free pages or needs reformatting, erase and reinitialise
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "NVS initialized");
    }
    return err;
}

// Stores a string value under the given key in the NVS namespace
// Opens NVS, writes the string, commits the change, then closes the handle
esp_err_t memory_set(const char *key, const char *value)
{
    // NVS handle — an opaque reference to the open NVS namespace
    nvs_handle_t handle;
    esp_err_t err;

    // Open the NVS namespace in read-write mode
    // NVS_NAMESPACE is defined in config.h (e.g. "zclaw")
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    // Write the string value into NVS under the given key
    err = nvs_set_str(handle, key, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set '%s': %s", key, esp_err_to_name(err));
        // Close the handle before returning — always clean up
        nvs_close(handle);
        return err;
    }

    // Commit the write to flash — without this the write is not persisted across reboots
    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit: %s", esp_err_to_name(err));
    } else {
        // Log the stored value, redacting it if the key is sensitive
        ESP_LOGI(TAG, "Stored: %s = %s", key, log_value_for_key(key, value));
    }

    // Close the NVS handle to release the resource
    nvs_close(handle);
    return err;
}

// Retrieves the string value stored under the given key from NVS
// Returns true if found and written to the output buffer, false if the key doesn't exist
bool memory_get(const char *key, char *value, size_t max_len)
{
    nvs_handle_t handle;
    esp_err_t err;

    // Open the NVS namespace in read-only mode — no writes will be made
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        // Namespace doesn't exist yet or NVS is not initialised
        return false;
    }

    // required_size is used both as input (max buffer size) and output (actual string length)
    size_t required_size = max_len;
    // Read the string from NVS into the caller-provided buffer
    err = nvs_get_str(handle, key, value, &required_size);
    // Close the handle immediately — no need to keep it open after the read
    nvs_close(handle);

    if (err == ESP_OK) {
        // Log the retrieved value, redacting sensitive keys
        ESP_LOGI(TAG, "Retrieved: %s = %s", key, log_value_for_key(key, value));
        return true;
    }
    // Key not found or read error — return false without writing to the buffer
    return false;
}

// Deletes the entry for the given key from NVS and commits the change
esp_err_t memory_delete(const char *key)
{
    nvs_handle_t handle;
    esp_err_t err;

    // Open NVS in read-write mode so we can erase a key
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    // Erase the key from the NVS namespace
    err = nvs_erase_key(handle, key);
    if (err == ESP_OK) {
        // Commit the deletion to flash so it survives a reboot
        esp_err_t commit_err = nvs_commit(handle);
        if (commit_err == ESP_OK) {
            ESP_LOGI(TAG, "Deleted: %s", key);
        } else {
            ESP_LOGE(TAG, "Failed to commit delete '%s': %s", key, esp_err_to_name(commit_err));
            // Propagate the commit error as the final result
            err = commit_err;
        }
    }

    // Close the handle regardless of success or failure
    nvs_close(handle);
    return err;
}

// Erases the entire NVS flash partition — destroys all persisted key-value data
// Equivalent to a factory reset; irreversible without re-provisioning the device
esp_err_t memory_factory_reset(void)
{
    // Erase the entire NVS partition (all namespaces and keys)
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Factory reset erase failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGW(TAG, "Factory reset: erased NVS storage");
    return ESP_OK;
}
