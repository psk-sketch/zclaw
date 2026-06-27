// Pull in project-wide compile-time configuration (pin numbers, queue sizes, feature flags, etc.)
#include "config.h"
// NVS-backed key-value storage (memory_init, memory_get, memory_factory_reset, etc.)
#include "memory.h"
// Communication channel abstraction (serial/USB input/output queue management)
#include "channel.h"
// AI agent logic — reads from the input queue, calls the LLM, dispatches tool calls
#include "agent.h"
// LLM (Large Language Model) client — sends prompts and receives responses
#include "llm.h"
// Tool registry — registers all built-in and user-defined tools the agent can invoke
#include "tools.h"
// Telegram bot integration — sends and receives messages via the Telegram API
#include "telegram.h"
// Cron scheduler — runs tools on a time-based schedule, includes NTP sync
#include "cron.h"
// Rate limiter — prevents the agent from being spammed with too many requests
#include "ratelimit.h"
// OTA (Over-The-Air) firmware update support — handles rollback and version confirmation
#include "ota.h"
// HTTP gateway — provides an HTTP API endpoint for interacting with the agent
#include "http_gate.h"
// Boot guard — tracks crash-loop boot counts and triggers safe mode if needed
#include "boot_guard.h"
// Local admin — handles serial/USB commands like /gpio, /diag, /reboot, /wifi
#include "local_admin.h"
// NVS key name constants — centralised strings used as keys in flash storage
#include "nvs_keys.h"
// Predefined message strings used in user-facing output
#include "messages.h"
// GPIO pin policy — checks whether a pin is safe to use before touching it
#include "gpio_policy.h"

// FreeRTOS core — task creation, scheduling, and delays
#include "freertos/FreeRTOS.h"
// FreeRTOS task API — xTaskCreate, vTaskDelay, vTaskDelete, etc.
#include "freertos/task.h"
// FreeRTOS queue API — xQueueCreate for passing messages between tasks
#include "freertos/queue.h"
// ESP-IDF logging macros — ESP_LOGI (info), ESP_LOGE (error), ESP_LOGW (warning)
#include "esp_log.h"
// ESP-IDF error type and ESP_ERROR_CHECK macro (aborts on non-ESP_OK result)
#include "esp_err.h"
// ESP system utilities — esp_restart() to reboot, esp_get_free_heap_size(), etc.
#include "esp_system.h"
// GPIO driver — gpio_reset_pin, gpio_set_direction, gpio_get_level, etc.
#include "driver/gpio.h"

// Tag used to prefix all log messages from this file (shows as "[main]" in the console)
static const char *TAG = "main";
// Global flag: set to true if the boot guard detects too many consecutive crashes
static bool s_safe_mode = false;

// Called when a critical subsystem fails to start up
// Logs the component name and error, waits 1 second, then reboots the device
static void fail_fast_startup(const char *component, esp_err_t err)
{
    ESP_LOGE(TAG, "Startup failure in %s: %s", component, esp_err_to_name(err));
    // Brief delay so the error message can flush to the serial console before reboot
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

// FreeRTOS task that runs after a stable boot window has passed
// Its job: confirm a pending OTA image as valid and reset the boot failure counter
static void clear_boot_count(void *arg)
{
    // Suppress unused parameter warning — no argument is passed to this task
    (void)arg;
    // Wait for the configured stable-boot window before doing anything
    vTaskDelay(pdMS_TO_TICKS(BOOT_SUCCESS_DELAY_MS));

    // Log the task's stack usage at start (useful for tuning stack size)
    UBaseType_t start_hwm_words = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "boot_ok stack high-water mark at start: %u words",
             (unsigned)start_hwm_words);

    // Check if there is an OTA image waiting to be confirmed as stable
    bool pending_before = ota_is_pending_verify();
    if (pending_before) {
        // Mark the OTA image as valid so the bootloader won't roll back to the previous firmware
        esp_err_t ota_err = ota_mark_valid_if_pending();
        if (ota_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to confirm OTA image: %s", esp_err_to_name(ota_err));
        } else {
            ESP_LOGI(TAG, "OTA image confirmed after stable boot window");
        }
    }

    // Reset the persistent boot counter to 0 — the system is considered stable
    esp_err_t boot_count_err = boot_guard_set_persisted_count(0);
    if (boot_count_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to clear boot counter: %s", esp_err_to_name(boot_count_err));
    } else {
        ESP_LOGI(TAG, "Boot counter cleared - system stable");
    }

    // Log stack usage again at exit to check for any stack growth during execution
    UBaseType_t end_hwm_words = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "boot_ok stack high-water mark before exit: %u words",
             (unsigned)end_hwm_words);

    // Self-delete: this task is done and should not run again
    vTaskDelete(NULL);
}

