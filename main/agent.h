#ifndef AGENT_H             // Include guard: prevents this header from being compiled twice
#define AGENT_H

#include "esp_err.h"        // ESP-IDF error type: esp_err_t (e.g. ESP_OK, ESP_FAIL)
#include "freertos/FreeRTOS.h" // FreeRTOS core: required before any other FreeRTOS headers
#include "freertos/queue.h" // FreeRTOS queue API: QueueHandle_t, xQueueSend, xQueueReceive
#include <stdint.h>         // Fixed-width integer types: uint8_t, int64_t, etc.

// Starts the agent background task.
// input_queue          — the queue the agent reads incoming messages from
// channel_output_queue — the queue the agent writes responses to (USB serial channel)
// telegram_output_queue— the queue the agent writes responses to (Telegram)
// Returns ESP_OK on success, or an error code on failure.
esp_err_t agent_start(QueueHandle_t input_queue,
                      QueueHandle_t channel_output_queue,
                      QueueHandle_t telegram_output_queue);

#ifdef TEST_BUILD
// The following functions are compiled ONLY in test builds (not in production firmware).
// They allow unit tests to drive agent logic directly without spawning real FreeRTOS tasks.

// Resets all agent state (history, buffers, queues, persona) back to a clean slate.
void agent_test_reset(void);

// Injects mock output queues so tests can capture what the agent would send.
void agent_test_set_queues(QueueHandle_t channel_output_queue,
                           QueueHandle_t telegram_output_queue);

// Simulates receiving a message via the USB channel (MSG_SOURCE_CHANNEL).
void agent_test_process_message(const char *user_message);

// Simulates receiving a message via Telegram, targeting a specific chat ID for replies.
void agent_test_process_message_for_chat(const char *user_message, int64_t reply_chat_id);
#endif

#endif // AGENT_H           // End of include guard
