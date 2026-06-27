/* Tool handler function signatures (bool handler(const cJSON*, char*, size_t)) */
#include "tools_handlers.h"
/* Cron public API: cron_set, cron_list, cron_delete, cron_get_time_str, etc. */
#include "cron.h"
/* Cron validation helpers: cron_validate_periodic_interval, cron_validate_daily_time */
#include "cron_utils.h"
/* Project-wide constants: CRON_MAX_ACTION_LEN, TIMEZONE_MAX_LEN, etc. */
#include "config.h"
/* Shared tool utilities: tools_validate_string_input */
#include "tools_common.h"
/* Standard I/O: snprintf */
#include <stdio.h>
/* Standard string functions: strcmp, strchr, strlen, memcpy */
#include <string.h>
/* POSIX string extension: strcasecmp (case-insensitive string compare) */
#include <strings.h>

/*
 * timezone_alias_t — maps a human-friendly timezone name (IANA name, common
 * abbreviation, or US zone shorthand) to the equivalent POSIX TZ string that
 * the C library's tzset() and localtime_r() understand.
 */
typedef struct {
    const char *alias; /* Input name accepted from the user (case-insensitive) */
    const char *posix; /* Equivalent POSIX TZ string passed to setenv("TZ", ...) */
} timezone_alias_t;

/*
 * TZ_ALIASES[] — static lookup table of all supported timezone aliases.
 *
 * POSIX TZ string format recap:
 *   "STDoffsetDST,start/time,end/time"
 *   e.g. "EST5EDT,M3.2.0/2,M11.1.0/2" means:
 *     Standard name EST, UTC-5; DST name EDT;
 *     DST starts 2nd Sunday of March at 02:00,
 *     ends 1st Sunday of November at 02:00.
 *
 * Entries are grouped by US timezone. Any alias that matches (case-insensitive)
 * is resolved to the canonical POSIX string before being applied.
 */
static const timezone_alias_t TZ_ALIASES[] = {
    /* UTC / GMT variants — no offset, no DST */
    {"UTC",                  "UTC0"},
    {"Etc/UTC",              "UTC0"},
    {"GMT",                  "UTC0"},

    /* US Pacific — UTC-8 standard / UTC-7 DST */
    {"America/Los_Angeles",  "PST8PDT,M3.2.0/2,M11.1.0/2"},
    {"US/Pacific",           "PST8PDT,M3.2.0/2,M11.1.0/2"},
    {"PST",                  "PST8PDT,M3.2.0/2,M11.1.0/2"},
    {"PDT",                  "PST8PDT,M3.2.0/2,M11.1.0/2"},
    {"PT",                   "PST8PDT,M3.2.0/2,M11.1.0/2"},

    /* US Mountain — UTC-7 standard / UTC-6 DST */
    {"America/Denver",       "MST7MDT,M3.2.0/2,M11.1.0/2"},
    {"US/Mountain",          "MST7MDT,M3.2.0/2,M11.1.0/2"},
    {"MST",                  "MST7MDT,M3.2.0/2,M11.1.0/2"},
    {"MDT",                  "MST7MDT,M3.2.0/2,M11.1.0/2"},
    {"MT",                   "MST7MDT,M3.2.0/2,M11.1.0/2"},

    /* US Central — UTC-6 standard / UTC-5 DST */
    {"America/Chicago",      "CST6CDT,M3.2.0/2,M11.1.0/2"},
    {"US/Central",           "CST6CDT,M3.2.0/2,M11.1.0/2"},
    {"CST",                  "CST6CDT,M3.2.0/2,M11.1.0/2"},
    {"CDT",                  "CST6CDT,M3.2.0/2,M11.1.0/2"},
    {"CT",                   "CST6CDT,M3.2.0/2,M11.1.0/2"},

    /* US Eastern — UTC-5 standard / UTC-4 DST */
    {"America/New_York",     "EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"US/Eastern",           "EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"EST",                  "EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"EDT",                  "EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"ET",                   "EST5EDT,M3.2.0/2,M11.1.0/2"},
};

/* --------------------------------------------------------------------------
 * trim_ascii_whitespace() — copies src into dst with leading and trailing
 * ASCII whitespace (space, tab, CR, LF) removed. Always null-terminates dst.
 * If src is NULL or dst is NULL/zero-length, dst is set to "" and the
 * function returns early. If the trimmed result is longer than dst_len - 1,
 * it is truncated to fit.
 * -------------------------------------------------------------------------- */
