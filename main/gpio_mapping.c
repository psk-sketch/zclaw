// Include the module's public interface declarations
#include "gpio_mapping.h"
#ifndef TEST_BUILD
// Include configuration constants
#include "config.h"
// Include key-value memory store public APIs (memory_set, memory_get, memory_delete)
#include "memory.h"
// Include GPIO policy checks (gpio_policy_pin_is_allowed)
#include "gpio_policy.h"
// Include ESP-IDF logging macros (ESP_LOGI, ESP_LOGE, etc.)
#include "esp_log.h"
// Include cJSON for serializing/deserializing the list of mappings
#include "cJSON.h"
// Include FreeRTOS core headers (required before semphr.h)
#include "freertos/FreeRTOS.h"
// Include FreeRTOS semaphores (used for thread-safety locks)
#include "freertos/semphr.h"
// Include mbedtls SHA-256 implementation
#include "mbedtls/sha256.h"
#else
// Fallbacks for host-side unit testing where config.h is not present
#define GPIO_MAPPING_KEY_PREFIX   "u_gpio_"
#define GPIO_MAPPING_MAX_NAME_LEN 16
#endif
// Include standard string functions (strlen, strchr, strcmp, strncpy, etc.)
#include <string.h>
// Include standard I/O (snprintf)
#include <stdio.h>
// Include standard library (free, atoi)
#include <stdlib.h>
// Include character type functions (tolower)
#include <ctype.h>

// Tag used to prefix all console log outputs from this module
static const char *TAG = "gpio_mapping";

#ifdef TEST_BUILD
#define ROTRIGHT(word,bits) (((word) >> (bits)) | ((word) << (32-(bits))))
#define CH(x,y,z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTRIGHT(x,2) ^ ROTRIGHT(x,13) ^ ROTRIGHT(x,22))
#define EP1(x) (ROTRIGHT(x,6) ^ ROTRIGHT(x,11) ^ ROTRIGHT(x,25))
#define SIG0(x) (ROTRIGHT(x,7) ^ ROTRIGHT(x,18) ^ ((x) >> 3))
#define SIG1(x) (ROTRIGHT(x,17) ^ ROTRIGHT(x,19) ^ ((x) >> 10))

typedef struct {
    unsigned char data[64];
    unsigned int datalen;
    unsigned long long bitlen;
    unsigned int state[8];
} MOCK_SHA256_CTX;