// Checks whether the factory reset button is being held at boot
// If held for FACTORY_RESET_HOLD_MS milliseconds, wipes NVS flash and reboots
static bool check_factory_reset(void)
{
    // First verify the factory reset pin is safe to use on this hardware target
    if (!gpio_policy_runtime_input_pin_is_safe(FACTORY_RESET_PIN)) {
        ESP_LOGW(TAG, "Skipping factory reset button check: pin %d is unsafe on this target", FACTORY_RESET_PIN);
        return false;
    }

    // Reset pin to default state, configure it as a digital input with internal pull-up
    // Pull-up means the pin reads HIGH when the button is open, LOW when pressed
    gpio_reset_pin(FACTORY_RESET_PIN);
    gpio_set_direction(FACTORY_RESET_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(FACTORY_RESET_PIN, GPIO_PULLUP_ONLY);

    // A LOW level means the button is currently pressed (active-low wiring)
    if (gpio_get_level(FACTORY_RESET_PIN) == 0) {
        ESP_LOGW(TAG, "Factory reset button detected, hold for 5 seconds...");

        // Poll every 100ms until the button is released or the hold time is reached
        int held_ms = 0;
        while (gpio_get_level(FACTORY_RESET_PIN) == 0 && held_ms < FACTORY_RESET_HOLD_MS) {
            vTaskDelay(pdMS_TO_TICKS(100));
            held_ms += 100;
        }

        // If the button was held for the full required duration, trigger the reset
        if (held_ms >= FACTORY_RESET_HOLD_MS) {
            ESP_LOGW(TAG, "Factory reset triggered!");
            // Erase all NVS data (WiFi credentials, persona, memory, cron jobs, etc.)
            ESP_ERROR_CHECK(memory_factory_reset());
            ESP_LOGI(TAG, "Factory reset complete, restarting...");
            // Brief pause so the log message is visible before reboot
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
            return true;
        }
    }
    return false;
}

// Returns true if the device has been provisioned with a WiFi SSID
// Checks NVS first, then falls back to a compile-time Kconfig value
static bool device_is_configured(void)
{
    char ssid[64] = {0};
    // Try to read the WiFi SSID from persistent NVS storage
    if (memory_get(NVS_KEY_WIFI_SSID, ssid, sizeof(ssid)) && ssid[0] != '\0') {
        return true;
    }

    // If not in NVS, check whether a compile-time SSID was baked in via menuconfig
#if defined(CONFIG_ZCLAW_WIFI_SSID)
    return CONFIG_ZCLAW_WIFI_SSID[0] != '\0';
#else
    // No SSID found in NVS or at compile time — device is unprovisioned
    return false;
#endif
}

// Prints a provisioning help message to the serial console when the device is unconfigured
static void print_provisioning_help(void)
{
    ESP_LOGE(TAG, "");
    ESP_LOGE(TAG, "========================================");
    ESP_LOGE(TAG, "  Device is not provisioned");
    ESP_LOGE(TAG, "========================================");
    ESP_LOGE(TAG, "Run on host:");
    // Instructs the developer to run the provisioning script with the correct serial port
    ESP_LOGE(TAG, "  ./scripts/provision.sh --port <serial-port>");
    ESP_LOGE(TAG, "Then restart the board.");
    ESP_LOGE(TAG, "");
}

// Main entry point — called by the ESP-IDF bootloader after hardware initialisation
// Sets up all subsystems in order and then hands off to the FreeRTOS scheduler
void app_main(void)
{
    // Print a startup banner showing the firmware version
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  zclaw v%s", ota_get_version());
    ESP_LOGI(TAG, "  AI Agent on ESP32");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "");

    // Step 1: Initialise NVS flash storage — required before reading any persisted config
    ESP_ERROR_CHECK(memory_init());
    // Initialise the HTTP gateway so local HTTP commands are available early
    ESP_ERROR_CHECK(http_gate_init());

    // Step 2: Initialise OTA subsystem — checks if a firmware update needs rollback confirmation
    ota_init();

    // Step 3: Check if the factory reset button is held at boot (skipped in emulator mode)
#if !CONFIG_ZCLAW_EMULATOR_MODE
    check_factory_reset();
#endif

    // Step 4: Boot loop protection — detect repeated crashes and enter safe mode if needed
#if !CONFIG_ZCLAW_EMULATOR_MODE
    // Read the number of consecutive boot attempts from NVS
    int boot_count = boot_guard_get_persisted_count();
    // Calculate what the count should be incremented to
    int next_boot_count = boot_guard_next_count(boot_count);
    // Persist the incremented count — if we crash again, this count will be seen next boot
    esp_err_t boot_count_err = boot_guard_set_persisted_count(next_boot_count);
    if (boot_count_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist boot counter: %s", esp_err_to_name(boot_count_err));
    }

    // If there have been too many consecutive failures, enter safe mode
    if (boot_guard_should_enter_safe_mode(boot_count, MAX_BOOT_FAILURES)) {
        ESP_LOGE(TAG, "");
        ESP_LOGE(TAG, "========================================");
        ESP_LOGE(TAG, "  SAFE MODE - Too many boot failures");
        ESP_LOGE(TAG, "  Hold BOOT button for factory reset");
        ESP_LOGE(TAG, "========================================");
        ESP_LOGE(TAG, "");
        s_safe_mode = true;
    }
#endif

    // Inform the local admin module whether we're in safe mode (affects available commands)
    local_admin_set_safe_mode(s_safe_mode);

// ── EMULATOR MODE ─────────────────────────────────────────────────────────────
// Emulator mode runs the agent on a host machine without real WiFi or Telegram.
// Input comes from the serial console; output goes back to serial.
#if CONFIG_ZCLAW_EMULATOR_MODE
    ESP_LOGW(TAG, "Emulator mode enabled: skipping WiFi/NTP/Telegram startup");
#ifndef CONFIG_ZCLAW_STUB_LLM
    // Warn that without a stub LLM, real network requests will be needed (may fail offline)
    ESP_LOGW(TAG, "Stub LLM is disabled; without network, LLM requests may fail");
#endif

    // Initialise the LLM client, rate limiter, tools, and channel in emulator mode
    ESP_ERROR_CHECK(llm_init());
    ratelimit_init();
    tools_init();
    channel_init();

    // Create message queues to connect the channel and agent tasks
    QueueHandle_t input_queue = xQueueCreate(INPUT_QUEUE_LENGTH, sizeof(channel_msg_t));
    QueueHandle_t channel_output_queue = xQueueCreate(OUTPUT_QUEUE_LENGTH, sizeof(channel_output_msg_t));
    if (!input_queue || !channel_output_queue) {
        ESP_LOGE(TAG, "Failed to create emulator queues");
        esp_restart();
    }

    // Start the channel task (reads serial input, writes to input_queue)
    esp_err_t startup_err = channel_start(input_queue, channel_output_queue);
    if (startup_err != ESP_OK) {
        fail_fast_startup("channel_start", startup_err);
    }

    // Start the agent task (reads from input_queue, calls LLM and tools, sends output)
    // No Telegram output queue in emulator mode (NULL)
    startup_err = agent_start(input_queue, channel_output_queue, NULL);
    if (startup_err != ESP_OK) {
        fail_fast_startup("agent_start", startup_err);
    }

    // Print a ready message to the serial console to signal that the emulator is running
    channel_write("\r\nzclaw emulator ready. Type a message and press Enter.\r\n\r\n");

    // Keep app_main alive — actual work happens in agent and channel FreeRTOS tasks
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

// ── NORMAL (DEVICE) MODE ──────────────────────────────────────────────────────
#else

    // Check whether WiFi credentials are available (NVS or compile-time config)
    bool device_configured = device_is_configured();
    // Tell local admin so it can adjust its behaviour for unconfigured state
    local_admin_set_device_configured(device_configured);

    // Step 5: Initialise the LLM client (needed even before WiFi for local serial commands)
    ESP_ERROR_CHECK(llm_init());

    // Step 6: Initialise the rate limiter to throttle incoming agent requests
    ratelimit_init();

    // Step 7: Initialise Telegram (non-fatal if token is not yet provisioned)
#if CONFIG_ZCLAW_STUB_TELEGRAM
    // Stub mode replaces real Telegram with a no-op for testing without a bot token
    ESP_LOGW(TAG, "Telegram stub mode enabled; skipping Telegram startup");
#else
    esp_err_t telegram_init_err = telegram_init();
    // ESP_ERR_NOT_FOUND means no token is stored yet — that's acceptable at this point
    if (telegram_init_err != ESP_OK && telegram_init_err != ESP_ERR_NOT_FOUND) {
        fail_fast_startup("telegram_init", telegram_init_err);
    }
#endif

    // Step 8: Register all tools and initialise the local serial channel
    // Done early so /gpio and /diag commands work even before WiFi comes up
    tools_init();
    channel_init();

    // Create the inter-task message queues
    // input_queue: receives messages from channel/Telegram to feed to the agent
    QueueHandle_t input_queue = xQueueCreate(INPUT_QUEUE_LENGTH, sizeof(channel_msg_t));
    // channel_output_queue: agent sends serial responses here for the channel task to write
    QueueHandle_t channel_output_queue = xQueueCreate(OUTPUT_QUEUE_LENGTH, sizeof(channel_output_msg_t));
    // telegram_output_queue: only created if Telegram is enabled
    QueueHandle_t telegram_output_queue = NULL;

#if CONFIG_ZCLAW_STUB_TELEGRAM
    // Telegram stub — always treat Telegram as disabled
    bool telegram_enabled = false;
#else
    // Telegram is only enabled if: device is configured, not in safe mode, and a bot token exists
    bool telegram_enabled = device_configured && !s_safe_mode && telegram_is_configured();
#endif

    // Only create the Telegram output queue if the Telegram channel is active
    if (telegram_enabled) {
        telegram_output_queue = xQueueCreate(TELEGRAM_OUTPUT_QUEUE_LENGTH, sizeof(telegram_msg_t));
    }

    // Abort if any required queue failed to allocate (out of heap memory)
    if (!input_queue || !channel_output_queue || (telegram_enabled && !telegram_output_queue)) {
        ESP_LOGE(TAG, "Failed to create queues");
        esp_restart();
    }

    // Start the local serial channel task
    esp_err_t startup_err = channel_start(input_queue, channel_output_queue);
    if (startup_err != ESP_OK) {
        fail_fast_startup("channel_start", startup_err);
    }

    // Start the agent task — this is the core AI loop
    // Passes all three queues; agent decides which output queue(s) to use per message
    startup_err = agent_start(input_queue, channel_output_queue, telegram_output_queue);
    if (startup_err != ESP_OK) {
        fail_fast_startup("agent_start", startup_err);
    }

    // Step 9: Handle unprovisioned or safe-mode states — don't proceed to WiFi
    if (!device_configured || s_safe_mode) {
        if (s_safe_mode) {
            // Safe mode: too many consecutive boot failures detected
            // Only local serial commands remain functional; no WiFi, no Telegram
            ESP_LOGE(TAG, "");
            ESP_LOGE(TAG, "========================================");
            ESP_LOGE(TAG, "  SAFE MODE - Too many boot failures");
            ESP_LOGE(TAG, "  Hold BOOT button for factory reset");
            ESP_LOGE(TAG, "========================================");
            ESP_LOGE(TAG, "");
            ESP_LOGE(TAG, "Recovery options:");
            ESP_LOGE(TAG, "  1) Hold BOOT for factory reset");
            ESP_LOGE(TAG, "  2) Reflash firmware and reprovision");
            ESP_LOGE(TAG, "");
            // Inform the user via the serial channel which commands are available
            channel_write("\r\nSAFE MODE - local serial commands remain available.\r\n"
                          "Try /gpio, /diag, /reboot, /wifi, /bootcount, /factory-reset, /help, or /settings.\r\n\r\n");
        } else {
            // Device hasn't been provisioned yet — print setup instructions
            print_provisioning_help();
            channel_write("\r\nDevice is not provisioned.\r\n"
                          "Local serial commands remain available: /gpio, /diag, /reboot, /wifi, /bootcount, /factory-reset, /help, /settings.\r\n\r\n");
        }
        // Hang here — agent and channel tasks are still running for local serial access
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }

    // Step 10: Connect to WiFi using the SSID and password stored in NVS
    if (!local_admin_wifi_connect_from_store()) {
        ESP_LOGE(TAG, "WiFi failed, restarting...");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }

    // Step 11: Spawn a background task to confirm stable boot after the observation window
    // This task will reset the boot counter and confirm any pending OTA image
    if (xTaskCreate(clear_boot_count, "boot_ok", BOOT_OK_TASK_STACK_SIZE, NULL, 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create boot confirmation task");
    }

    // Step 12: Initialise the cron scheduler (also performs NTP time sync over WiFi)
    ESP_ERROR_CHECK(cron_init());

    // Step 13: Start the Telegram polling task if Telegram is enabled
    if (telegram_enabled) {
        startup_err = telegram_start(input_queue, telegram_output_queue);
        if (startup_err != ESP_OK) {
            fail_fast_startup("telegram_start", startup_err);
        }
    }

    // Step 14: Start the cron task — it will fire scheduled jobs into the input queue
    startup_err = cron_start(input_queue);
    if (startup_err != ESP_OK) {
        fail_fast_startup("cron_start", startup_err);
    }

    // Step 15: Print a ready banner with current heap usage
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Ready! Free heap: %lu bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "");

    // Step 16: Send a startup notification message via Telegram to the configured chat
    if (telegram_enabled && telegram_is_configured()) {
        telegram_send_startup();
    }

    // app_main returns here — the FreeRTOS scheduler takes over and runs all created tasks
#endif
}
