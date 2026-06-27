#ifndef MEMORY_KEYS_H       // Include guard: prevents this header from being processed twice
#define MEMORY_KEYS_H

#include <stdbool.h>        // Brings in the 'bool', 'true', and 'false' types (standard C99+)

// Prefix that all user-scoped memory keys must begin with.
// This namespaces user data away from system data in NVS.
#define USER_MEMORY_KEY_PREFIX "u_"

// Checks whether a given NVS key is a valid user-scoped key.
// A key is user-scoped if it starts with the "u_" prefix.
// Tools (LLM-callable functions) should only read/write user keys.
bool memory_keys_is_user_key(const char *key);

// Checks whether a given NVS key is sensitive (system-level secret).
// Tools must NEVER read or modify keys that return true here.
// This protects credentials like API keys, Wi-Fi passwords, and Telegram tokens.
bool memory_keys_is_sensitive(const char *key);

#endif // MEMORY_KEYS_H     // End of include guard