static void mock_sha256_transform(MOCK_SHA256_CTX *ctx, const unsigned char data[])
{
    unsigned int a, b, c, d, e, f, g, h, i, j, t1, t2, m[64];

    for (i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = (data[j] << 24) | (data[j + 1] << 16) | (data[j + 2] << 8) | (data[j + 3]);
    for ( ; i < 64; ++i)
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    static const unsigned int k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };

    for (i = 0; i < 64; ++i) {
        t1 = h + EP1(e) + CH(e,f,g) + k[i] + m[i];
        t2 = EP0(a) + MAJ(a,b,c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void mock_sha256_init(MOCK_SHA256_CTX *ctx)
{
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
}

static void mock_sha256_update(MOCK_SHA256_CTX *ctx, const unsigned char data[], size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        ctx->data[ctx->datalen] = data[i];
        ctx->datalen++;
        if (ctx->datalen == 64) {
            mock_sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void mock_sha256_final(MOCK_SHA256_CTX *ctx, unsigned char hash[])
{
    unsigned int i = ctx->datalen;

    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56)
            ctx->data[i++] = 0x00;
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64)
            ctx->data[i++] = 0x00;
        mock_sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }

    ctx->bitlen += ctx->datalen * 8;
    ctx->data[56] = (ctx->bitlen >> 56) & 0xFF;
    ctx->data[57] = (ctx->bitlen >> 48) & 0xFF;
    ctx->data[58] = (ctx->bitlen >> 40) & 0xFF;
    ctx->data[59] = (ctx->bitlen >> 32) & 0xFF;
    ctx->data[60] = (ctx->bitlen >> 24) & 0xFF;
    ctx->data[61] = (ctx->bitlen >> 16) & 0xFF;
    ctx->data[62] = (ctx->bitlen >> 8) & 0xFF;
    ctx->data[63] = (ctx->bitlen) & 0xFF;
    mock_sha256_transform(ctx, ctx->data);

    for (i = 0; i < 4; ++i) {
        hash[i]      = (ctx->state[0] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 4]  = (ctx->state[1] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 8]  = (ctx->state[2] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 12] = (ctx->state[3] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 16] = (ctx->state[4] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 20] = (ctx->state[5] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 24] = (ctx->state[6] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 28] = (ctx->state[7] >> (24 - i * 8)) & 0x000000ff;
    }
}
#endif // TEST_BUILD

static void hash_password_sha256(const char *password, char *hex_out)
{
    unsigned char hash[32];
    const char *pwd = password ? password : "";
#ifdef TEST_BUILD
    MOCK_SHA256_CTX ctx;
    mock_sha256_init(&ctx);
    mock_sha256_update(&ctx, (const unsigned char *)pwd, strlen(pwd));
    mock_sha256_final(&ctx, hash);
#else
    mbedtls_sha256((const unsigned char *)pwd, strlen(pwd), hash, 0);
#endif
    for (int i = 0; i < 32; i++) {
        sprintf(hex_out + (i * 2), "%02x", hash[i]);
    }
    hex_out[64] = '\0';
}

static bool is_authorized(const char *password)
{
    char stored_hash[65] = {0};
    // Fetch stored hash
    if (!memory_get("gpio_pwd_hash", stored_hash, sizeof(stored_hash))) {
        // Optional password protection is inactive if the key is not set
        return true;
    }
    char input_hash[65] = {0};
    hash_password_sha256(password, input_hash);
    return strcmp(input_hash, stored_hash) == 0;
}

// Mutex handle to protect read-modify-write actions on the shared device list key
static SemaphoreHandle_t s_mapping_mutex = NULL;
// Spinlock to guard thread-safe lazy initialization of the mutex
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;

// --------------------------------------------------------------------------
// lock_mapping() - Lazily initializes and acquires the module mutex.
// Returns true on success, false if the acquisition times out or fails.
// --------------------------------------------------------------------------
static bool lock_mapping(void)
{
    // If the mutex does not exist, initialize it safely across multiple threads
    if (s_mapping_mutex == NULL) {
        portENTER_CRITICAL(&s_init_mux);
        if (s_mapping_mutex == NULL) {
            s_mapping_mutex = xSemaphoreCreateMutex();
        }
        portEXIT_CRITICAL(&s_init_mux);
    }
    // Return false if mutex allocation failed entirely
    if (s_mapping_mutex == NULL) {
        return false;
    }
    // Block up to 1000 ms to acquire the mutex
    return xSemaphoreTake(s_mapping_mutex, pdMS_TO_TICKS(1000)) == pdTRUE;
}

// --------------------------------------------------------------------------
// unlock_mapping() - Releases the module mutex if it has been acquired.
// --------------------------------------------------------------------------
static void unlock_mapping(void)
{
    if (s_mapping_mutex != NULL) {
        xSemaphoreGive(s_mapping_mutex);
    }
}

// --------------------------------------------------------------------------
// validate_and_normalize_device() - Checks constraints and converts name to lowercase.
// Reject NULL, empty, or strings exceeding 10 characters.
// Prints warning "Device name count exceeded." on any violation.
// Returns ESP_OK on success, ESP_ERR_INVALID_ARG on validation failure.
// --------------------------------------------------------------------------
static esp_err_t validate_and_normalize_device(const char *device, char *normalized, size_t max_len)
{
    // Reject NULL pointer outright
    if (device == NULL) {
        ESP_LOGW(TAG, "Device name count exceeded.");
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(device);
    // Reject empty device names or names exceeding the 10 character limit
    if (len == 0 || len > 10) {
        ESP_LOGW(TAG, "Device name count exceeded.");
        return ESP_ERR_INVALID_ARG;
    }
    // Ensure the output buffer is large enough for the name and terminator
    if (len >= max_len) {
        ESP_LOGW(TAG, "Device name count exceeded.");
        return ESP_ERR_INVALID_ARG;
    }
    // Convert to lowercase to enforce case-insensitivity
    for (size_t i = 0; i < len; i++) {
        normalized[i] = tolower((unsigned char)device[i]);
    }
    normalized[len] = '\0';
    return ESP_OK;
}

// --------------------------------------------------------------------------
// get_device_key() - Builds the NVS key string: "u_gpio_<normalized_device>"
// Returns ESP_OK on success, ESP_ERR_INVALID_SIZE if the key exceeds NVS limit (15 chars)
// --------------------------------------------------------------------------
static esp_err_t get_device_key(const char *normalized_device, char *key_out, size_t max_len)
{
    // Format the NVS key name with the user prefix and the device name
    int written = snprintf(key_out, max_len, GPIO_MAPPING_KEY_PREFIX "%s", normalized_device);
    // If snprintf truncated or failed, it means the key name is too long for NVS limit (15 chars)
    if (written < 0 || (size_t)written >= max_len) {
        ESP_LOGW(TAG, "NVS key name prefix/device combination is too long");
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

// --------------------------------------------------------------------------
// list_contains_device() - Scans a comma-separated list for a device token.
// Returns true if found, false otherwise.
// --------------------------------------------------------------------------
static bool list_contains_device(const char *list, const char *device)
{
    if (!list || !device) {
        return false;
    }
    const char *ptr = list;
    size_t dev_len = strlen(device);
    // Loop through comma-separated tokens in the list
    while (ptr) {
        const char *next = strchr(ptr, ',');
        size_t token_len = next ? (size_t)(next - ptr) : strlen(ptr);
        // If length and content match, token exists in list
        if (token_len == dev_len && strncmp(ptr, device, dev_len) == 0) {
            return true;
        }
        ptr = next ? (next + 1) : NULL;
    }
    return false;
}

// --------------------------------------------------------------------------
// list_add_device() - Adds a normalized device name to the list in NVS.
// Returns ESP_OK on success, or an error code on failure.
// --------------------------------------------------------------------------
static esp_err_t list_add_device(const char *device)
{
    char list_buf[512] = {0};
    // Retrieve the existing list of device names
    bool exists = memory_get("u_gpio_list", list_buf, sizeof(list_buf));
    if (!exists) {
        // If list doesn't exist, store this device as the first entry
        return memory_set("u_gpio_list", device);
    }
    // If the device is already in the list, no need to add it again
    if (list_contains_device(list_buf, device)) {
        return ESP_OK;
    }
    size_t cur_len = strlen(list_buf);
    size_t dev_len = strlen(device);
    // Check for overflow before appending
    if (cur_len + 1 + dev_len >= sizeof(list_buf)) {
        ESP_LOGE(TAG, "Device list buffer size exceeded");
        return ESP_ERR_NO_MEM;
    }
    // Append a comma and the new device name to the list buffer
    snprintf(list_buf + cur_len, sizeof(list_buf) - cur_len, ",%s", device);
    // Write the updated list back to NVS
    return memory_set("u_gpio_list", list_buf);
}

// --------------------------------------------------------------------------
// list_remove_device() - Removes a device name from the NVS list.
// Returns ESP_OK on success, or an error code on failure.
// --------------------------------------------------------------------------
static esp_err_t list_remove_device(const char *device)
{
    char list_buf[512] = {0};
    // Retrieve the list of devices; if not found, nothing to remove
    if (!memory_get("u_gpio_list", list_buf, sizeof(list_buf))) {
        return ESP_OK;
    }
    // If the device isn't even in the list, we can return success directly
    if (!list_contains_device(list_buf, device)) {
        return ESP_OK;
    }
    char new_list[512] = {0};
    const char *ptr = list_buf;
    size_t dev_len = strlen(device);
    bool first = true;
    // Iterate over tokens and copy all except the target device to the new list
    while (ptr) {
        const char *next = strchr(ptr, ',');
        size_t token_len = next ? (size_t)(next - ptr) : strlen(ptr);
        if (token_len != dev_len || strncmp(ptr, device, dev_len) != 0) {
            if (!first) {
                strncat(new_list, ",", sizeof(new_list) - strlen(new_list) - 1);
            }
            strncat(new_list, ptr, token_len);
            first = false;
        }
        ptr = next ? (next + 1) : NULL;
    }
    // If list is now empty, delete the list key; otherwise write the updated list
    if (strlen(new_list) == 0) {
        return memory_delete("u_gpio_list");
    } else {
        return memory_set("u_gpio_list", new_list);
    }
}

// --------------------------------------------------------------------------
// Public API: gpio_mapping_save()
// --------------------------------------------------------------------------
esp_err_t gpio_mapping_save(const char *device, int pin, const char *password)
{
    char normalized[GPIO_MAPPING_MAX_NAME_LEN] = {0};
    // Validate device name length and normalize it to lowercase
    esp_err_t err = validate_and_normalize_device(device, normalized, sizeof(normalized));
    if (err != ESP_OK) {
        return err;
    }
    // Validate that the pin is permitted by the policy configuration
    if (!gpio_policy_pin_is_allowed(pin)) {
        ESP_LOGE(TAG, "GPIO pin %d is not allowed by policy", pin);
        return ESP_ERR_INVALID_ARG;
    }
    char key[GPIO_MAPPING_MAX_NAME_LEN] = {0};
    // Generate the NVS key name; will fail if key name exceeds NVS key limit (15 chars)
    err = get_device_key(normalized, key, sizeof(key));
    if (err != ESP_OK) {
        return err;
    }
    // Convert pin integer to string for storing
    char pin_str[16];
    snprintf(pin_str, sizeof(pin_str), "%d", pin);

    // Acquire lock before modifications to ensure list consistency
    if (!lock_mapping()) {
        return ESP_ERR_TIMEOUT;
    }

    // Verify authentication if password protection is set
    if (!is_authorized(password)) {
        ESP_LOGE(TAG, "Authentication failed");
        unlock_mapping();
        return ESP_ERR_INVALID_ARG;
    }

    // Check if the pin is already mapped to another device
    char list_buf[512] = {0};
    if (memory_get("u_gpio_list", list_buf, sizeof(list_buf))) {
        char list_copy[512];
        strncpy(list_copy, list_buf, sizeof(list_copy) - 1);
        list_copy[sizeof(list_copy) - 1] = '\0';

        char *ptr = list_copy;
        while (ptr) {
            char *next = strchr(ptr, ',');
            if (next) {
                *next = '\0';
            }

            // Skip checking the current device itself (which is allowed to change its pin)
            if (strcmp(ptr, normalized) != 0) {
                char dev_key[16] = {0};
                if (get_device_key(ptr, dev_key, sizeof(dev_key)) == ESP_OK) {
                    char mapped_pin_str[16] = {0};
                    if (memory_get(dev_key, mapped_pin_str, sizeof(mapped_pin_str))) {
                        int mapped_pin = atoi(mapped_pin_str);
                        if (mapped_pin == pin) {
                            ESP_LOGE(TAG, "GPIO pin %d is already mapped to device '%s'", pin, ptr);
                            unlock_mapping();
                            return ESP_ERR_INVALID_ARG;
                        }
                    }
                }
            }

            ptr = next ? (next + 1) : NULL;
        }
    }

    // Check if the mapping already exists for the current device
    char temp_pin_str[16] = {0};
    bool already_exists = memory_get(key, temp_pin_str, sizeof(temp_pin_str));

    // Save pin mapping to NVS and update the device list metadata
    err = memory_set(key, pin_str);
    if (err == ESP_OK) {
        err = list_add_device(normalized);
    }

    // Log warning if updating an existing mapping
    if (err == ESP_OK && already_exists) {
        ESP_LOGW(TAG, "Mappings have been updated");
    }

    // Release lock under all circumstances
    unlock_mapping();
    return err;
}

// --------------------------------------------------------------------------
// Public API: gpio_mapping_get_pin()
// --------------------------------------------------------------------------
bool gpio_mapping_get_pin(const char *device, int *pin)
{
    if (!pin) {
        return false;
    }
    char normalized[GPIO_MAPPING_MAX_NAME_LEN] = {0};
    // Validate and normalize name; returns false on failure
    if (validate_and_normalize_device(device, normalized, sizeof(normalized)) != ESP_OK) {
        return false;
    }
    char key[GPIO_MAPPING_MAX_NAME_LEN] = {0};
    // Generate key name; returns false if key name exceeds NVS key limit (15 chars)
    if (get_device_key(normalized, key, sizeof(key)) != ESP_OK) {
        return false;
    }
    char pin_str[16] = {0};
    // Fetch stored pin string from NVS; returns false if key does not exist
    if (!memory_get(key, pin_str, sizeof(pin_str))) {
        return false;
    }
    // Parse integer pin and store in output pointer
    *pin = atoi(pin_str);
    return true;
}

// --------------------------------------------------------------------------
// Public API: gpio_mapping_delete()
// --------------------------------------------------------------------------
esp_err_t gpio_mapping_delete(const char *device, const char *password)
{
    char normalized[GPIO_MAPPING_MAX_NAME_LEN] = {0};
    // Validate and normalize device name
    esp_err_t err = validate_and_normalize_device(device, normalized, sizeof(normalized));
    if (err != ESP_OK) {
        return err;
    }
    char key[GPIO_MAPPING_MAX_NAME_LEN] = {0};
    // Generate key name
    err = get_device_key(normalized, key, sizeof(key));
    if (err != ESP_OK) {
        return err;
    }

    // Acquire lock before editing list and keys
    if (!lock_mapping()) {
        return ESP_ERR_TIMEOUT;
    }

    // Verify authentication if password protection is set
    if (!is_authorized(password)) {
        ESP_LOGE(TAG, "Authentication failed");
        unlock_mapping();
        return ESP_ERR_INVALID_ARG;
    }

    // Delete mapping from NVS and remove device name from metadata list
    err = memory_delete(key);
    if (err == ESP_OK || err == ESP_ERR_NOT_FOUND) {
        esp_err_t list_err = list_remove_device(normalized);
        if (err == ESP_OK) {
            err = list_err;
        }
    }

    // Release lock
    unlock_mapping();
    return err;
}

// --------------------------------------------------------------------------
// Public API: gpio_mapping_exists()
// --------------------------------------------------------------------------
bool gpio_mapping_exists(const char *device)
{
    char normalized[GPIO_MAPPING_MAX_NAME_LEN] = {0};
    // Validate and normalize device name
    if (validate_and_normalize_device(device, normalized, sizeof(normalized)) != ESP_OK) {
        return false;
    }
    char key[GPIO_MAPPING_MAX_NAME_LEN] = {0};
    // Generate key name
    if (get_device_key(normalized, key, sizeof(key)) != ESP_OK) {
        return false;
    }
    char pin_str[16] = {0};
    // Return true if key exists in memory, false otherwise
    return memory_get(key, pin_str, sizeof(pin_str));
}

// --------------------------------------------------------------------------
// Public API: gpio_mapping_list()
// --------------------------------------------------------------------------
esp_err_t gpio_mapping_list(char *result, size_t result_len)
{
    if (!result || result_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Acquire mutex lock to ensure list data integrity
    if (!lock_mapping()) {
        return ESP_ERR_TIMEOUT;
    }

    char list_buf[512] = {0};
    // Read list of device names from NVS
    bool has_list = memory_get("u_gpio_list", list_buf, sizeof(list_buf));
    if (!has_list || strlen(list_buf) == 0) {
        unlock_mapping();
        snprintf(result, result_len, "[]");
        return ESP_OK;
    }

    // Allocate cJSON root array
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        unlock_mapping();
        return ESP_ERR_NO_MEM;
    }

    char list_copy[512];
    strncpy(list_copy, list_buf, sizeof(list_copy) - 1);
    list_copy[sizeof(list_copy) - 1] = '\0';

    char *ptr = list_copy;
    bool oom = false;
    // Iterate over each device name in the list copy
    while (ptr) {
        char *next = strchr(ptr, ',');
        if (next) {
            *next = '\0';
        }

        char device[GPIO_MAPPING_MAX_NAME_LEN];
        strncpy(device, ptr, sizeof(device) - 1);
        device[sizeof(device) - 1] = '\0';

        char key[GPIO_MAPPING_MAX_NAME_LEN] = {0};
        // Retrieve and add each active device mapping to the JSON array
        if (get_device_key(device, key, sizeof(key)) == ESP_OK) {
            char pin_str[16] = {0};
            if (memory_get(key, pin_str, sizeof(pin_str))) {
                cJSON *obj = cJSON_CreateObject();
                if (obj) {
                    cJSON_AddStringToObject(obj, "device", device);
                    cJSON_AddNumberToObject(obj, "pin", atoi(pin_str));
                    cJSON_AddItemToArray(arr, obj);
                } else {
                    oom = true;
                    break;
                }
            }
        }

        ptr = next ? (next + 1) : NULL;
    }

    // Unlock mutex after finishing NVS reads
    unlock_mapping();

    if (oom) {
        cJSON_Delete(arr);
        return ESP_ERR_NO_MEM;
    }

    // Serialize JSON array to string
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    // Ensure output buffer is large enough for the serialized JSON string
    if (strlen(json) >= result_len) {
        free(json);
        return ESP_ERR_INVALID_SIZE;
    }

    // Copy to output buffer and free serialized memory
    strncpy(result, json, result_len - 1);
    result[result_len - 1] = '\0';
    free(json);

    return ESP_OK;
}

// --------------------------------------------------------------------------
// Public API: gpio_mapping_set_password()
// --------------------------------------------------------------------------
esp_err_t gpio_mapping_set_password(const char *new_password, const char *current_password)
{
    // Acquire lock before modifications to ensure list consistency
    if (!lock_mapping()) {
        return ESP_ERR_TIMEOUT;
    }

    if (!is_authorized(current_password)) {
        ESP_LOGE(TAG, "Authentication failed");
        unlock_mapping();
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err;
    if (new_password == NULL || strlen(new_password) == 0) {
        // Clear/delete password
        err = memory_delete("gpio_pwd_hash");
        if (err == ESP_ERR_NOT_FOUND) {
            err = ESP_OK; // Deleting a non-existent password is a no-op success
        }
    } else {
        char hex_out[65];
        hash_password_sha256(new_password, hex_out);
        err = memory_set("gpio_pwd_hash", hex_out);
    }

    unlock_mapping();
    return err;
}
