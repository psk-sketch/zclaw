// ============================================================
// agent.c — Core agent: message routing, LLM loop, tool dispatch
// ============================================================

// --- Includes ---
#include "agent.h"              // Our own public interface
#include "agent_commands.h"     // Command detection & argument parsing (/diag, /gpio, etc.)
#include "agent_prompt.h"       // Builds the LLM system prompt based on persona
#include "config.h"             // Project-wide compile-time constants (buffer sizes, limits)
#include "local_admin.h"        // USB serial admin commands (/reboot, /wifi, /factory-reset)
#include "llm.h"                // Makes HTTP requests to the LLM API
#include "tools.h"              // Built-in tool registry and executor
#include "user_tools.h"         // User-defined tools (stored custom actions)
#include "json_util.h"          // JSON request builder and response parser
#include "messages.h"           // Message types: channel_msg_t, telegram_msg_t, etc.
#include "ratelimit.h"          // Daily LLM request rate limiter
#include "memory.h"             // NVS read/write wrappers
#include "nvs_keys.h"           // NVS key name constants
#include "telegram.h"           // Telegram polling control and message sending
#include "cJSON.h"              // Lightweight JSON library
#include "esp_timer.h"          // esp_timer_get_time(): microsecond-resolution timer
#include "esp_log.h"            // ESP_LOGI / ESP_LOGE / ESP_LOGW logging macros
#include "freertos/FreeRTOS.h"  // FreeRTOS core
#include "freertos/task.h"      // xTaskCreate, vTaskDelay
#include "freertos/queue.h"     // xQueueReceive, xQueueSend
#include <string.h>             // memset, memmove, strncpy, strcmp, strlen
#include <stdlib.h>             // free(), malloc()
#include <inttypes.h>           // PRIu32 / PRIu64 printf format macros for fixed-width ints
#include <ctype.h>              // tolower()

// --- Module-level logging tag ---
// All ESP_LOG* calls from this file will be prefixed with "[agent]" in the serial monitor.
static const char *TAG = "agent";

// --- Static (module-private) state ---

// FreeRTOS queue handles — set once by agent_start() and used throughout.
static QueueHandle_t s_input_queue;             // Incoming messages to be processed
static QueueHandle_t s_channel_output_queue;    // Output to USB serial channel
static QueueHandle_t s_telegram_output_queue;   // Output to Telegram

// Timestamps (in microseconds) used to suppress duplicate/rapid messages.
static int64_t s_last_start_response_us = 0;           // Last time /start was responded to
static int64_t s_last_non_command_response_us = 0;     // Last time a plain chat message was handled

// The text of the last plain (non-command) message, used for duplicate detection.
static char s_last_non_command_text[CHANNEL_RX_BUF_SIZE] = {0};

// When true, the agent silently drops incoming messages (user sent /stop).
static bool s_messages_paused = false;

// Scratch buffer for building the LLM system prompt string.
static char s_system_prompt_buf[2048];

// Current personality the LLM uses when generating responses.
static agent_persona_t s_persona = AGENT_PERSONA_NEUTRAL;

#ifdef TEST_BUILD
// In test builds only: stores a persona string that persona_store_get() will return
// instead of reading from NVS (which doesn't exist in unit tests).
static char s_test_persona_value[16] = {0};
#endif

// --- Conversation history ---
// A rolling buffer of the last MAX_HISTORY_TURNS*2 messages (user + assistant pairs).
static conversation_msg_t s_history[MAX_HISTORY_TURNS * 2];
static int s_history_len = 0;  // Number of valid entries currently in s_history

// --- Static buffers for LLM I/O ---
// Declared static (not on stack) to avoid stack overflow on the FreeRTOS task.
static char s_response_buf[LLM_RESPONSE_BUF_SIZE];     // Holds raw JSON response from LLM
static char s_tool_result_buf[TOOL_RESULT_BUF_SIZE];   // Holds the result string of a tool call

// --- Per-request performance metrics ---
typedef struct {
    int64_t  started_us;     // Absolute timestamp when the request began (microseconds)
    uint64_t llm_us_total;   // Total microseconds spent waiting for LLM HTTP responses
    uint64_t tool_us_total;  // Total microseconds spent executing tool calls
    int      llm_calls;      // Number of LLM API calls made for this request
    int      tool_calls;     // Number of tool calls dispatched for this request
    int      rounds;         // Number of tool-use → LLM loop iterations completed
} request_metrics_t;

// Returns how many microseconds have elapsed since the given timestamp.
// Returns 0 if the clock appears to have gone backwards (shouldn't happen, but safe).
static uint64_t elapsed_us_since(int64_t started_us)
{
    int64_t now_us = esp_timer_get_time();  // Monotonic microsecond clock
    if (now_us <= started_us) {
        return 0;   // Guard against clock anomalies
    }
    return (uint64_t)(now_us - started_us);
}

// Converts a microsecond duration to milliseconds as a uint32_t.
// Clamps to UINT32_MAX to avoid overflow if the duration is very large.
static uint32_t us_to_ms_u32(uint64_t duration_us)
{
    uint64_t duration_ms = duration_us / 1000ULL;   // 1 ms = 1000 µs
    if (duration_ms > UINT32_MAX) {
        return UINT32_MAX;  // Clamp: shouldn't happen in practice
    }
    return (uint32_t)duration_ms;
}

