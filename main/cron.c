/* Public API for the cron/scheduler subsystem (cron_init, cron_set, etc.) */
#include "cron.h"
/* Project-wide configuration constants (NTP server, stack sizes, limits, etc.) */
#include "config.h"
/* Helper utilities for cron: ID generation, interval validation, time formatting */
#include "cron_utils.h"
/* NVS-backed key-value memory store (memory_get / memory_set) */
#include "memory.h"
/* channel_msg_t definition and MSG_SOURCE_CRON constant used to push actions
   to the agent input queue */
#include "messages.h"
/* Centralised NVS key string constants (e.g. NVS_KEY_TIMEZONE, NVS_NAMESPACE_CRON) */
#include "nvs_keys.h"
/* ESP-IDF logging macros: ESP_LOGI, ESP_LOGW, ESP_LOGE */
#include "esp_log.h"
/* SNTP / NTP client initialisation and configuration */
#include "esp_netif_sntp.h"
/* FreeRTOS core types and task scheduler */
#include "freertos/FreeRTOS.h"
/* FreeRTOS task creation and delay (xTaskCreate, vTaskDelay) */
#include "freertos/task.h"
/* FreeRTOS queue API (QueueHandle_t, xQueueSend) */
#include "freertos/queue.h"
/* FreeRTOS mutex/semaphore API (xSemaphoreCreateMutex, xSemaphoreTake, etc.) */
#include "freertos/semphr.h"
/* NVS flash initialisation */
#include "nvs_flash.h"
/* NVS read/write API (nvs_open, nvs_get_blob, nvs_set_blob, nvs_commit, etc.) */
#include "nvs.h"
/* Lightweight JSON parser/builder used to serialise cron_list output */
#include "cJSON.h"
/* Standard string operations (strcmp, strncpy, memset, strlen) */
#include <string.h>
/* Standard I/O (snprintf) */
#include <stdio.h>
/* Standard library (free) */
#include <stdlib.h>
/* POSIX time types (time_t, struct tm, time(), localtime_r()) */
#include <time.h>
/* gettimeofday / struct timeval (used by the NTP sync callback) */
#include <sys/time.h>

/* Module log tag — appears as the prefix in all ESP_LOGx output from this file */
static const char *TAG = "cron";

/* Handle to the agent's input queue. Cron fires are dispatched here so the
   agent processes them like any other incoming message. Set by cron_start(). */
static QueueHandle_t s_agent_queue;

/* In-RAM array of all cron schedule entries. Entries with id == 0 are empty
   slots. Persisted to / loaded from NVS on changes and at boot. */
static cron_entry_t s_entries[CRON_MAX_ENTRIES];

/* Set to true by the NTP sync callback once wall-clock time is available.
   Daily schedules are skipped until this flag is set. */
static bool s_time_synced = false;

/* Mutex protecting s_entries[] from concurrent access between the cron task
   (which reads/fires entries) and the tool handlers (which add/delete entries). */
static SemaphoreHandle_t s_entries_mutex = NULL;

/* Currently active POSIX TZ string, e.g. "IST-5:30".
   Initialised to the compile-time default; updated by set_timezone tool or NVS. */
static char s_timezone[TIMEZONE_MAX_LEN] = DEFAULT_TIMEZONE_POSIX;

/*
 * pending_cron_fire_t — lightweight struct that captures the id and action
 * string of an entry that is about to fire. Used to decouple the "decide what
 * should fire" step (done under the mutex) from the "send to queue" step
 * (done after releasing the mutex, to avoid holding the lock during a
 * potentially blocking queue send).
 */
typedef struct {
    uint8_t id;                          /* ID of the entry that fired */
    char action[CRON_MAX_ACTION_LEN];    /* Copy of the action string to execute */
} pending_cron_fire_t;

/* Static buffer for pending fires. Declared at file scope (not on the cron task
   stack) to avoid stack overflow on memory-constrained targets like the ESP32-C6
   where CRON_TASK_STACK_SIZE is kept small. */
static pending_cron_fire_t s_pending_fires[CRON_MAX_ENTRIES];

/* --------------------------------------------------------------------------
 * entries_lock() — acquires the entries mutex within the given FreeRTOS tick
 * timeout. Returns true on success, false if the mutex does not yet exist or
 * the timeout expires. All callers must check the return value before
 * accessing s_entries[].
 * -------------------------------------------------------------------------- */
static bool entries_lock(TickType_t timeout_ticks)
{
    /* Guard against being called before cron_init() creates the mutex */
    if (!s_entries_mutex) {
        return false;
    }
    /* Block until the mutex is acquired or the timeout expires */
    return xSemaphoreTake(s_entries_mutex, timeout_ticks) == pdTRUE;
}

