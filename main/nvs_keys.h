#ifndef NVS_KEYS_H          // Include guard: if NVS_KEYS_H is not yet defined...
#define NVS_KEYS_H          // ...define it now. Prevents this file from being included twice.

// System/configuration keys stored in NVS namespace "zclaw".
// NVS = Non-Volatile Storage (ESP-IDF's flash storage system, like a key-value database).
// Each macro is a string literal that acts as a named key in that database.

#define NVS_KEY_BOOT_COUNT   "boot_count"   // Tracks how many times the device has booted
#define NVS_KEY_WIFI_SSID    "wifi_ssid"    // The Wi-Fi network name to connect to
#define NVS_KEY_WIFI_PASS    "wifi_pass"    // The Wi-Fi password
#define NVS_KEY_LLM_BACKEND  "llm_backend"  // Which LLM provider to use (e.g. OpenAI, Ollama)
#define NVS_KEY_API_KEY      "api_key"      // The API key for the LLM provider
#define NVS_KEY_LLM_MODEL    "llm_model"    // Which model to use (e.g. "gpt-4o")
#define NVS_KEY_LLM_API_URL  "llm_api_url"  // The API endpoint URL for the LLM
#define NVS_KEY_TG_TOKEN     "tg_token"     // Telegram bot token for sending messages
#define NVS_KEY_TG_CHAT_ID   "tg_chat_id"   // A single Telegram chat ID to message
#define NVS_KEY_TG_CHAT_IDS  "tg_chat_ids"  // Multiple Telegram chat IDs (e.g. comma-separated)
#define NVS_KEY_TIMEZONE     "timezone"     // The device's timezone string (e.g. "Asia/Kolkata")
#define NVS_KEY_PERSONA      "persona"      // The LLM system prompt / personality to use
#define NVS_KEY_GPIO_PWD_HASH "gpio_pwd_hash" // SHA-256 hash of password protecting GPIO mappings

// Rate-limit bookkeeping keys.
// These track how many LLM requests have been made to enforce daily limits.

#define NVS_KEY_RL_DAILY     "rl_daily"     // The daily request limit count
#define NVS_KEY_RL_DAY       "rl_day"       // The current day of month (to detect day rollover)
#define NVS_KEY_RL_YEAR      "rl_year"      // The current year (used together with rl_day)

#endif // NVS_KEYS_H        // End of the include guard