// Emits a structured log line summarising timing and call counts for a completed request.
// 'outcome' is a short string like "success", "rate_limited", "llm_error", etc.
static void metrics_log_request(const request_metrics_t *metrics, const char *outcome)
{
    if (!metrics) {
        return;  // Safety check: don't crash if caller passes NULL
    }

    ESP_LOGI(TAG,
             "METRIC request outcome=%s total_ms=%" PRIu32 " llm_ms=%" PRIu32
             " tool_ms=%" PRIu32 " rounds=%d llm_calls=%d tool_calls=%d",
             outcome ? outcome : "unknown",
             us_to_ms_u32(elapsed_us_since(metrics->started_us)), // Wall-clock total
             us_to_ms_u32(metrics->llm_us_total),                 // Time in LLM calls
             us_to_ms_u32(metrics->tool_us_total),                // Time in tool calls
             metrics->rounds,
             metrics->llm_calls,
             metrics->tool_calls);
}

// Rolls back the conversation history to a previously saved length.
// Used to undo partial history additions when a request fails midway,
// keeping the history consistent.
// 'marker' is the s_history_len value saved before the turn began.
// 'reason' is a human-readable string logged for diagnostics.
static void history_rollback_to(int marker, const char *reason)
{
    // Bounds check: marker must be a valid index strictly less than current length.
    if (marker < 0 || marker > s_history_len || marker == s_history_len) {
        return;  // Nothing to do or invalid marker
    }

    ESP_LOGW(TAG, "Rolling back conversation history (%d -> %d): %s",
             s_history_len, marker, reason ? reason : "unknown");

    // Zero out the entries from 'marker' onwards to prevent stale data.
    memset(&s_history[marker], 0, (s_history_len - marker) * sizeof(conversation_msg_t));
    s_history_len = marker;  // Restore length to the saved point
}

// Appends a new message to the conversation history buffer.
// When full, drops the oldest single entry (not pairs) to accommodate tool call sequences
// that may span more than two messages.
// role         — "user" or "assistant"
// content      — the message text (or serialised tool input JSON)
// is_tool_use  — true if this is an assistant requesting a tool call
// is_tool_result — true if this is the result of a tool call being fed back
// tool_id      — unique ID linking a tool_use message to its tool_result
// tool_name    — name of the tool being called (only for tool_use messages)
static void history_add(const char *role, const char *content,
                        bool is_tool_use, bool is_tool_result,
                        const char *tool_id, const char *tool_name)
{
    // If history is full, shift everything left by one to make room.
    // Dropping by pairs (2 at a time) would be unsafe because tool interactions
    // involve 3+ messages, so we drop one at a time instead.
    if (s_history_len >= MAX_HISTORY_TURNS * 2) {
        memmove(&s_history[0], &s_history[1],
                (MAX_HISTORY_TURNS * 2 - 1) * sizeof(conversation_msg_t));
        s_history_len -= 1;
    }

    // Write the new entry at the current end of the array.
    conversation_msg_t *msg = &s_history[s_history_len++];

    // Copy strings with explicit null-termination (strncpy does NOT guarantee '\0').
    strncpy(msg->role, role, sizeof(msg->role) - 1);
    msg->role[sizeof(msg->role) - 1] = '\0';

    strncpy(msg->content, content, sizeof(msg->content) - 1);
    msg->content[sizeof(msg->content) - 1] = '\0';

    msg->is_tool_use    = is_tool_use;
    msg->is_tool_result = is_tool_result;

    // tool_id links a tool_use to its matching tool_result (may be NULL for plain messages).
    if (tool_id) {
        strncpy(msg->tool_id, tool_id, sizeof(msg->tool_id) - 1);
        msg->tool_id[sizeof(msg->tool_id) - 1] = '\0';
    } else {
        msg->tool_id[0] = '\0';  // Empty string signals "no tool ID"
    }

    // tool_name is only set on tool_use entries, not on tool_result or plain messages.
    if (tool_name) {
        strncpy(msg->tool_name, tool_name, sizeof(msg->tool_name) - 1);
        msg->tool_name[sizeof(msg->tool_name) - 1] = '\0';
    } else {
        msg->tool_name[0] = '\0';
    }
}

