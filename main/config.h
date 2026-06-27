/* ============================================================
 * config.h
 *
 * Central configuration header for "zclaw" — an AI agent
 * firmware that runs on an ESP32 microcontroller under FreeRTOS.
 *
 * This file is the single source of truth for every tunable
 * constant in the system: memory sizes, task priorities, API
 * endpoints, GPIO safety limits, NVS keys, Telegram settings,
 * scheduling, rate-limiting, and more.
 *
 * Nothing in here is executable code — it is purely #define
 * constants, a single enum, and a few compile-time safety checks.
 * Every other .c/.h file in the project #includes this header.
 * ============================================================ */

/* Standard include guard — prevents this file being processed
 * more than once if multiple source files include it.          */
#ifndef CONFIG_H
#define CONFIG_H


// =============================================================
// Buffer Sizes
// =============================================================
// How many bytes to allocate for each data buffer.
// Sized to fit the largest realistic payload while staying
// within the ESP32's limited RAM (~400 KB total DRAM).

#define LLM_REQUEST_BUF_SIZE    12288   // 12 KB: JSON body sent TO the LLM API
#define LLM_RESPONSE_BUF_SIZE   16384   // 16 KB: JSON body received FROM the LLM API (larger because responses can be verbose)
#define CHANNEL_RX_BUF_SIZE     512     // 512 B: one line of input from serial port or web relay
#define CHANNEL_TX_BUF_SIZE     1024    // 1 KB: outbound response text going back to serial/web relay
#define TOOL_RESULT_BUF_SIZE    512     // 512 B: the result string returned by a tool handler (e.g. "Pin 4 = HIGH")


// =============================================================
// Conversation History
// =============================================================
// Controls how much chat context is kept in RAM and sent to
// the LLM on each request.  More turns = better context but
// higher RAM and token usage.

#define MAX_HISTORY_TURNS       12      // Keep at most 12 user/assistant back-and-forth pairs
#define MAX_MESSAGE_LEN         1024    // Each individual message (user or assistant) capped at 1 KB


// =============================================================
// Agent Loop
// =============================================================
// The agent loop lets the LLM call tools repeatedly in one
// "session" (e.g. read a pin, decide, write a pin).
// This cap prevents runaway loops that drain tokens or hang.

#define MAX_TOOL_ROUNDS         5       // LLM may call tools at most 5 times before the loop is force-stopped


// =============================================================
// FreeRTOS Tasks
// =============================================================
// Each task runs concurrently on FreeRTOS.  Stack size is in
// bytes; priority is a small integer (higher = more CPU time).
// All stacks must fit comfortably inside available heap.

#define AGENT_TASK_STACK_SIZE   8192    // 8 KB stack for the main AI agent task (needs most room: JSON, HTTP, tool calls)
#define CHANNEL_TASK_STACK_SIZE 4096    // 4 KB stack for the input/output channel task (serial or web relay)
#define CRON_TASK_STACK_SIZE    4096    // 4 KB stack for the cron/scheduler task
#define BOOT_OK_TASK_STACK_SIZE 4096    // 4 KB stack for the boot-success watchdog task
#define AGENT_TASK_PRIORITY     5       // Agent and channel share the same priority — both are user-facing
#define CHANNEL_TASK_PRIORITY   5       // Same as agent; neither starves the other
#define CRON_TASK_PRIORITY      4       // Cron is slightly lower priority — time-triggered, not interactive


// =============================================================
// Queues
// =============================================================
// FreeRTOS queues decouple producers from consumers.
// LENGTH is the number of items (messages/strings) the queue
// can hold before the sender must block or drop.

#define INPUT_QUEUE_LENGTH      8       // Up to 8 incoming messages waiting to be processed by the agent
#define OUTPUT_QUEUE_LENGTH     8       // Up to 8 outgoing responses waiting to be sent back to the user
#define TELEGRAM_OUTPUT_QUEUE_LENGTH 4  // Smaller queue for Telegram replies (TLS overhead limits throughput)


// =============================================================
// LLM Backend Configuration
// =============================================================
// zclaw supports multiple LLM providers.  At runtime one
// backend is selected; the rest are inactive.