static void trim_ascii_whitespace(const char *src, char *dst, size_t dst_len)
{
    /* Guard: dst must be a valid, non-empty buffer */
    if (!dst || dst_len == 0) {
        return;
    }
    dst[0] = '\0'; /* Ensure dst is always null-terminated even on early return */
    if (!src) {
        return;
    }

    /* Advance past any leading whitespace characters */
    const char *start = src;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }

    /* Walk the end pointer back past any trailing whitespace */
    const char *end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        end--;
    }

    /* Calculate the trimmed length and clamp it to the available buffer space */
    size_t len = (size_t)(end - start);
    if (len >= dst_len) {
        len = dst_len - 1; /* Reserve space for null terminator */
    }
    /* Copy the trimmed content and null-terminate */
    if (len > 0) {
        memcpy(dst, start, len);
    }
    dst[len] = '\0';
}

/* --------------------------------------------------------------------------
 * resolve_timezone_to_posix() — converts a user-supplied timezone string
 * (IANA name, US abbreviation, or raw POSIX TZ string) into a validated
 * POSIX TZ string ready for setenv("TZ", ...).
 *
 * Resolution steps:
 *   1. Trim leading/trailing ASCII whitespace.
 *   2. Validate length and character safety via tools_validate_string_input().
 *   3. Reject an empty string after trimming.
 *   4. Search TZ_ALIASES[] with a case-insensitive match; if found, copy the
 *      corresponding POSIX string to timezone_posix_out and return true.
 *   5. If the input contains '/' but wasn't matched in step 4, it looks like
 *      an IANA name we don't support — return a helpful error message.
 *   6. Reject strings containing spaces (invalid in POSIX TZ format).
 *   7. Fall through: treat the trimmed input as a raw POSIX TZ string and
 *      pass it through unchanged (the caller validates it further via
 *      cron_set_timezone → apply_timezone → timezone_string_is_valid).
 *
 * Returns true and fills timezone_posix_out on success.
 * Returns false and fills error_out with a user-facing message on failure.
 * -------------------------------------------------------------------------- */
static bool resolve_timezone_to_posix(
    const char *timezone_input,
    char *timezone_posix_out,
    size_t timezone_posix_out_len,
    char *error_out,
    size_t error_out_len)
{
    /* Step 1: strip whitespace into a local buffer */
    char trimmed[TIMEZONE_MAX_LEN];
    trim_ascii_whitespace(timezone_input, trimmed, sizeof(trimmed));

    /* Step 2: validate length and that no dangerous characters are present */
    if (!tools_validate_string_input(trimmed, TIMEZONE_MAX_LEN - 1, error_out, error_out_len)) {
        return false;
    }

    /* Step 3: reject blank input after trimming */
    if (trimmed[0] == '\0') {
        snprintf(error_out, error_out_len, "Error: timezone must be non-empty");
        return false;
    }

    /* Step 4: case-insensitive alias lookup */
    for (size_t i = 0; i < sizeof(TZ_ALIASES) / sizeof(TZ_ALIASES[0]); i++) {
        if (strcasecmp(trimmed, TZ_ALIASES[i].alias) == 0) {
            /* Found — write the canonical POSIX TZ string to the output buffer */
            snprintf(timezone_posix_out, timezone_posix_out_len, "%s", TZ_ALIASES[i].posix);
            return true;
        }
    }

    /* Step 5: unrecognised IANA-style name (contains '/') — give a helpful error */
    if (strchr(trimmed, '/') != NULL) {
        snprintf(
            error_out,
            error_out_len,
            "Error: timezone name not recognized. Use UTC, America/Los_Angeles, "
            "America/Denver, America/Chicago, America/New_York, or a POSIX TZ string."
        );
        return false;
    }

    /* Step 6: reject strings with internal spaces (invalid POSIX TZ format) */
    for (size_t i = 0; trimmed[i] != '\0'; i++) {
        char c = trimmed[i];
        if (c == ' ' || c == '\t') {
            snprintf(error_out, error_out_len, "Error: timezone must not contain spaces");
            return false;
        }
    }

    /* Step 7: pass through as a raw POSIX TZ string (e.g. "IST-5:30") */
    snprintf(timezone_posix_out, timezone_posix_out_len, "%s", trimmed);
    return true;
}