/* --------------------------------------------------------------------------
 * entries_unlock() — releases the entries mutex. Safe to call even if the
 * mutex handle is NULL (e.g. during early-boot error paths).
 * -------------------------------------------------------------------------- */
static void entries_unlock(void)
{
    if (s_entries_mutex) {
        xSemaphoreGive(s_entries_mutex);
    }
}

/* --------------------------------------------------------------------------
 * timezone_string_is_valid() — lightweight sanity-check for a POSIX TZ string.
 * Rejects NULL, empty strings, strings that exceed TIMEZONE_MAX_LEN, and any
 * string containing ASCII control characters or DEL (0x7F), which would be
 * invalid in a POSIX TZ value and could corrupt the NVS or setenv() call.
 * Returns true only if the string passes all checks.
 * -------------------------------------------------------------------------- */
static bool timezone_string_is_valid(const char *tz)
{
    /* Reject NULL pointer outright */
    if (!tz) {
        return false;
    }

    size_t len = strlen(tz);
    /* Reject empty string or a string too long for our buffer */
    if (len == 0 || len >= TIMEZONE_MAX_LEN) {
        return false;
    }

    /* Scan every character for control codes (< 0x20) or DEL (0x7F) */
    for (size_t i = 0; i < len; i++) {
        char c = tz[i];
        if (c < 0x20 || c == 0x7f) {
            return false;
        }
    }

    return true;
}

/* --------------------------------------------------------------------------
 * apply_timezone() — validates, activates, and optionally persists a POSIX
 * TZ string.
 *
 *   1. Validates the string with timezone_string_is_valid().
 *   2. Calls setenv("TZ", ...) + tzset() so that all subsequent localtime_r()
 *      calls use the new timezone.
 *   3. Copies the string into the module-global s_timezone buffer.
 *   4. If persist_to_nvs is true, writes the string to NVS via memory_set()
 *      so it survives a reboot.
 *
 * Returns ESP_OK on success, or an appropriate esp_err_t code on failure.
 * -------------------------------------------------------------------------- */
static esp_err_t apply_timezone(const char *timezone_posix, bool persist_to_nvs)
{
    /* Validate before touching any system state */
    if (!timezone_string_is_valid(timezone_posix)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Set the TZ environment variable; overwrite=1 replaces any existing value */
    if (setenv("TZ", timezone_posix, 1) != 0) {
        ESP_LOGE(TAG, "Failed to set TZ environment");
        return ESP_FAIL;
    }
    /* tzset() re-reads TZ and updates the C library's internal timezone state */
    tzset();

    /* Keep our module-level copy in sync; ensure null termination */
    strncpy(s_timezone, timezone_posix, sizeof(s_timezone) - 1);
    s_timezone[sizeof(s_timezone) - 1] = '\0';
    ESP_LOGI(TAG, "Timezone applied: %s", s_timezone);

    /* Optionally persist to NVS so the timezone survives a reboot */
    if (persist_to_nvs) {
        esp_err_t err = memory_set(NVS_KEY_TIMEZONE, s_timezone);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to persist timezone: %s", esp_err_to_name(err));
            return err;
        }
    }

    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * load_timezone_from_nvs() — called once at boot to restore the previously
 * saved timezone. If NVS contains a valid TZ string, it is applied without
 * re-persisting (persist_to_nvs = false). If loading or applying fails, the
 * compile-time DEFAULT_TIMEZONE_POSIX is applied as a fallback. If even the
 * fallback apply_timezone() call fails (extremely unlikely), the s_timezone
 * buffer is set manually to avoid it being empty.
 * -------------------------------------------------------------------------- */
static void load_timezone_from_nvs(void)
{
    char stored_tz[TIMEZONE_MAX_LEN] = {0};
    /* Try to read the stored value and apply it without writing back to NVS */
    if (memory_get(NVS_KEY_TIMEZONE, stored_tz, sizeof(stored_tz)) &&
        timezone_string_is_valid(stored_tz)) {
        if (apply_timezone(stored_tz, false) == ESP_OK) {
            return; /* Successfully restored from NVS */
        }
    }

    /* Fall back to the compile-time default timezone */
    ESP_LOGI(TAG, "Using default timezone: %s", DEFAULT_TIMEZONE_POSIX);
    if (apply_timezone(DEFAULT_TIMEZONE_POSIX, false) != ESP_OK) {
        /* apply_timezone failed (e.g. setenv not available) — set the buffer
           directly so s_timezone is at least consistent with the default */
        strncpy(s_timezone, DEFAULT_TIMEZONE_POSIX, sizeof(s_timezone) - 1);
        s_timezone[sizeof(s_timezone) - 1] = '\0';
    }
}

/* --------------------------------------------------------------------------
 * time_sync_notification_cb() — NTP sync callback registered with the SNTP
 * driver. Invoked by the SNTP task once the system clock has been set.
 * Sets s_time_synced = true, which unblocks daily schedule evaluation and
 * the cron_init() NTP-wait loop.
 * The tv parameter (actual synced time) is unused here.
 * -------------------------------------------------------------------------- */
static void time_sync_notification_cb(struct timeval *tv)
{
    (void)tv; /* Suppress unused-parameter warning */
    ESP_LOGI(TAG, "NTP time synchronized");
    s_time_synced = true;
}

/* --------------------------------------------------------------------------
 * init_sntp() — configures and starts the ESP-IDF SNTP client using the
 * NTP_SERVER address from config.h. Registers time_sync_notification_cb so
 * we are notified when the clock is first set.
 * -------------------------------------------------------------------------- */
static void init_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");

    /* Build the SNTP config from the macro-provided server address */
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
    /* Register our callback so cron knows when clock is valid */
    config.sync_cb = time_sync_notification_cb;
    /* Start the SNTP service; it will sync in the background */
    esp_netif_sntp_init(&config);
}