// Enum tags each supported backend with a stable integer ID
typedef enum {
    LLM_BACKEND_ANTHROPIC  = 0,   // Claude models via Anthropic's API
    LLM_BACKEND_OPENAI     = 1,   // GPT models via OpenAI's API
    LLM_BACKEND_OPENROUTER = 2,   // Any model via OpenRouter's unified proxy API
    LLM_BACKEND_OLLAMA     = 3,   // Local/self-hosted models via Ollama
} llm_backend_t;

// Base URL for each backend's messages/completions endpoint
#define LLM_API_URL_ANTHROPIC   "https://api.anthropic.com/v1/messages"
#define LLM_API_URL_OPENAI      "https://api.openai.com/v1/chat/completions"
#define LLM_API_URL_OPENROUTER  "https://openrouter.ai/api/v1/chat/completions"
// Ollama defaults to localhost — intended to be overridden at provisioning time
// for whatever host/port the local Ollama server is actually running on.
#define LLM_API_URL_OLLAMA      "http://127.0.0.1:11434/v1/chat/completions"

// Default model string sent in the API request body for each backend
#define LLM_DEFAULT_MODEL_ANTHROPIC   "claude-sonnet-4-6"    // Anthropic mid-tier model
#define LLM_DEFAULT_MODEL_OPENAI      "gpt-5.4"              // OpenAI model
#define LLM_DEFAULT_MODEL_OPENROUTER  "openrouter/auto"      // Let OpenRouter pick the best available model
#define LLM_DEFAULT_MODEL_OLLAMA      "qwen3:8b"             // 8-billion-parameter local model

// API key storage sizing
#define LLM_API_KEY_MAX_LEN       511                         // Max characters in an API key (no null terminator)
#define LLM_API_KEY_BUF_SIZE      (LLM_API_KEY_MAX_LEN + 1)  // +1 for the null terminator
// "Bearer " prefix (7 chars) + key + null terminator = full Authorization header value
#define LLM_AUTH_HEADER_BUF_SIZE  (sizeof("Bearer ") - 1 + LLM_API_KEY_MAX_LEN + 1)

#define LLM_MAX_TOKENS          1024    // Cap on tokens the LLM may generate per response (controls cost & RAM)
#define HTTP_TIMEOUT_MS         30000   // 30 s: general HTTP request timeout (non-LLM calls)
#define LLM_HTTP_TIMEOUT_MS     20000   // 20 s: tighter timeout specifically for LLM API calls
#define LLM_MAX_RETRIES         3       // Total attempts per LLM round (1 initial + 2 retries)
#define LLM_RETRY_BASE_MS       2000    // First retry waits 2 s (exponential back-off base)
#define LLM_RETRY_MAX_MS        10000   // Back-off is capped at 10 s per retry interval
#define LLM_RETRY_BUDGET_MS     45000   // Hard wall-clock limit: give up retrying after 45 s total


// =============================================================
// System Prompt
// =============================================================
// The fixed instruction block prepended to every conversation
// so the LLM knows its identity, constraints, and rules.
// Written as a multi-line string macro using backslash continuation.

#define SYSTEM_PROMPT \
    "You are zclaw, an AI agent running on an ESP32 with 400KB RAM and FreeRTOS. " \
    /* Reminds the model it is on-device, not a cloud chatbot */                    \
    "You run on the device, not in a cloud session. "                               \
    /* Keeps responses short to save tokens and fit small display buffers */        \
    "Be concise. Return plain text only, never markdown. "                          \
    /* Lists the categories of built-in tools available */                          \
    "Use tools to control hardware, memory, schedules, personas, and custom tools. "\
    /* Performance hint: batch GPIO reads into one call instead of many */          \
    "When asked for multiple GPIO states, prefer one gpio_read_all call. "          \
    /* Prevents the model from spontaneously switching personality */               \
    "Use persona tools only when the user explicitly asks to view, set, or reset persona. " \
    "Do not change persona from casual wording. "                                   \
    /* Forces the model to verify device state via tools, not from memory */        \
    "When asked what is saved or set on the device, verify with tools. "            \
    /* Ensures dynamic/custom tool results are acted upon, not just reported */     \
    "When a custom tool returns an action, carry it out with built-in tools."


// =============================================================
// GPIO Tool Safety Range  (configurable via Kconfig / menuconfig)
// =============================================================
// These defines set which GPIO pin numbers the AI is allowed
// to touch.  They can be overridden at build time through
// ESP-IDF's Kconfig system (sdkconfig / menuconfig) so the
// same firmware can be safely flashed to different boards.