/* --------------------------------------------------------------------------
 * tools_cron_set_handler() — tool handler for "cron_set".
 *
 * Reads the required "type" and "action" fields from the JSON input, then
 * dispatches to one of three code paths:
 *
 *   "periodic" — requires "interval_minutes" (1–1440). Fires repeatedly at
 *                that interval. interval_minutes is passed as interval_or_hour.
 *
 *   "daily"    — requires "hour" (0–23); "minute" (0–59) is optional (default 0).
 *                Fires once per day at the specified local time.
 *
 *   "once"     — requires "delay_minutes" (1–1440). Fires exactly once after
 *                the specified delay, then deletes itself.
 *
 * On success, calls cron_set() and writes a human-readable confirmation
 * string (including the assigned schedule ID) into result. Returns true.
 * On any validation or allocation failure, writes an error message into
 * result and returns false.
 * -------------------------------------------------------------------------- */
bool tools_cron_set_handler(const cJSON *input, char *result, size_t result_len)
{
    /* Extract the mandatory "type" and "action" JSON fields */
    cJSON *type_json   = cJSON_GetObjectItem(input, "type");
    cJSON *action_json = cJSON_GetObjectItem(input, "action");

    /* Reject missing or non-string "type" */
    if (!type_json || !cJSON_IsString(type_json)) {
        snprintf(result, result_len, "Error: 'type' required (periodic/daily/once)");
        return false;
    }
    /* Reject missing or non-string "action" */
    if (!action_json || !cJSON_IsString(action_json)) {
        snprintf(result, result_len, "Error: 'action' required (what to do)");
        return false;
    }

    const char *type_str = type_json->valuestring;
    const char *action   = action_json->valuestring;

    /* Validate the action string length and content via shared utility */
    if (!tools_validate_string_input(action, CRON_MAX_ACTION_LEN, result, result_len)) {
        return false;
    }

    /* Variables populated by the type-specific branches below */
    cron_type_t type;
    uint16_t interval_or_hour = 0; /* Minutes (periodic/once) or hour-of-day (daily) */
    uint8_t  minute           = 0; /* Minute-of-hour for daily schedules */

    if (strcmp(type_str, "periodic") == 0) {
        type = CRON_TYPE_PERIODIC;

        /* "interval_minutes" is required for periodic schedules */
        cJSON *interval = cJSON_GetObjectItem(input, "interval_minutes");
        if (!interval || !cJSON_IsNumber(interval)) {
            snprintf(result, result_len, "Error: 'interval_minutes' required for periodic");
            return false;
        }
        /* Validate that the interval is in the 1–1440 minute range */
        if (!cron_validate_periodic_interval(interval->valueint)) {
            snprintf(result, result_len, "Error: interval_minutes must be 1-1440");
            return false;
        }
        interval_or_hour = interval->valueint;

    } else if (strcmp(type_str, "daily") == 0) {
        type = CRON_TYPE_DAILY;

        /* "hour" is required; "minute" is optional (defaults to 0) */
        cJSON *hour_json = cJSON_GetObjectItem(input, "hour");
        cJSON *min_json  = cJSON_GetObjectItem(input, "minute");

        if (!hour_json || !cJSON_IsNumber(hour_json)) {
            snprintf(result, result_len, "Error: 'hour' required for daily (0-23)");
            return false;
        }
        /* If "minute" is provided it must be a number; reject any other type */
        if (min_json && !cJSON_IsNumber(min_json)) {
            snprintf(result, result_len, "Error: 'minute' must be a number (0-59)");
            return false;
        }

        int hour       = hour_json->valueint;
        int minute_int = min_json ? min_json->valueint : 0; /* Default to :00 */

        /* Validate the combined hour:minute value */
        if (!cron_validate_daily_time(hour, minute_int)) {
            snprintf(result, result_len, "Error: daily time must be hour 0-23 and minute 0-59");
            return false;
        }
        interval_or_hour = (uint16_t)hour;
        minute           = (uint8_t)minute_int;

    } else if (strcmp(type_str, "once") == 0) {
        type = CRON_TYPE_ONCE;

        /* "delay_minutes" is required for one-shot schedules */
        cJSON *delay = cJSON_GetObjectItem(input, "delay_minutes");
        if (!delay || !cJSON_IsNumber(delay)) {
            snprintf(result, result_len, "Error: 'delay_minutes' required for once");
            return false;
        }
        /* Reuse the periodic interval validator — same 1–1440 minute range */
        if (!cron_validate_periodic_interval(delay->valueint)) {
            snprintf(result, result_len, "Error: delay_minutes must be 1-1440");
            return false;
        }
        interval_or_hour = (uint16_t)delay->valueint;

    } else {
        /* Unknown type string — reject with a helpful message */
        snprintf(result, result_len, "Error: type must be 'periodic', 'daily', or 'once'");
        return false;
    }

    /* Delegate to the cron subsystem to allocate an ID and persist the entry */
    uint8_t id = cron_set(type, interval_or_hour, minute, action);
    if (id > 0) {
        /* Build a human-readable confirmation message appropriate to the type */
        if (type == CRON_TYPE_PERIODIC) {
            snprintf(result, result_len, "Created schedule #%d: every %d min → %s",
                     id, interval_or_hour, action);
        } else if (type == CRON_TYPE_DAILY) {
            /* Include the timezone abbreviation so the user knows which clock is used */
            char timezone_abbrev[16];
            cron_get_timezone_abbrev(timezone_abbrev, sizeof(timezone_abbrev));
            snprintf(result, result_len, "Created schedule #%d: daily at %02d:%02d %s → %s",
                     id, interval_or_hour, minute, timezone_abbrev, action);
        } else {
            snprintf(result, result_len, "Created schedule #%d: once in %d min → %s",
                     id, interval_or_hour, action);
        }
        return true;
    }

    /* cron_set() returned 0 — no free slots available */
    snprintf(result, result_len, "Error: no free schedule slots");
    return false;
}

