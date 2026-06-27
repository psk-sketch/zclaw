/* Include the tool definitions header (tool_def_t struct, function prototypes, etc.) */
#include "tools.h"
/* Include handlers for built-in tools (the actual execute functions) */
#include "tools_handlers.h"
/* Include user-defined tool support (tools stored in NVS flash) */
#include "user_tools.h"
/* Include ESP-IDF logging macros (ESP_LOGI, ESP_LOGE, etc.) */
#include "esp_log.h"
/* Standard string functions (strcmp, etc.) */
#include <string.h>
/* Standard I/O functions (snprintf, etc.) */
#include <stdio.h>

/* Tag used to identify this module in ESP log output */
static const char *TAG = "tools";

// -----------------------------------------------------------------------------
// Tool Registry
// -----------------------------------------------------------------------------

/*
 * TOOL_ENTRY macro: expands each entry in builtin_tools.def into a
 * tool_def_t struct initialiser with four fields:
 *   - name:             the tool's identifier string
 *   - description:      human-readable description of what the tool does
 *   - input_schema_json: JSON schema string describing expected input
 *   - execute:          function pointer to the tool's handler
 * The trailing comma makes it valid as an array element.
 */
#define TOOL_ENTRY(tool_name, tool_description, tool_schema, tool_execute) \
    { \
        .name = tool_name, \
        .description = tool_description, \
        .input_schema_json = tool_schema, \
        .execute = tool_execute \
    },

/*
 * s_tools[]: static array of all built-in tool definitions.
 * Populated at compile time by including builtin_tools.def, which expands
 * each TOOL_ENTRY(...) macro call into a struct initialiser (see macro above).
 */
static const tool_def_t s_tools[] = {
#include "builtin_tools.def"
};

/* Undefine TOOL_ENTRY now that the array is populated — prevents accidental
   reuse of the macro elsewhere in this translation unit */
#undef TOOL_ENTRY

/* Total number of built-in tools, computed at compile time from the array size */
static const int s_tool_count = sizeof(s_tools) / sizeof(s_tools[0]);

/*
 * tools_init() — one-time initialisation called at startup.
 * Initialises user-defined tools from NVS, then logs a summary of all
 * registered built-in and user tools.
 */
void tools_init(void)
{
    /* Load and register user-defined tools stored in non-volatile storage */
    user_tools_init();

    /* Log how many built-in tools and user tools are registered in total */
    ESP_LOGI(TAG, "Registered %d built-in tools, %d user tools",
             s_tool_count, user_tools_count());

    /* Loop through each built-in tool and log its name for diagnostics */
    for (int i = 0; i < s_tool_count; i++) {
        ESP_LOGI(TAG, "  %s", s_tools[i].name);
    }
}

/*
 * tools_get_all() — returns a pointer to the built-in tool array.
 * Writes the number of tools into *count so callers know the array length.
 * The returned pointer is to a static const array — do not modify it.
 */
const tool_def_t *tools_get_all(int *count)
{
    /* Write the tool count out through the caller-supplied pointer */
    *count = s_tool_count;
    /* Return the base address of the static tool array */
    return s_tools;
}

/*
 * tools_execute() — finds a tool by name and runs it.
 *
 * Parameters:
 *   name       — null-terminated string identifying the tool to run
 *   input      — parsed cJSON object containing the tool's input arguments
 *   result     — caller-supplied buffer to receive the output string
 *   result_len — size of the result buffer (prevents overflow)
 *
 * Returns true if the tool was found and executed successfully, false otherwise.
 */
bool tools_execute(const char *name, const cJSON *input, char *result, size_t result_len)
{
    /* Linear search through the built-in tool registry for a name match */
    for (int i = 0; i < s_tool_count; i++) {
        /* Compare the requested name against this tool's registered name */
        if (strcmp(s_tools[i].name, name) == 0) {
            /* Log which tool is being executed for tracing/debugging */
            ESP_LOGI(TAG, "Exec: %s", name);
            /* Dispatch to the tool's execute function and return its result */
            return s_tools[i].execute(input, result, result_len);
        }
    }
    /* No matching tool found — write an error message into the result buffer */
    snprintf(result, result_len, "Unknown tool: %s", name);
    /* Return false to signal that execution failed */
    return false;
}