// Minimum allowed GPIO pin number
#ifdef CONFIG_ZCLAW_GPIO_MIN_PIN
#define GPIO_MIN_PIN            CONFIG_ZCLAW_GPIO_MIN_PIN   // Use the Kconfig value if set
#else
#define GPIO_MIN_PIN            2                           // Default: start at pin 2 (avoid boot-strapping pins 0 & 1)
#endif

// Maximum allowed GPIO pin number
#ifdef CONFIG_ZCLAW_GPIO_MAX_PIN
#define GPIO_MAX_PIN            CONFIG_ZCLAW_GPIO_MAX_PIN   // Use the Kconfig value if set
#else
#define GPIO_MAX_PIN            10                          // Default: stop at pin 10
#endif

// Optional comma-separated list of specific allowed pins
// If non-empty this overrides the MIN/MAX range scan in gpio_read_all.
#ifdef CONFIG_ZCLAW_GPIO_ALLOWED_PINS
#define GPIO_ALLOWED_PINS_CSV   CONFIG_ZCLAW_GPIO_ALLOWED_PINS  // e.g. "4,5,18,19"
#else
#define GPIO_ALLOWED_PINS_CSV   ""                              // Empty = use MIN/MAX range instead
#endif

// Compile-time sanity check: catch a mis-configured range before linking
#if GPIO_MIN_PIN > GPIO_MAX_PIN
#error "GPIO_MIN_PIN must be <= GPIO_MAX_PIN"
#endif


// =============================================================
// NVS (Non-Volatile Storage — flash-backed key/value store)
// =============================================================
// NVS is the ESP-IDF equivalent of EEPROM: survives power loss.
// Different namespaces act like separate "folders" in flash,
// preventing key collisions between subsystems.

#define NVS_NAMESPACE           "zclaw"       // Default namespace for general agent data
#define NVS_NAMESPACE_CRON      "zc_cron"     // Namespace for scheduled cron entries
#define NVS_NAMESPACE_TOOLS     "zc_tools"    // Namespace for user-registered dynamic tools
#define NVS_NAMESPACE_CONFIG    "zc_config"   // Namespace for runtime config (API keys, model, timezone, etc.)
#define NVS_MAX_KEY_LEN         15            // NVS hardware limit: keys must be ≤ 15 characters
#define NVS_MAX_VALUE_LEN       512           // Max bytes for a single NVS string value (tool/cron definitions)


// =============================================================
// WiFi
// =============================================================

#define WIFI_MAX_RETRY          10            // Try to (re)connect up to 10 times before giving up
#define WIFI_RETRY_DELAY_MS     1000          // Wait 1 second between each retry attempt


// =============================================================
// Telegram Bot Integration
// =============================================================
// zclaw can receive user messages and send replies via a
// Telegram bot using long-polling (no inbound server needed).

#define TELEGRAM_API_URL        "https://api.telegram.org/bot"  // Base URL; bot token is appended at runtime

#define TELEGRAM_POLL_TIMEOUT   30      // Default long-poll window: ask Telegram to hold the connection open 30 s
                                        // before returning an empty response if no messages arrived

// OpenRouter's TLS handshake is heavier; a shorter poll window
// reduces the chance of two simultaneous HTTPS connections
// exhausting heap on RAM-limited targets.
#define TELEGRAM_POLL_TIMEOUT_OPENROUTER 8   // 8 s poll window when using the OpenRouter backend

// Classic ESP32 (single-core, 520 KB RAM) can run out of heap if
// Telegram's TLS session overlaps with an outbound LLM HTTPS call.
// Shorter poll window = connections are less likely to overlap.
#define TELEGRAM_POLL_TIMEOUT_ESP32 5        // 5 s poll window on the classic ESP32 target

#define TELEGRAM_POLL_INTERVAL  100     // ms to wait between retrying after a failed poll (error back-off)
#define TELEGRAM_MAX_MSG_LEN    4096    // Discard or truncate Telegram messages longer than 4 KB
#define TELEGRAM_FLUSH_ON_START 1       // 1 = discard any messages that arrived while the device was offline at startup

// Anti-spam / de-duplication
#define TELEGRAM_STALE_POLL_LOG_INTERVAL    4   // Log a "stale poll" warning only every 4th consecutive empty poll
#define TELEGRAM_STALE_POLL_RESYNC_STREAK   8   // After 8 consecutive stale polls, trigger an offset resync
#define TELEGRAM_STALE_POLL_RESYNC_COOLDOWN_MS 60000  // But don't resync more often than once per minute