/* --------------------------------------------------------------------------
 * load_entries() — reads all cron entries from NVS into s_entries[] at boot.
 * Keys follow the pattern "cron_0", "cron_1", ... up to CRON_MAX_ENTRIES.
 * If a key is missing or the blob read fails, the slot is zeroed (id = 0
 * marks it as empty). NVS is opened read-only; no changes are written.
 * -------------------------------------------------------------------------- */
static void load_entries(void)
{
    nvs_handle_t handle;
    /* Open the cron NVS namespace read-only; bail out silently if unavailable */
    if (nvs_open(NVS_NAMESPACE_CRON, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    for (int i = 0; i < CRON_MAX_ENTRIES; i++) {
        char key[16];
        /* Build the per-slot key string, e.g. "cron_2" */
        snprintf(key, sizeof(key), "cron_%d", i);

        size_t size = sizeof(cron_entry_t);
        /* Attempt to read the blob; on failure, zero the id to mark slot empty */
        if (nvs_get_blob(handle, key, &s_entries[i], &size) != ESP_OK) {
            s_entries[i].id = 0;  /* Mark as empty */
        }
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "Loaded cron entries from NVS");
}

/* --------------------------------------------------------------------------
 * save_entry() — persists (or erases) a single slot to NVS.
 * If s_entries[index].id == 0 the slot is considered deleted and the NVS key
 * is erased. Otherwise the full cron_entry_t blob is written.
 * nvs_commit() is called to flush the write to flash before closing.
 * Returns ESP_OK on success, or the first error encountered.
 * -------------------------------------------------------------------------- */
static esp_err_t save_entry(int index)
{
    nvs_handle_t handle;
    /* Open the cron NVS namespace for reading and writing */
    esp_err_t err = nvs_open(NVS_NAMESPACE_CRON, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open cron NVS: %s", esp_err_to_name(err));
        return err;
    }

    char key[16];
    /* Build the key for this slot, e.g. "cron_0" */
    snprintf(key, sizeof(key), "cron_%d", index);

    if (s_entries[index].id == 0) {
        /* id == 0 means this slot is being cleared — erase the NVS key */
        err = nvs_erase_key(handle, key);
        /* NOT_FOUND is fine: the key was never written or was already erased */
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    } else {
        /* Active entry — persist the full struct as a binary blob */
        err = nvs_set_blob(handle, key, &s_entries[index], sizeof(cron_entry_t));
    }

    /* Flush the pending write to flash before closing */
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist cron entry slot %d: %s", index, esp_err_to_name(err));
    }

    return err;
}

/* --------------------------------------------------------------------------
 * cron_init() — one-time initialisation, must be called before cron_start().
 *
 *   1. Zeroes the in-RAM entries array.
 *   2. Creates the entries mutex (idempotent — skipped if already created).
 *   3. Loads persisted entries and timezone from NVS.
 *   4. Starts the SNTP client.
 *   5. Busy-waits up to NTP_SYNC_TIMEOUT_MS for the first NTP sync, polling
 *      every 100 ms. Logs a warning if the timeout expires without sync.
 *
 * Returns ESP_OK on success, ESP_ERR_NO_MEM if the mutex cannot be allocated.
 * -------------------------------------------------------------------------- */
esp_err_t cron_init(void)
{
    /* Clear all entry slots so unwritten fields are deterministically zero */
    memset(s_entries, 0, sizeof(s_entries));

    /* Create the mutex if it has not already been created */
    if (!s_entries_mutex) {
        s_entries_mutex = xSemaphoreCreateMutex();
        if (!s_entries_mutex) {
            ESP_LOGE(TAG, "Failed to create cron mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    /* Restore previously scheduled entries from NVS flash */
    load_entries();
    /* Restore the user's timezone setting from NVS */
    load_timezone_from_nvs();
    /* Start the SNTP background service to obtain wall-clock time */
    init_sntp();

    /* Poll until NTP syncs or the timeout expires.
       vTaskDelay yields the CPU between polls so other tasks can run. */
    int wait_ms = 0;
    while (!s_time_synced && wait_ms < NTP_SYNC_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_ms += 100;
    }

    if (s_time_synced) {
        /* Log the synced local time for diagnostic confirmation */
        char time_str[32];
        cron_get_time_str(time_str, sizeof(time_str));
        ESP_LOGI(TAG, "Current time: %s", time_str);
    } else {
        /* Not fatal — periodic schedules still work; daily ones will fire once
           NTP eventually syncs (could be after WiFi reconnects). */
        ESP_LOGW(TAG, "NTP sync timed out - clock-based schedules may be delayed");
    }

    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * cron_set_timezone() — public API to change the device timezone at runtime.
 * Delegates entirely to apply_timezone() with persist_to_nvs = true so the
 * new value survives a reboot.
 * -------------------------------------------------------------------------- */
esp_err_t cron_set_timezone(const char *timezone_posix)
{
    return apply_timezone(timezone_posix, true);
}

/* --------------------------------------------------------------------------
 * cron_get_timezone() — copies the current POSIX TZ string into the caller's
 * buffer. Always null-terminates. No-ops if buf is NULL or buf_len is 0.
 * -------------------------------------------------------------------------- */
void cron_get_timezone(char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) {
        return;
    }
    strncpy(buf, s_timezone, buf_len - 1);
    buf[buf_len - 1] = '\0';
}

/* --------------------------------------------------------------------------
 * cron_get_timezone_abbrev() — writes the abbreviated local timezone name
 * (e.g. "IST", "UTC", "EST") into buf using strftime("%Z").
 * Falls back to "UTC" if strftime returns 0 or produces an empty string
 * (can happen on targets without full tzdata support).
 * -------------------------------------------------------------------------- */
void cron_get_timezone_abbrev(char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) {
        return;
    }

    time_t now;
    struct tm timeinfo;
    time(&now);
    /* localtime_r is thread-safe (reentrant); uses the TZ set by apply_timezone() */
    localtime_r(&now, &timeinfo);

    /* strftime writes the timezone abbreviation; if it fails, default to "UTC" */
    if (strftime(buf, buf_len, "%Z", &timeinfo) == 0 || buf[0] == '\0') {
        strncpy(buf, "UTC", buf_len - 1);
        buf[buf_len - 1] = '\0';
    }
}

/* --------------------------------------------------------------------------
 * cron_is_time_synced() — returns true if the NTP client has successfully
 * set the system clock at least once since boot.
 * -------------------------------------------------------------------------- */
bool cron_is_time_synced(void)
{
    return s_time_synced;
}

/* --------------------------------------------------------------------------
 * cron_get_time_str() — formats the current local time as "YYYY-MM-DD HH:MM:SS"
 * into the caller-supplied buffer. Uses localtime_r (thread-safe) with the
 * currently active TZ setting.
 * -------------------------------------------------------------------------- */
void cron_get_time_str(char *buf, size_t buf_len)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    /* ISO 8601-like format for easy human readability in logs */
    strftime(buf, buf_len, "%Y-%m-%d %H:%M:%S", &timeinfo);
}

/* --------------------------------------------------------------------------
 * cron_set() — creates a new scheduled cron entry and persists it to NVS.
 *
 * Parameters:
 *   type             — CRON_TYPE_PERIODIC, CRON_TYPE_ONCE, or CRON_TYPE_DAILY
 *   interval_or_hour — minutes (periodic/once) or hour-of-day (daily)
 *   minute           — minute-of-hour (daily only; ignored for other types)
 *   action           — non-empty string dispatched to the agent when fired
 *
 * Returns the new entry's uint8_t ID (1–255) on success, or 0 on failure.
 *
 * Failure paths:
 *   - Empty action string
 *   - Invalid interval or time values (delegated to cron_utils validators)
 *   - Mutex lock timeout
 *   - No free slots in s_entries[]
 *   - ID space exhausted (all 255 IDs in use)
 *   - NVS write failure (entry is rolled back)
 * -------------------------------------------------------------------------- */
uint8_t cron_set(cron_type_t type, uint16_t interval_or_hour, uint8_t minute, const char *action)
{
    uint8_t created_id = 0; /* 0 means failure; updated to the new ID on success */

    /* Reject a missing or blank action string before doing any other work */
    if (!action || action[0] == '\0') {
        ESP_LOGE(TAG, "Cannot create cron entry: empty action");
        return 0;
    }

    /* For periodic type, validate that the interval is within the allowed range */
    if (type == CRON_TYPE_PERIODIC && !cron_validate_periodic_interval((int)interval_or_hour)) {
        ESP_LOGE(TAG, "Invalid periodic interval: %u", interval_or_hour);
        return 0;
    }
    /* For once type, the delay is validated the same way as a periodic interval */
    if (type == CRON_TYPE_ONCE && !cron_validate_periodic_interval((int)interval_or_hour)) {
        ESP_LOGE(TAG, "Invalid once delay: %u", interval_or_hour);
        return 0;
    }
    /* For daily type, validate the hour:minute combination (0–23 : 0–59) */
    if (type == CRON_TYPE_DAILY && !cron_validate_daily_time((int)interval_or_hour, (int)minute)) {
        ESP_LOGE(TAG, "Invalid daily time: %u:%u", interval_or_hour, minute);
        return 0;
    }

    /* Acquire the mutex before scanning or modifying s_entries[] */
    if (!entries_lock(pdMS_TO_TICKS(1000))) {
        ESP_LOGE(TAG, "Failed to lock cron entries");
        return 0;
    }

    /* Scan all slots: find the first free slot and collect every ID in use
       (needed to generate the next unique ID without collisions). */
    int slot = -1;
    uint8_t used_ids[CRON_MAX_ENTRIES] = {0};
    size_t used_count = 0;

    for (int i = 0; i < CRON_MAX_ENTRIES; i++) {
        /* Record the first empty slot we find */
        if (s_entries[i].id == 0 && slot == -1) {
            slot = i;
        }
        /* Collect all currently-used IDs for the ID allocator */
        if (s_entries[i].id != 0 && used_count < CRON_MAX_ENTRIES) {
            used_ids[used_count++] = s_entries[i].id;
        }
    }

    /* No empty slot available — registry is full */
    if (slot == -1) {
        ESP_LOGE(TAG, "No free cron slots");
        goto out;
    }

    /* Allocate the next available ID (skipping any currently in use) */
    uint8_t next_id = cron_next_entry_id(used_ids, used_count);
    if (next_id == 0) {
        /* All 255 IDs are exhausted (extremely unlikely in practice) */
        ESP_LOGE(TAG, "No free cron IDs");
        goto out;
    }

    /* Populate the chosen slot with the new entry's data */
    cron_entry_t *entry = &s_entries[slot];
    entry->id      = next_id;
    entry->type    = type;
    entry->enabled = true;
    entry->last_run = 0; /* Will be set appropriately below for ONCE type */

    if (type == CRON_TYPE_PERIODIC || type == CRON_TYPE_ONCE) {
        /* interval_or_hour holds the number of minutes between fires */
        entry->interval_minutes = interval_or_hour;
        entry->hour   = 0; /* Not used for these types */
        entry->minute = 0;
    } else {
        /* Daily type: store the target hour and minute; interval not used */
        entry->interval_minutes = 0;
        entry->hour   = interval_or_hour;
        entry->minute = minute;
    }

    if (type == CRON_TYPE_ONCE) {
        /* For one-shot entries, last_run stores the creation timestamp so
           check_entries() can calculate when the delay expires. */
        time_t now;
        time(&now);
        /* Only set if the clock looks valid (time_t < 0 means unset on some ports) */
        if (now >= 0) {
            entry->last_run = (uint32_t)now;
        }
    }

    /* Copy the action string with guaranteed null termination */
    strncpy(entry->action, action, CRON_MAX_ACTION_LEN - 1);
    entry->action[CRON_MAX_ACTION_LEN - 1] = '\0';

    /* Attempt to persist the new entry to NVS; roll back if it fails */
    if (save_entry(slot) != ESP_OK) {
        /* Clear the slot so we don't have an in-RAM entry with no NVS backing */
        memset(entry, 0, sizeof(*entry));
        goto out;
    }

    /* Success — record the new ID to return to the caller */
    created_id = entry->id;
    ESP_LOGI(TAG, "Created cron entry %d: type=%d action=%s", entry->id, type, action);

out:
    /* Always release the mutex before returning, even on error paths */
    entries_unlock();
    return created_id;
}

/* --------------------------------------------------------------------------
 * cron_list() — serialises all active cron entries to a JSON array string
 * and writes it into the caller-supplied buffer.
 *
 * Each object in the array contains: id, type, timing field (interval_minutes
 * / delay_minutes / time), action, enabled, timezone (POSIX), timezone_abbrev.
 *
 * On any allocation or build failure the output is set to "[]" as a safe
 * fallback. The mutex is held only for the duration of the JSON build loop;
 * cJSON_PrintUnformatted() is called after releasing it.
 * -------------------------------------------------------------------------- */
void cron_list(char *buf, size_t buf_len)
{
    /* Validate the output buffer before doing any work */
    if (!buf || buf_len == 0) {
        return;
    }

    /* Allocate the root JSON array */
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        /* Allocation failed — return a valid but empty JSON array */
        snprintf(buf, buf_len, "[]");
        return;
    }

    /* Lock entries while we iterate; fail gracefully on timeout */
    if (!entries_lock(pdMS_TO_TICKS(1000))) {
        cJSON_Delete(arr);
        snprintf(buf, buf_len, "[]");
        return;
    }

    /* Snapshot the timezone strings while we hold the lock */
    char timezone_posix[TIMEZONE_MAX_LEN];
    char timezone_abbrev[16];
    cron_get_timezone(timezone_posix, sizeof(timezone_posix));
    cron_get_timezone_abbrev(timezone_abbrev, sizeof(timezone_abbrev));

    /* Iterate all slots; skip empty ones (id == 0) */
    for (int i = 0; i < CRON_MAX_ENTRIES; i++) {
        if (s_entries[i].id == 0) continue;

        /* Allocate a JSON object for this entry */
        cJSON *obj = cJSON_CreateObject();
        if (!obj) {
            ESP_LOGE(TAG, "Failed to allocate cron list entry object");
            /* Unlock, clean up, and return an empty array rather than partial data */
            entries_unlock();
            cJSON_Delete(arr);
            snprintf(buf, buf_len, "[]");
            return;
        }

        /* Track whether all field additions succeed; any NULL return means OOM */
        bool ok = true;
        /* Add the numeric entry ID */
        ok &= cJSON_AddNumberToObject(obj, "id", s_entries[i].id) != NULL;

        /* Map the internal type enum to a human-readable string */
        const char *type_str = "unknown";
        switch (s_entries[i].type) {
            case CRON_TYPE_PERIODIC:  type_str = "periodic";  break;
            case CRON_TYPE_DAILY:     type_str = "daily";     break;
            case CRON_TYPE_CONDITION: type_str = "condition"; break;
            case CRON_TYPE_ONCE:      type_str = "once";      break;
        }
        ok &= cJSON_AddStringToObject(obj, "type", type_str) != NULL;

        /* Add the timing field appropriate to the schedule type */
        if (s_entries[i].type == CRON_TYPE_PERIODIC) {
            /* Periodic: report how many minutes between fires */
            ok &= cJSON_AddNumberToObject(obj, "interval_minutes", s_entries[i].interval_minutes) != NULL;
        } else if (s_entries[i].type == CRON_TYPE_ONCE) {
            /* Once: report the original delay in minutes (stored in interval_minutes) */
            ok &= cJSON_AddNumberToObject(obj, "delay_minutes", s_entries[i].interval_minutes) != NULL;
        } else {
            /* Daily / condition: format the target time as "HH:MM" */
            char time_str[8];
            snprintf(time_str, sizeof(time_str), "%02d:%02d", s_entries[i].hour, s_entries[i].minute);
            ok &= cJSON_AddStringToObject(obj, "time", time_str) != NULL;
        }

        /* Add the action string, enabled flag, and both timezone representations */
        ok &= cJSON_AddStringToObject(obj, "action",          s_entries[i].action) != NULL;
        ok &= cJSON_AddBoolToObject  (obj, "enabled",         s_entries[i].enabled) != NULL;
        ok &= cJSON_AddStringToObject(obj, "timezone",        timezone_posix) != NULL;
        ok &= cJSON_AddStringToObject(obj, "timezone_abbrev", timezone_abbrev) != NULL;

        /* If any field addition failed (OOM), abort and return an empty array */
        if (!ok) {
            ESP_LOGE(TAG, "Failed to build cron list entry JSON");
            cJSON_Delete(obj);
            entries_unlock();
            cJSON_Delete(arr);
            snprintf(buf, buf_len, "[]");
            return;
        }

        /* Append the fully-built object to the array */
        cJSON_AddItemToArray(arr, obj);
    }

    /* Release the mutex before the (potentially slow) JSON serialisation */
    entries_unlock();

    /* Serialise the completed array to a compact JSON string */
    char *json = cJSON_PrintUnformatted(arr);
    if (json) {
        /* Copy into the caller's buffer with guaranteed null termination */
        strncpy(buf, json, buf_len - 1);
        buf[buf_len - 1] = '\0';
        free(json); /* cJSON heap-allocates the string; caller must free it */
    } else {
        /* Serialisation failed (OOM) — fall back to empty array */
        snprintf(buf, buf_len, "[]");
    }

    /* Free the cJSON tree (the serialised string has already been copied) */
    cJSON_Delete(arr);
}

/* --------------------------------------------------------------------------
 * cron_delete() — removes a cron entry by its ID.
 *
 * Acquires the mutex, locates the entry with the matching ID, snapshots it
 * for rollback, clears the slot (id = 0), and calls save_entry() to erase
 * it from NVS. If the NVS write fails the in-RAM entry is restored from the
 * snapshot and the error code is returned. Returns ESP_ERR_NOT_FOUND if no
 * entry with the given ID exists.
 * -------------------------------------------------------------------------- */
esp_err_t cron_delete(uint8_t id)
{
    /* Acquire the mutex with a 1-second timeout */
    if (!entries_lock(pdMS_TO_TICKS(1000))) {
        ESP_LOGE(TAG, "Failed to lock cron entries");
        return ESP_ERR_TIMEOUT;
    }

    /* Linear search for the entry with the requested ID */
    for (int i = 0; i < CRON_MAX_ENTRIES; i++) {
        if (s_entries[i].id == id) {
            /* Snapshot the entry so we can roll back if NVS write fails */
            cron_entry_t previous_entry = s_entries[i];
            /* Mark the slot as free before calling save_entry() */
            s_entries[i].id = 0;
            esp_err_t err = save_entry(i);
            if (err != ESP_OK) {
                /* NVS write failed — restore the in-RAM entry to keep RAM/NVS consistent */
                s_entries[i] = previous_entry;
                entries_unlock();
                return err;
            }
            ESP_LOGI(TAG, "Deleted cron entry %d", id);
            entries_unlock();
            return ESP_OK;
        }
    }

    /* No entry found with the given ID */
    entries_unlock();
    return ESP_ERR_NOT_FOUND;
}

/* --------------------------------------------------------------------------
 * check_entries() — core scheduler tick, called periodically by cron_task().
 *
 * Algorithm:
 *   1. Capture the current time.
 *   2. Lock the entries mutex (skip this tick on timeout — non-fatal).
 *   3. Iterate all active entries and evaluate their fire condition:
 *        PERIODIC — elapsed seconds since last_run >= interval_minutes * 60
 *        ONCE     — elapsed seconds since creation (last_run) >= delay in seconds
 *        DAILY    — current hour:minute matches entry's hour:minute, AND
 *                   last_run is before the start of the current minute (fires
 *                   at most once per minute even if check_entries runs faster)
 *   4. For entries that should fire: copy id + action to s_pending_fires[].
 *      For ONCE entries: clear the slot (set id = 0) and persist the deletion.
 *      For other types: update last_run and persist.
 *   5. Release the mutex.
 *   6. Send each pending fire to the agent queue as a channel_msg_t with
 *      source = MSG_SOURCE_CRON. If the queue is full, the action is dropped
 *      with a warning (non-blocking send avoids deadlock with the agent task).
 * -------------------------------------------------------------------------- */
static void check_entries(void)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    /* localtime_r is reentrant and uses the active TZ set by apply_timezone() */
    localtime_r(&now, &timeinfo);

    /* Counter for how many entries fired this tick */
    int pending_count = 0;

    /* Acquire the mutex; skip this tick rather than block indefinitely */
    if (!entries_lock(pdMS_TO_TICKS(1000))) {
        ESP_LOGW(TAG, "Skipping cron check: lock timeout");
        return;
    }

    /* Evaluate every slot; skip empty (id == 0) or disabled entries */
    for (int i = 0; i < CRON_MAX_ENTRIES; i++) {
        cron_entry_t *entry = &s_entries[i];
        if (entry->id == 0 || !entry->enabled) continue;

        bool should_fire = false;

        if (entry->type == CRON_TYPE_PERIODIC) {
            /* Fire when enough seconds have elapsed since the last run */
            uint32_t interval_seconds = entry->interval_minutes * 60;
            if (now - entry->last_run >= interval_seconds) {
                should_fire = true;
            }
        } else if (entry->type == CRON_TYPE_ONCE) {
            /* Fire when the one-shot delay has elapsed since creation time */
            uint32_t delay_seconds = entry->interval_minutes * 60;
            time_t created_at = (time_t)entry->last_run; /* last_run = creation timestamp for ONCE */
            if (now >= created_at && (uint32_t)(now - created_at) >= delay_seconds) {
                should_fire = true;
            }
        } else if (entry->type == CRON_TYPE_DAILY && s_time_synced) {
            /* Only attempt daily matching after NTP has set the clock */
            /* Check if current time matches the scheduled hour and minute */
            if (timeinfo.tm_hour == entry->hour && timeinfo.tm_min == entry->minute) {
                /* Compute the Unix timestamp of the start of the current minute
                   (subtract the seconds component) to enforce at-most-once-per-minute */
                uint32_t minute_start = now - timeinfo.tm_sec;
                if (entry->last_run < minute_start) {
                    should_fire = true;
                }
            }
        }

        if (should_fire) {
            /* Copy the firing data to static storage outside the mutex scope */
            if (pending_count < CRON_MAX_ENTRIES) {
                s_pending_fires[pending_count].id = entry->id;
                /* Safe copy of the action string with guaranteed null termination */
                strncpy(s_pending_fires[pending_count].action, entry->action,
                        sizeof(s_pending_fires[pending_count].action) - 1);
                s_pending_fires[pending_count].action[sizeof(s_pending_fires[pending_count].action) - 1] = '\0';
                pending_count++;
            }

            if (entry->type == CRON_TYPE_ONCE) {
                /* One-shot: delete the entry after firing so it never runs again */
                uint8_t fired_id = entry->id;
                entry->id = 0; /* Mark slot free before persisting */
                if (save_entry(i) != ESP_OK) {
                    /* NVS write failed — restore the id to keep RAM/NVS consistent;
                       the entry may fire again on the next tick, which is preferable
                       to silently losing the action */
                    entry->id = fired_id;
                    ESP_LOGW(TAG, "Failed to clear one-shot cron %d after firing", fired_id);
                }
            } else {
                /* Recurring entry: update last_run so the next fire is computed correctly */
                entry->last_run = now;
                if (save_entry(i) != ESP_OK) {
                    /* Non-fatal — the entry will still fire correctly in RAM;
                       the worst case after a reboot is one early re-fire */
                    ESP_LOGW(TAG, "Failed to persist run timestamp for cron %d", entry->id);
                }
            }
        }
    }

    /* Release the mutex before sending to the queue to avoid priority inversion */
    entries_unlock();

    /* Dispatch each pending action to the agent input queue */
    for (int i = 0; i < pending_count; i++) {
        ESP_LOGI(TAG, "Firing cron %d: %s", s_pending_fires[i].id, s_pending_fires[i].action);

        /* Build the channel message that the agent will process */
        channel_msg_t msg;
        /* Prefix with "[CRON <id>]" so the agent can identify the source */
        snprintf(msg.text, sizeof(msg.text), "[CRON %d] %s", s_pending_fires[i].id, s_pending_fires[i].action);
        msg.source  = MSG_SOURCE_CRON; /* Tag as originating from the cron subsystem */
        msg.chat_id = 0;               /* No specific chat context for cron-fired actions */

        /* Non-blocking send (100 ms timeout); drop the action if the queue is full
           rather than blocking the cron task indefinitely */
        if (xQueueSend(s_agent_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "Agent queue full, cron action dropped");
        }
    }
}

