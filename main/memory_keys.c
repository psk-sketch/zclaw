#include "memory_keys.h"    // Include our own header (declares the functions we're implementing)
#include "nvs_keys.h"       // Include the NVS key name macros (e.g. NVS_KEY_API_KEY)
#include <string.h>         // Standard library: gives us strcmp(), strncmp(), strlen()

// --- Implementation of memory_keys_is_user_key ---
bool memory_keys_is_user_key(const char *key)
{
    if (!key) {             // Null-check: if key is NULL, it can't be a valid user key
        return false;
    }
    size_t prefix_len = strlen(USER_MEMORY_KEY_PREFIX); // Get the length of "u_" → 2
    // strncmp compares only the first 'prefix_len' characters of key against the prefix.
    // Returns 0 if they match, so == 0 means the key starts with "u_".
    return strncmp(key, USER_MEMORY_KEY_PREFIX, prefix_len) == 0;
}

// --- Implementation of memory_keys_is_sensitive ---
bool memory_keys_is_sensitive(const char *key)
{
    // A local array of string pointers listing every sensitive NVS key.
    // Terminated by NULL so we can loop without knowing the array length upfront.
    const char *sensitive[] = {
        NVS_KEY_API_KEY,        // "api_key"    — LLM provider secret
        NVS_KEY_TG_TOKEN,       // "tg_token"   — Telegram bot token
        NVS_KEY_TG_CHAT_ID,     // "tg_chat_id" — Telegram chat identifier
        NVS_KEY_TG_CHAT_IDS,    // "tg_chat_ids"— Multiple chat IDs
        NVS_KEY_WIFI_PASS,      // "wifi_pass"  — Wi-Fi password
        NVS_KEY_LLM_BACKEND,    // "llm_backend"— Which LLM backend is configured
        NVS_KEY_LLM_MODEL,      // "llm_model"  — Which model is selected
        NVS_KEY_LLM_API_URL,    // "llm_api_url"— The API endpoint
        NVS_KEY_WIFI_SSID,      // "wifi_ssid"  — Wi-Fi network name
        NVS_KEY_GPIO_PWD_HASH,  // "gpio_pwd_hash" — SHA-256 hash of password
        NULL                    // Sentinel value: marks the end of the array
    };

    if (!key) {             // Null-check: a NULL key is not sensitive (avoids a crash)
        return false;
    }

    for (int i = 0; sensitive[i] != NULL; i++) { // Walk the array until we hit NULL
        if (strcmp(key, sensitive[i]) == 0) {    // Full string comparison (not prefix)
            return true;    // Exact match found → this key is sensitive
        }
    }
    return false;           // No match found → key is not in the sensitive list
}