#define START_COMMAND_COOLDOWN_MS   30000   // Ignore /start commands repeated within 30 s (debounce Telegram glitches)
#define MESSAGE_REPLAY_COOLDOWN_MS  20000   // Suppress identical non-command messages repeated within 20 s


// =============================================================
// Cron / Scheduler
// =============================================================
// A lightweight time-based task scheduler stored in NVS.
// The cron task wakes up periodically and fires any entries
// whose schedule matches the current time.

#define CRON_CHECK_INTERVAL_MS  10000   // Wake the cron task every 10 seconds to check schedules
#define CRON_MAX_ENTRIES        16      // At most 16 scheduled tasks can be stored
#define CRON_MAX_ACTION_LEN     256     // Each action string (what to do when triggered) is at most 256 bytes


// =============================================================
// Factory Reset
// =============================================================
// Holding a physical button LOW for long enough wipes NVS and
// reboots into a clean state — a hardware escape hatch.

#ifdef CONFIG_ZCLAW_FACTORY_RESET_PIN
#define FACTORY_RESET_PIN       CONFIG_ZCLAW_FACTORY_RESET_PIN   // Board-specific pin from Kconfig
#else
#define FACTORY_RESET_PIN       9       // Default: GPIO 9 — hold LOW to trigger reset
#endif

#ifdef CONFIG_ZCLAW_FACTORY_RESET_HOLD_MS
#define FACTORY_RESET_HOLD_MS   CONFIG_ZCLAW_FACTORY_RESET_HOLD_MS  // Configurable hold time
#else
#define FACTORY_RESET_HOLD_MS   5000    // Default: must hold the button for 5 full seconds
#endif


// =============================================================
// NTP (Network Time Protocol — clock synchronisation)
// =============================================================
// The ESP32 has no real-time clock battery; it must sync time
// over WiFi after each boot so cron schedules are correct.

#define NTP_SERVER              "pool.ntp.org"  // Public NTP pool — resolves to a nearby stratum-2 server
#define NTP_SYNC_TIMEOUT_MS     10000           // Give up waiting for NTP sync after 10 seconds
#define DEFAULT_TIMEZONE_POSIX  "UTC0"          // Default timezone: UTC with no DST offset
#define TIMEZONE_MAX_LEN        64              // Max length of a POSIX timezone string (e.g. "IST-5:30")


// =============================================================
// Dynamic Tools
// =============================================================
// Users can register their own custom tools at runtime (stored
// in NVS). These are loaded and exposed to the LLM alongside
// the built-in tools.

#define MAX_DYNAMIC_TOOLS       8       // At most 8 user-defined tools can be registered simultaneously
#define TOOL_NAME_MAX_LEN       24      // Tool names are at most 24 characters (e.g. "read_temperature")
#define TOOL_DESC_MAX_LEN       128     // Tool descriptions sent to the LLM are at most 128 characters


// =============================================================
// GPIO Mapping Subsystem
// =============================================================
// Constants for the persistent GPIO mapping subsystem.
#define GPIO_MAPPING_KEY_PREFIX   "u_gpio_"
#define GPIO_MAPPING_MAX_NAME_LEN 16


// =============================================================
// Boot Loop Protection
// =============================================================
// If the firmware crashes repeatedly on startup, it enters a
// minimal "safe mode" so the user can recover via serial.

#define MAX_BOOT_FAILURES       4       // Enter safe mode after 4 consecutive crash-reboots
#define BOOT_SUCCESS_DELAY_MS   30000   // If the device stays up for 30 s, reset the crash counter to 0


// =============================================================
// Rate Limiting
// =============================================================
// Prevents runaway usage (or abuse via Telegram) from burning
// through API quota or overheating the device with requests.

#define RATELIMIT_MAX_PER_HOUR      100     // At most 100 LLM API calls in any 60-minute window
#define RATELIMIT_MAX_PER_DAY       1000    // At most 1000 LLM API calls in any 24-hour window
#define RATELIMIT_ENABLED           1       // Master switch: set to 0 to disable all rate limiting (dev/test)


#endif // CONFIG_H  ← closes the #ifndef CONFIG_H include guard at the top