/* --------------------------------------------------------------------------
 * cron_task() — FreeRTOS task entry point for the cron scheduler.
 * Runs an infinite loop: check for due entries, then sleep for
 * CRON_CHECK_INTERVAL_MS milliseconds before checking again.
 * The arg parameter is unused (required by the FreeRTOS task signature).
 * -------------------------------------------------------------------------- */
static void cron_task(void *arg)
{
    (void)arg; /* Suppress unused-parameter warning */

    while (1) {
        /* Evaluate all entries and fire any that are due */
        check_entries();
        /* Yield the CPU for the configured check interval before the next tick */
        vTaskDelay(pdMS_TO_TICKS(CRON_CHECK_INTERVAL_MS));
    }
}

/* --------------------------------------------------------------------------
 * cron_start() — creates the cron FreeRTOS task and binds it to the agent
 * input queue. Must be called after cron_init() and after the agent task
 * has been started (so the queue is valid).
 *
 * Parameters:
 *   agent_input_queue — the queue to which cron will post channel_msg_t
 *                       messages when an entry fires.
 *
 * Returns ESP_OK on success, ESP_ERR_INVALID_ARG if the queue is NULL, or
 * ESP_ERR_NO_MEM if xTaskCreate() fails.
 * -------------------------------------------------------------------------- */
esp_err_t cron_start(QueueHandle_t agent_input_queue)
{
    /* A valid queue handle is required before we can start firing actions */
    if (!agent_input_queue) {
        ESP_LOGE(TAG, "Invalid queue for cron startup");
        return ESP_ERR_INVALID_ARG;
    }

    /* Store the queue handle so check_entries() can use it */
    s_agent_queue = agent_input_queue;

    /* Create the cron task with the configured stack size and priority */
    if (xTaskCreate(cron_task, "cron", CRON_TASK_STACK_SIZE, NULL,
                    CRON_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create cron task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Cron task started");
    return ESP_OK;
}