// Puts a text response onto the USB serial channel output queue.
// If the queue is full (timeout = 1 second), logs an error and drops the message.
static void queue_channel_response(const char *text)
{
    if (!s_channel_output_queue) {
        return;  // Queue not configured (e.g. in a partial test setup)
    }

    channel_output_msg_t msg;
    strncpy(msg.text, text, CHANNEL_TX_BUF_SIZE - 1);
    msg.text[CHANNEL_TX_BUF_SIZE - 1] = '\0';  // Guarantee null termination

    // xQueueSend blocks for up to 1000 ms waiting for space in the queue.
    // pdTRUE means the send succeeded; anything else is a queue full/timeout.
    if (xQueueSend(s_channel_output_queue, &msg, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to send response to channel queue");
    }
}

// Puts a text response onto the Telegram output queue, addressed to a specific chat.
// Follows the same pattern as queue_channel_response.
static void queue_telegram_response(const char *text, int64_t chat_id)
{
    if (!s_telegram_output_queue) {
        return;
    }

    telegram_msg_t msg;
    strncpy(msg.text, text, TELEGRAM_MAX_MSG_LEN - 1);
    msg.text[TELEGRAM_MAX_MSG_LEN - 1] = '\0';
    msg.chat_id = chat_id;  // The Telegram chat that should receive this reply

    if (xQueueSend(s_telegram_output_queue, &msg, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to send response to Telegram queue");
    }
}

// Convenience wrapper: sends the same text to BOTH the channel and Telegram simultaneously.
static void send_response(const char *text, int64_t chat_id)
{
    queue_channel_response(text);
    queue_telegram_response(text, chat_id);
}

// --- Persona storage (production vs test) ---
// In production, reads the persona string from NVS flash storage.
// In test builds, returns from a small in-memory string instead (NVS doesn't exist in tests).
#ifndef TEST_BUILD
static bool persona_store_get(char *value, size_t max_len)
{
    return memory_get(NVS_KEY_PERSONA, value, max_len);  // Reads "persona" key from NVS
}
#else
static bool persona_store_get(char *value, size_t max_len)
{
    // Return false (not found) if no test value has been set yet.
    if (!value || max_len == 0 || s_test_persona_value[0] == '\0') {
        return false;
    }
    strncpy(value, s_test_persona_value, max_len - 1);
    value[max_len - 1] = '\0';
    return true;
}
#endif

// Reads the persona from persistent storage and applies it to the running agent state.
// If no persona is stored, or the stored string is invalid, silently stays NEUTRAL.
static void load_persona_from_store(void)
{
    char stored[32] = {0};
    agent_persona_t parsed = AGENT_PERSONA_NEUTRAL;

    s_persona = AGENT_PERSONA_NEUTRAL;  // Start from a known default

    if (!persona_store_get(stored, sizeof(stored))) {
        return;  // Nothing stored, keep default
    }

    // Normalise to lowercase so comparison is case-insensitive.
    for (size_t i = 0; stored[i] != '\0'; i++) {
        stored[i] = (char)tolower((unsigned char)stored[i]);
    }

    // Try to parse the stored string into a known persona enum value.
    if (!agent_parse_persona_name(stored, &parsed)) {
        ESP_LOGW(TAG, "Ignoring invalid stored persona '%s'", stored);
        return;  // Unknown string; stay neutral rather than crashing
    }

    s_persona = parsed;
    ESP_LOGI(TAG, "Loaded persona: %s", agent_persona_name(s_persona));
}

// --- Command handlers ---

// Handles the /diag [scope] [verbose] command: runs the get_diagnostics tool directly
// (bypassing the LLM) and sends the result back as plain text.
static void handle_diag_command(const char *user_message, int64_t chat_id, request_metrics_t *metrics)
{
    char error[120] = {0};
    cJSON *tool_input = cJSON_CreateObject();  // JSON object to hold parsed arguments
    bool ok;
    int64_t started_us;

    if (!tool_input) {
        // cJSON allocation failed — very unlikely but must be handled.
        send_response("Error: diagnostics unavailable (allocation failed)", chat_id);
        metrics_log_request(metrics, "diag_no_mem");
        return;
    }

    // Parse the command arguments (scope, verbose flag) into tool_input JSON.
    if (!agent_parse_diag_command_args(user_message, tool_input, error, sizeof(error))) {
        send_response(error, chat_id);         // Error message was populated by the parser
        cJSON_Delete(tool_input);              // Always free cJSON objects when done
        metrics_log_request(metrics, "diag_invalid_args");
        return;
    }

    // Execute the get_diagnostics built-in tool directly.
    s_tool_result_buf[0] = '\0';               // Clear any previous tool result
    started_us = esp_timer_get_time();
    ok = tools_execute("get_diagnostics", tool_input, s_tool_result_buf, sizeof(s_tool_result_buf));
    metrics->tool_us_total += elapsed_us_since(started_us);  // Accumulate time spent in tool
    metrics->tool_calls++;
    cJSON_Delete(tool_input);                  // Done with input JSON

    if (!ok) {
        // Tool itself failed — use its error message if available, otherwise a generic one.
        if (s_tool_result_buf[0] == '\0') {
            snprintf(s_tool_result_buf, sizeof(s_tool_result_buf), "Error: diagnostics failed");
        }
        send_response(s_tool_result_buf, chat_id);
        metrics_log_request(metrics, "diag_failed");
        return;
    }

    send_response(s_tool_result_buf, chat_id);
    metrics_log_request(metrics, "diag_handled");
}

// Handles /gpio [all|pin|pin high|pin low]: reads or writes GPIO pins directly,
// without going through the LLM.
static void handle_gpio_command(const char *user_message, int64_t chat_id, request_metrics_t *metrics)
{
    char error[120] = {0};
    cJSON *tool_input = cJSON_CreateObject();
    const char *tool_name = NULL;  // Will be set to "gpio_read" or "gpio_write" by the parser
    bool ok;
    int64_t started_us;

    if (!tool_input) {
        send_response("Error: GPIO read unavailable (allocation failed)", chat_id);
        metrics_log_request(metrics, "gpio_no_mem");
        return;
    }

    // Parse the GPIO command to determine which tool to call and with what arguments.
    if (!agent_parse_gpio_command_args(user_message, &tool_name, tool_input, error, sizeof(error))) {
        send_response(error, chat_id);
        cJSON_Delete(tool_input);
        metrics_log_request(metrics, "gpio_invalid_args");
        return;
    }

    // Execute the resolved GPIO tool (read or write).
    s_tool_result_buf[0] = '\0';
    started_us = esp_timer_get_time();
    ok = tools_execute(tool_name, tool_input, s_tool_result_buf, sizeof(s_tool_result_buf));
    metrics->tool_us_total += elapsed_us_since(started_us);
    metrics->tool_calls++;
    cJSON_Delete(tool_input);

    if (!ok) {
        if (s_tool_result_buf[0] == '\0') {
            snprintf(s_tool_result_buf, sizeof(s_tool_result_buf), "Error: GPIO read failed");
        }
        send_response(s_tool_result_buf, chat_id);
        metrics_log_request(metrics, "gpio_failed");
        return;
    }

    send_response(s_tool_result_buf, chat_id);
    metrics_log_request(metrics, "gpio_handled");
}

// Returns true if the message is a local admin command (/gpio, /diag, or anything
// handled by the local_admin module like /reboot, /wifi, /factory-reset).
static bool is_local_admin_command(const char *user_message)
{
    return agent_is_command(user_message, "gpio") ||
           agent_is_command(user_message, "diag") ||
           local_admin_is_command(user_message);
}

// Dispatches a recognised local admin command to the appropriate handler.
// IMPORTANT: Local admin commands are only allowed over the USB serial channel,
// never from Telegram, for security reasons.
static void handle_local_admin_command(const char *user_message,
                                       message_source_t source,
                                       int64_t chat_id,
                                       request_metrics_t *metrics)
{
    char response[CHANNEL_TX_BUF_SIZE];
    local_admin_action_t action = LOCAL_ADMIN_ACTION_NONE;

    // Reject the command if it came from Telegram (remote channel).
    if (source != MSG_SOURCE_CHANNEL) {
        send_response("Error: local admin commands are only available on the USB serial console.", chat_id);
        metrics_log_request(metrics, "local_admin_remote_denied");
        return;
    }

    // Route to specific handlers for /diag and /gpio.
    if (agent_is_command(user_message, "diag")) {
        handle_diag_command(user_message, chat_id, metrics);
        return;
    }

    if (agent_is_command(user_message, "gpio")) {
        handle_gpio_command(user_message, chat_id, metrics);
        return;
    }

    // All other admin commands go to the generic local_admin handler
    // (which covers /reboot, /wifi, /bootcount, /factory-reset, etc.).
    if (!local_admin_handle_command(user_message, response, sizeof(response), &action)) {
        send_response(response, chat_id);
        metrics_log_request(metrics, "local_admin_invalid");
        return;
    }

    send_response(response, chat_id);
    metrics_log_request(metrics, "local_admin_handled");

    // Some commands (e.g. /reboot) need a side-effect *after* the response is sent.
    local_admin_perform_action(action);
}

// Responds to /start or /help with a user-facing summary of available commands.
static void handle_start_command(int64_t chat_id)
{
    // Static string stored in flash (not RAM) — doesn't waste precious heap/stack.
    static const char *START_HELP_TEXT =
        "zclaw online.\n\n"
        "Talk to me in normal language. You do not need command syntax.\n\n"
        "Examples:\n"
        "- what are all GPIO states\n"
        "- turn GPIO 5 on\n"
        "- remind me daily at 8:15 to water plants\n"
        "- remember that GPIO 4 controls the arcade machine\n"
        "- create a tool called arcade_on that turns GPIO 4 on\n"
        "- turn the arcade on in 10 minutes\n"
        "- switch to witty persona\n"
        "\n"
        "Chat commands:\n"
        "- /help (show this message)\n"
        "- /settings (show status)\n"
        "- /stop (pause intake)\n"
        "- /resume (resume)\n"
        "\n"
        "USB local admin commands:\n"
        "- /gpio [all|pin|pin high|pin low]\n"
        "- /diag [scope] [verbose]\n"
        "- /reboot\n"
        "- /wifi [status|scan]\n"
        "- /bootcount\n"
        "- /factory-reset confirm";
    send_response(START_HELP_TEXT, chat_id);
}

// Responds to /settings with a live status snapshot of the agent's current configuration.
static void handle_settings_command(int64_t chat_id)
{
    char settings_text[384];
    snprintf(settings_text, sizeof(settings_text),
             "zclaw settings:\n"
             "- Message intake: %s\n"                 // "active" or "paused"
             "- Persona: %s\n"                        // Current persona name
             "- Chat commands: /start, /help, /settings, /stop, /resume\n"
             "- USB local admin: /gpio, /diag, /reboot, /wifi, /bootcount, /factory-reset\n"
             "- /gpio supports reads and writes (e.g. /gpio 9 low)\n"
             "- Persona changes: ask in normal chat (handled via tool calls)\n"
             "- Device settings are global (e.g., timezone <name>)",
             s_messages_paused ? "paused" : "active",
             agent_persona_name(s_persona));
    send_response(settings_text, chat_id);
}

// Maps an incoming message's source to the chat ID that should receive the reply.
// For Telegram messages with a real chat ID, we reply to that specific chat.
// For serial channel messages (chat_id == 0), we return 0 (no Telegram reply needed).
static int64_t response_chat_id_for_source(message_source_t source, int64_t chat_id)
{
    if (source == MSG_SOURCE_TELEGRAM && chat_id != 0) {
        return chat_id;   // Reply to the Telegram chat this message came from
    }
    return 0;             // Serial channel: no Telegram reply target
}

// ============================================================
// process_message — the heart of the agent
// ============================================================
// Handles a single incoming message end-to-end:
//   1. Routes built-in slash commands (/start, /stop, /settings, /gpio, /diag, etc.)
//   2. Deduplicates repeated messages
//   3. Runs the LLM request → tool call → LLM loop until a text response is produced
//      or MAX_TOOL_ROUNDS is reached
// user_message  — the raw text of the incoming message
// source        — whether it came from the USB serial channel or Telegram
// reply_chat_id — which Telegram chat to reply to (0 for serial channel)
static void process_message(const char *user_message, message_source_t source, int64_t reply_chat_id)
{
    ESP_LOGI(TAG, "Processing: %s", user_message);

    int history_turn_start = s_history_len;  // Snapshot history length for potential rollback

    // Classify the message before any routing decisions.
    bool is_non_command_message = !agent_is_slash_command(user_message); // True for plain chat
    bool is_cron_trigger = agent_is_cron_trigger_message(user_message);  // True for scheduled tasks

    bool telegram_polling_paused = false;  // Tracks whether we paused Telegram polling

    // Initialise per-request performance metrics.
    request_metrics_t metrics = {
        .started_us   = esp_timer_get_time(),
        .llm_us_total = 0,
        .tool_us_total= 0,
        .llm_calls    = 0,
        .tool_calls   = 0,
        .rounds       = 0,
    };

    // --- Slash command routing ---
    // /resume must be checked BEFORE the paused gate below, so it can unblock the agent.
    if (agent_is_command(user_message, "resume")) {
        if (!s_messages_paused) {
            send_response("zclaw is already active.", reply_chat_id);
            metrics_log_request(&metrics, "resume_noop");
            return;
        }
        s_messages_paused = false;
        send_response("zclaw resumed. Send /start for command help.", reply_chat_id);
        metrics_log_request(&metrics, "resumed");
        return;
    }

    // /settings works even when paused.
    if (agent_is_command(user_message, "settings")) {
        handle_settings_command(reply_chat_id);
        metrics_log_request(&metrics, "settings_handled");
        return;
    }

    // Local admin commands (/gpio, /diag, /reboot, etc.) work even when paused.
    if (is_local_admin_command(user_message)) {
        handle_local_admin_command(user_message, source, reply_chat_id, &metrics);
        return;
    }

    // All other non-command and non-resume messages are dropped while paused.
    if (s_messages_paused) {
        ESP_LOGD(TAG, "Paused mode: ignoring message");
        metrics_log_request(&metrics, "paused_drop");
        return;
    }

    // /help and /start show the help text.
    if (agent_is_command(user_message, "help")) {
        handle_start_command(reply_chat_id);
        metrics_log_request(&metrics, "help_handled");
        return;
    }

    if (agent_is_command(user_message, "stop")) {
        s_messages_paused = true;
        send_response("zclaw paused. I will ignore new messages until /resume.", reply_chat_id);
        metrics_log_request(&metrics, "paused");
        return;
    }

    // /start is rate-limited by a cooldown to avoid flooding on reconnects.
    if (agent_is_command(user_message, "start")) {
        int64_t now_us = esp_timer_get_time();
        uint32_t since_last_start_ms = 0;
        if (s_last_start_response_us > 0 && now_us > s_last_start_response_us) {
            since_last_start_ms = (uint32_t)((now_us - s_last_start_response_us) / 1000ULL);
        }

        // Suppress if we responded to /start recently (within the cooldown window).
        if (s_last_start_response_us > 0 && since_last_start_ms < START_COMMAND_COOLDOWN_MS) {
            ESP_LOGW(TAG, "Suppressing repeated /start (%" PRIu32 "ms since last response)",
                     since_last_start_ms);
            metrics_log_request(&metrics, "start_suppressed");
            return;
        }

        s_last_start_response_us = now_us;  // Record this response time
        handle_start_command(reply_chat_id);
        metrics_log_request(&metrics, "start_handled");
        return;
    }

    // --- Duplicate plain-message suppression ---
    // If the exact same non-command message arrives within the replay cooldown,
    // drop it silently (protects against Telegram message delivery retries).
    if (is_non_command_message) {
        int64_t now_us = esp_timer_get_time();
        uint32_t since_last_ms = 0;

        if (s_last_non_command_response_us > 0 && now_us > s_last_non_command_response_us) {
            since_last_ms = (uint32_t)((now_us - s_last_non_command_response_us) / 1000ULL);
        }

        if (s_last_non_command_text[0] != '\0' &&
            strcmp(user_message, s_last_non_command_text) == 0 &&  // Same text
            s_last_non_command_response_us > 0 &&
            since_last_ms < MESSAGE_REPLAY_COOLDOWN_MS) {           // Within cooldown window
            ESP_LOGW(TAG, "Suppressing repeated message replay (%" PRIu32 "ms since last response)",
                     since_last_ms);
            metrics_log_request(&metrics, "replay_suppressed");
            return;
        }
    }

    // --- LLM request loop ---

    // Fetch the full list of registered tools to include in each LLM request.
    int tool_count;
    const tool_def_t *tools = tools_get_all(&tool_count);

    // Pause Telegram polling while we process, to avoid interleaved messages.
    telegram_pause_polling();
    telegram_polling_paused = true;

    // Record the user message in conversation history before the first LLM call.
    history_add("user", user_message, false, false, NULL, NULL);

    int rounds = 0;
    bool done  = false;

    // Tool-use loop: the LLM may request multiple tools before giving a final text response.
    // MAX_TOOL_ROUNDS caps this to prevent infinite loops.
    while (!done && rounds < MAX_TOOL_ROUNDS) {
        rounds++;
        metrics.rounds = rounds;

        // Serialise the full conversation history + system prompt into a JSON request body.
        // The user message is already in history, so we pass NULL for the extra message slot.
        char *request = json_build_request(
            agent_build_system_prompt(s_persona, s_system_prompt_buf, sizeof(s_system_prompt_buf)),
            s_history,
            s_history_len,
            NULL,       // User message already added to history above
            tools,
            tool_count
        );

        if (!request) {
            // JSON builder failed (likely OOM) — rollback history and abort.
            ESP_LOGE(TAG, "Failed to build request JSON");
            history_rollback_to(history_turn_start, "request build failed");
            send_response("Error: Failed to build request", reply_chat_id);
            telegram_resume_polling();
            telegram_polling_paused = false;
            metrics_log_request(&metrics, "request_build_error");
            return;
        }

        ESP_LOGI(TAG, "Request: %d bytes", (int)strlen(request));

        // Check the daily rate limit before spending an API call.
        char rate_reason[128];
        if (!ratelimit_check(rate_reason, sizeof(rate_reason))) {
            free(request);
            history_rollback_to(history_turn_start, "rate limited");
            send_response(rate_reason, reply_chat_id);  // rate_reason contains the human-readable message
            telegram_resume_polling();
            telegram_polling_paused = false;
            metrics_log_request(&metrics, "rate_limited");
            return;
        }

        // --- LLM request with exponential backoff retry ---
        esp_err_t err = ESP_FAIL;
        int retry_delay_ms = LLM_RETRY_BASE_MS;                    // Initial delay before first retry
        int64_t retry_window_started_us = esp_timer_get_time();    // Start of the retry budget window

        for (int retry = 0; retry < LLM_MAX_RETRIES; retry++) {
            // Check if we've already spent the total retry budget, before attempting.
            uint32_t retry_elapsed_ms = us_to_ms_u32(elapsed_us_since(retry_window_started_us));
            if (retry > 0 && retry_elapsed_ms >= LLM_RETRY_BUDGET_MS) {
                ESP_LOGW(TAG,
                         "LLM retry budget exhausted before attempt %d/%d (%" PRIu32 "ms/%dms)",
                         retry + 1, LLM_MAX_RETRIES, retry_elapsed_ms, LLM_RETRY_BUDGET_MS);
                break;
            }

            // Make the actual HTTP request to the LLM API.
            int64_t llm_started_us = esp_timer_get_time();
            err = llm_request(request, s_response_buf, sizeof(s_response_buf));
            metrics.llm_us_total += elapsed_us_since(llm_started_us);  // Track time spent
            metrics.llm_calls++;

            if (err == ESP_OK) {
                break;  // Success — no retry needed
            }

            if (retry == LLM_MAX_RETRIES - 1) {
                break;  // Last attempt — don't wait, just fall through to error handling
            }

            // Check budget again after the failed attempt.
            retry_elapsed_ms = us_to_ms_u32(elapsed_us_since(retry_window_started_us));
            if (retry_elapsed_ms >= LLM_RETRY_BUDGET_MS) {
                ESP_LOGW(TAG,
                         "LLM retry budget exhausted after attempt %d/%d (%" PRIu32 "ms/%dms)",
                         retry + 1, LLM_MAX_RETRIES, retry_elapsed_ms, LLM_RETRY_BUDGET_MS);
                break;
            }

            // Clamp the delay so we don't overshoot the remaining budget.
            uint32_t remaining_budget_ms = (uint32_t)(LLM_RETRY_BUDGET_MS - retry_elapsed_ms);
            int delay_ms = retry_delay_ms;
            if ((uint32_t)delay_ms > remaining_budget_ms) {
                delay_ms = (int)remaining_budget_ms;
            }

            if (delay_ms <= 0) {
                // Budget is essentially exhausted — no point waiting.
                ESP_LOGW(TAG,
                         "LLM retry budget left no delay before next attempt (%" PRIu32 "ms/%dms)",
                         retry_elapsed_ms, LLM_RETRY_BUDGET_MS);
                break;
            }

            ESP_LOGW(TAG,
                     "LLM request failed (attempt %d/%d), retrying in %dms (budget %" PRIu32 "/%dms)",
                     retry + 1, LLM_MAX_RETRIES, delay_ms, retry_elapsed_ms, LLM_RETRY_BUDGET_MS);

            // Yield the FreeRTOS task for the delay period (doesn't block other tasks).
            vTaskDelay(pdMS_TO_TICKS(delay_ms));

            // Exponential backoff: double the delay, but cap at LLM_RETRY_MAX_MS.
            retry_delay_ms *= 2;
            if (retry_delay_ms > LLM_RETRY_MAX_MS) {
                retry_delay_ms = LLM_RETRY_MAX_MS;
            }
        }

        free(request);  // Done with the request JSON regardless of outcome

        if (err != ESP_OK) {
            // All retries failed — give up on this turn.
            ESP_LOGE(TAG, "LLM request failed after %d retries", LLM_MAX_RETRIES);
            history_rollback_to(history_turn_start, "llm request failed");
            send_response("Error: Failed to contact LLM API after retries", reply_chat_id);
            telegram_resume_polling();
            telegram_polling_paused = false;
            metrics_log_request(&metrics, "llm_error");
            return;
        }

        // Record this successful API call for daily rate limiting.
        ratelimit_record_request();

        // --- Parse the LLM JSON response ---
        char text_out[MAX_MESSAGE_LEN] = {0};  // Will hold the assistant's text reply (if any)
        char tool_name[32]             = {0};  // Will hold the tool name (if the LLM wants one)
        char tool_id[64]               = {0};  // Will hold the tool call's unique ID
        cJSON *tool_input              = NULL; // Will hold the tool's JSON arguments

        if (!json_parse_response(s_response_buf, text_out, sizeof(text_out),
                                  tool_name, sizeof(tool_name),
                                  tool_id, sizeof(tool_id),
                                  &tool_input)) {
            ESP_LOGE(TAG, "Failed to parse response");
            history_rollback_to(history_turn_start, "llm response parse failed");
            send_response("Error: Failed to parse LLM response", reply_chat_id);
            json_free_parsed_response();
            telegram_resume_polling();
            telegram_polling_paused = false;
            metrics_log_request(&metrics, "parse_error");
            return;
        }

        // --- Branch: tool call vs. final text response ---
        if (tool_name[0] != '\0' && tool_input) {
            // The LLM wants to invoke a tool. Execute it and feed the result back.
            ESP_LOGI(TAG, "Tool call: %s (round %d)", tool_name, rounds);

            // Serialise the tool's JSON input arguments for history storage.
            char *input_str = cJSON_PrintUnformatted(tool_input);

            // Record the assistant's tool_use turn in history.
            history_add("assistant", input_str ? input_str : "{}",
                        true, false, tool_id, tool_name);
            free(input_str);

            // Check if this is a user-defined tool (custom action stored in flash).
            const user_tool_t *user_tool = user_tools_find(tool_name);
            metrics.tool_calls++;

            if (user_tool) {
                // User-defined tool: return the raw action string as an "instruction"
                // so the LLM executes the action via its next response.
                snprintf(s_tool_result_buf, sizeof(s_tool_result_buf),
                         "Execute this action now: %s", user_tool->action);
                ESP_LOGI(TAG, "User tool '%s' action: %s", tool_name, user_tool->action);

            } else if (is_cron_trigger && strcmp(tool_name, "cron_set") == 0) {
                // Prevent the LLM from scheduling a new cron job while *running* a scheduled task,
                // which would cause an infinite scheduling loop.
                snprintf(s_tool_result_buf, sizeof(s_tool_result_buf),
                         "Error: cron_set is not allowed during scheduled task execution. "
                         "Execute the scheduled action now instead of creating a new schedule.");
                ESP_LOGW(TAG, "Blocked cron_set during cron-triggered turn");

            } else {
                // Built-in tool: dispatch to the tools module and record timing.
                int64_t tool_started_us = esp_timer_get_time();
                bool tool_ok = tools_execute(tool_name, tool_input,
                                             s_tool_result_buf, sizeof(s_tool_result_buf));
                metrics.tool_us_total += elapsed_us_since(tool_started_us);

                // Keep the in-memory persona state in sync when the LLM changes it via tool.
                if (tool_ok && strcmp(tool_name, "set_persona") == 0) {
                    cJSON *persona_json = cJSON_GetObjectItem(tool_input, "persona");
                    agent_persona_t parsed_persona = AGENT_PERSONA_NEUTRAL;
                    if (persona_json && cJSON_IsString(persona_json) &&
                        agent_parse_persona_name(persona_json->valuestring, &parsed_persona)) {
                        s_persona = parsed_persona;  // Apply the new persona immediately
                    }
                } else if (tool_ok && strcmp(tool_name, "reset_persona") == 0) {
                    s_persona = AGENT_PERSONA_NEUTRAL;  // Reset to default
                }

                ESP_LOGI(TAG, "Tool result: %s", s_tool_result_buf);
            }

            // Feed the tool result back into history as a "user" role message
            // (Anthropic API convention: tool results are sent as user-role content).
            history_add("user", s_tool_result_buf, false, true, tool_id, NULL);

            json_free_parsed_response();
            // Loop continues — the LLM will now see the tool result and respond again.

        } else {
            // No tool call — the LLM produced a final text response. We are done.
            if (text_out[0] != '\0') {
                history_add("assistant", text_out, false, false, NULL, NULL);
                send_response(text_out, reply_chat_id);
            } else {
                // LLM returned an empty response — send a placeholder so the user isn't left hanging.
                history_add("assistant", "(No response from Claude)", false, false, NULL, NULL);
                send_response("(No response from Claude)", reply_chat_id);
            }
            json_free_parsed_response();
            done = true;  // Signal the loop to exit
        }
    }

    // If the loop exited because MAX_TOOL_ROUNDS was reached (not because done=true),
    // inform the user that the agent got stuck.
    if (!done) {
        ESP_LOGW(TAG, "Max tool rounds reached");
        history_add("assistant", "(Reached max tool iterations)", false, false, NULL, NULL);
        send_response("(Reached max tool iterations)", reply_chat_id);
        telegram_resume_polling();
        telegram_polling_paused = false;
        metrics_log_request(&metrics, "max_rounds");
        return;
    }

    // Successful completion: update the last-seen plain-message record for deduplication.
    if (is_non_command_message) {
        strncpy(s_last_non_command_text, user_message, sizeof(s_last_non_command_text) - 1);
        s_last_non_command_text[sizeof(s_last_non_command_text) - 1] = '\0';
        s_last_non_command_response_us = esp_timer_get_time();
    }

    // Resume Telegram polling now that we are done processing.
    if (telegram_polling_paused) {
        telegram_resume_polling();
    }

    metrics_log_request(&metrics, "success");
}

// ============================================================
// TEST BUILD helpers (compiled out in production)
// ============================================================
#ifdef TEST_BUILD

// Resets all module-level state to a clean starting point between unit tests.
void agent_test_reset(void)
{
    memset(s_history, 0, sizeof(s_history));          // Clear conversation history
    s_history_len = 0;
    memset(s_response_buf, 0, sizeof(s_response_buf));
    memset(s_tool_result_buf, 0, sizeof(s_tool_result_buf));
    s_channel_output_queue        = NULL;
    s_telegram_output_queue       = NULL;
    s_last_start_response_us      = 0;
    s_last_non_command_response_us= 0;
    memset(s_last_non_command_text, 0, sizeof(s_last_non_command_text));
    s_messages_paused = false;
    memset(s_test_persona_value, 0, sizeof(s_test_persona_value));
    local_admin_test_reset();     // Reset local_admin's state too
    load_persona_from_store();    // Re-apply persona from test store (will be empty after reset)
}

// Injects mock output queues so test code can inspect what the agent would send.
void agent_test_set_queues(QueueHandle_t channel_output_queue,
                           QueueHandle_t telegram_output_queue)
{
    s_channel_output_queue  = channel_output_queue;
    s_telegram_output_queue = telegram_output_queue;
}

// Simulates the agent receiving a plain USB channel message.
void agent_test_process_message(const char *user_message)
{
    process_message(user_message, MSG_SOURCE_CHANNEL, 0);
}

// Simulates the agent receiving a Telegram message from a specific chat.
void agent_test_process_message_for_chat(const char *user_message, int64_t reply_chat_id)
{
    process_message(user_message, MSG_SOURCE_TELEGRAM, reply_chat_id);
}
#endif // TEST_BUILD

// ============================================================
// FreeRTOS task entry point
// ============================================================

// The agent's background task loop.
// Blocks on the input queue indefinitely (portMAX_DELAY) and processes
// each message as it arrives. Never returns.
static void agent_task(void *arg)
{
    (void)arg;              // Unused — suppress compiler warning
    channel_msg_t msg;

    ESP_LOGI(TAG, "Agent task started");

    while (1) {
        // Block here until a message arrives in the input queue.
        if (xQueueReceive(s_input_queue, &msg, portMAX_DELAY) == pdTRUE) {
            // Determine the reply chat ID based on message source and route to process_message.
            process_message(msg.text, msg.source,
                            response_chat_id_for_source(msg.source, msg.chat_id));
        }
    }
}

// ============================================================
// Public API: agent_start
// ============================================================

// Initialises the agent and spawns its FreeRTOS task.
// Must be called once at startup, after all queues have been created.
// Returns ESP_OK on success, ESP_ERR_INVALID_ARG if queues are invalid,
// or ESP_ERR_NO_MEM if the task could not be created.
esp_err_t agent_start(QueueHandle_t input_queue,
                      QueueHandle_t channel_output_queue,
                      QueueHandle_t telegram_output_queue)
{
    // Validate required queues (telegram queue is optional).
    if (!input_queue || !channel_output_queue) {
        ESP_LOGE(TAG, "Invalid queues for agent startup");
        return ESP_ERR_INVALID_ARG;
    }

    // Store queue handles in module-level statics for use throughout the agent.
    s_input_queue           = input_queue;
    s_channel_output_queue  = channel_output_queue;
    s_telegram_output_queue = telegram_output_queue;

    // Restore persona from NVS so the agent starts with the last-saved personality.
    load_persona_from_store();

    // Create the agent FreeRTOS task.
    // AGENT_TASK_STACK_SIZE and AGENT_TASK_PRIORITY are defined in config.h.
    if (xTaskCreate(agent_task, "agent", AGENT_TASK_STACK_SIZE, NULL,
                    AGENT_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create agent task");
        return ESP_ERR_NO_MEM;  // xTaskCreate returns pdFAIL when heap is insufficient
    }

    ESP_LOGI(TAG, "Agent started");
    return ESP_OK;
}