/* --------------------------------------------------------------------------
 * tools_cron_list_handler() — tool handler for "cron_list".
 *
 * Takes no inputs. Delegates directly to cron_list() which writes a JSON
 * array of all active schedule entries into the result buffer.
 * Always returns true (cron_list() always produces valid output).
 * -------------------------------------------------------------------------- */
bool tools_cron_list_handler(const cJSON *input, char *result, size_t result_len)
{
    (void)input; /* No input fields required for this tool */
    cron_list(result, result_len);
    return true;
}

/* --------------------------------------------------------------------------
 * tools_cron_delete_handler() — tool handler for "cron_delete".
 *
 * Reads the required numeric "id" field and calls cron_delete(). Three
 * outcome cases:
 *   ESP_OK           — writes "Deleted schedule #N", returns true.
 *   ESP_ERR_NOT_FOUND — writes "Schedule #N not found", returns true
 *                       (not an error from the tool's perspective — the
 *                       desired end state is already achieved).
 *   Any other error  — writes an error string including esp_err_to_name(),
 *                       returns false.
 * -------------------------------------------------------------------------- */
bool tools_cron_delete_handler(const cJSON *input, char *result, size_t result_len)
{
    /* Extract the mandatory numeric "id" field */
    cJSON *id_json = cJSON_GetObjectItem(input, "id");

    if (!id_json || !cJSON_IsNumber(id_json)) {
        snprintf(result, result_len, "Error: 'id' required (number)");
        return false;
    }

    /* Attempt to delete; handle each outcome distinctly */
    esp_err_t err = cron_delete(id_json->valueint);
    if (err == ESP_OK) {
        snprintf(result, result_len, "Deleted schedule #%d", id_json->valueint);
        return true;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        /* Idempotent: treat "already gone" as success so the agent doesn't retry */
        snprintf(result, result_len, "Schedule #%d not found", id_json->valueint);
        return true;
    }

    /* Unexpected error — include the ESP error name for diagnostics */
    snprintf(result, result_len, "Error: failed to delete schedule #%d (%s)",
             id_json->valueint, esp_err_to_name(err));
    return false;
}

/* --------------------------------------------------------------------------
 * tools_get_time_handler() — tool handler for "get_time".
 *
 * Takes no inputs. Reads the current timezone (POSIX string and abbreviation)
 * and, if NTP has synced, the current local time string. Writes a combined
 * human-readable result:
 *   Synced:    "YYYY-MM-DD HH:MM:SS TZ (TZ=POSIX_STRING)"
 *   Not synced: "Time not synced (no NTP). Configured TZ=POSIX_STRING (ABBREV)"
 * Always returns true.
 * -------------------------------------------------------------------------- */
bool tools_get_time_handler(const cJSON *input, char *result, size_t result_len)
{
    (void)input; /* No input fields required */

    char time_str[32];
    char timezone_posix[TIMEZONE_MAX_LEN];
    char timezone_abbrev[16];

    /* Retrieve both the full POSIX TZ string and the short abbreviation */
    cron_get_timezone(timezone_posix, sizeof(timezone_posix));
    cron_get_timezone_abbrev(timezone_abbrev, sizeof(timezone_abbrev));

    if (cron_is_time_synced()) {
        /* NTP has set the clock — report the current local time */
        cron_get_time_str(time_str, sizeof(time_str));
        snprintf(result, result_len, "%s %s (TZ=%s)", time_str, timezone_abbrev, timezone_posix);
    } else {
        /* Clock not yet set — report the configured timezone but warn about sync */
        snprintf(result, result_len, "Time not synced (no NTP). Configured TZ=%s (%s)",
                 timezone_posix, timezone_abbrev);
    }
    return true;
}

/* --------------------------------------------------------------------------
 * tools_set_timezone_handler() — tool handler for "set_timezone".
 *
 * Reads the required "timezone" string, resolves it to a POSIX TZ string via
 * resolve_timezone_to_posix() (which accepts IANA names, US abbreviations, or
 * raw POSIX strings), then calls cron_set_timezone() to apply and persist it.
 *
 * On success, writes a confirmation message that includes:
 *   - The POSIX TZ string that was applied.
 *   - The resolved timezone abbreviation (e.g. "IST").
 *   - The current local time if NTP is synced, or a "pending" note if not.
 *
 * Returns true on success, false on validation or apply failure.
 * -------------------------------------------------------------------------- */
bool tools_set_timezone_handler(const cJSON *input, char *result, size_t result_len)
{
    /* Extract the mandatory "timezone" string field */
    cJSON *tz_json = cJSON_GetObjectItem(input, "timezone");
    if (!tz_json || !cJSON_IsString(tz_json)) {
        snprintf(result, result_len, "Error: 'timezone' required (string)");
        return false;
    }

    /* Resolve the user-supplied name to a POSIX TZ string */
    char timezone_posix[TIMEZONE_MAX_LEN];
    if (!resolve_timezone_to_posix(
            tz_json->valuestring,
            timezone_posix,
            sizeof(timezone_posix),
            result,          /* resolve writes error message directly into result on failure */
            result_len)) {
        return false;
    }

    /* Apply the resolved POSIX TZ string (validates, calls setenv+tzset, persists to NVS) */
    esp_err_t err = cron_set_timezone(timezone_posix);
    if (err != ESP_OK) {
        snprintf(result, result_len, "Error: failed to set timezone (%s)", esp_err_to_name(err));
        return false;
    }

    /* Build the confirmation message — include current local time if clock is synced */
    char timezone_abbrev[16];
    char time_str[32];
    cron_get_timezone_abbrev(timezone_abbrev, sizeof(timezone_abbrev));
    if (cron_is_time_synced()) {
        cron_get_time_str(time_str, sizeof(time_str));
        snprintf(result, result_len,
                 "Timezone set to %s (%s). Current local time: %s %s",
                 timezone_posix, timezone_abbrev, time_str, timezone_abbrev);
    } else {
        /* NTP not yet synced — acknowledge the change but flag the pending sync */
        snprintf(result, result_len,
                 "Timezone set to %s (%s). Time not synced yet (NTP pending).",
                 timezone_posix, timezone_abbrev);
    }
    return true;
}

/* --------------------------------------------------------------------------
 * tools_get_timezone_handler() — tool handler for "get_timezone".
 *
 * Takes no inputs. Reads the current POSIX TZ string and its abbreviated
 * display name, then writes them into result as:
 *   "Timezone: <POSIX_STRING> (<ABBREV>)"
 * Always returns true.
 * -------------------------------------------------------------------------- */
bool tools_get_timezone_handler(const cJSON *input, char *result, size_t result_len)
{
    (void)input; /* No input fields required */

    char timezone_posix[TIMEZONE_MAX_LEN];
    char timezone_abbrev[16];

    /* Retrieve both representations of the active timezone */
    cron_get_timezone(timezone_posix, sizeof(timezone_posix));
    cron_get_timezone_abbrev(timezone_abbrev, sizeof(timezone_abbrev));

    snprintf(result, result_len, "Timezone: %s (%s)", timezone_posix, timezone_abbrev);
    return true;
}
