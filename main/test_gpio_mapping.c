#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <assert.h>

// Mock esp_err_t
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_SIZE 0x103
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NVS_NOT_FOUND 0x1101

// Mock logging
#define ESP_LOGI(tag, fmt, ...) printf("[INFO][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[WARN][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[ERROR][%s] " fmt "\n", tag, ##__VA_ARGS__)

// Mock FreeRTOS
typedef void* SemaphoreHandle_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) (void)mux
#define portEXIT_CRITICAL(mux) (void)mux
#define pdMS_TO_TICKS(ms) ms
#define pdTRUE 1
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (SemaphoreHandle_t)1; }
int xSemaphoreTake(SemaphoreHandle_t lock, int ticks) { (void)lock; (void)ticks; return pdTRUE; }
void xSemaphoreGive(SemaphoreHandle_t lock) { (void)lock; }
void vSemaphoreDelete(SemaphoreHandle_t lock) { (void)lock; }

// Mock NVS Key-Value storage
typedef struct {
    char key[32];
    char value[512];
    bool active;
} mock_nvs_entry_t;

#define MAX_MOCK_NVS_ENTRIES 64
mock_nvs_entry_t mock_nvs[MAX_MOCK_NVS_ENTRIES];

void reset_mock_nvs(void) {
    memset(mock_nvs, 0, sizeof(mock_nvs));
}

esp_err_t memory_set(const char *key, const char *value) {
    if (strlen(key) > 15) {
        return ESP_ERR_INVALID_SIZE; // Matches ESP-IDF NVS key limit
    }
    for (int i = 0; i < MAX_MOCK_NVS_ENTRIES; i++) {
        if (mock_nvs[i].active && strcmp(mock_nvs[i].key, key) == 0) {
            strncpy(mock_nvs[i].value, value, sizeof(mock_nvs[i].value) - 1);
            return ESP_OK;
        }
    }
    for (int i = 0; i < MAX_MOCK_NVS_ENTRIES; i++) {
        if (!mock_nvs[i].active) {
            strncpy(mock_nvs[i].key, key, sizeof(mock_nvs[i].key) - 1);
            strncpy(mock_nvs[i].value, value, sizeof(mock_nvs[i].value) - 1);
            mock_nvs[i].active = true;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

bool memory_get(const char *key, char *value, size_t max_len) {
    for (int i = 0; i < MAX_MOCK_NVS_ENTRIES; i++) {
        if (mock_nvs[i].active && strcmp(mock_nvs[i].key, key) == 0) {
            strncpy(value, mock_nvs[i].value, max_len - 1);
            value[max_len - 1] = '\0';
            return true;
        }
    }
    return false;
}

esp_err_t memory_delete(const char *key) {
    for (int i = 0; i < MAX_MOCK_NVS_ENTRIES; i++) {
        if (mock_nvs[i].active && strcmp(mock_nvs[i].key, key) == 0) {
            mock_nvs[i].active = false;
            return ESP_OK;
        }
    }
    return ESP_ERR_NVS_NOT_FOUND;
}

// Mock GPIO policy validation
bool gpio_policy_pin_is_allowed(int pin) {
    return (pin >= 2 && pin <= 10) || pin == 21 || pin == 22;
}

// Mock cJSON implementation
typedef struct cJSON {
    struct cJSON *next;
    struct cJSON *child;
    char *string;
    char *valuestring;
    int valueint;
} cJSON;

cJSON *cJSON_CreateArray(void) {
    cJSON *c = calloc(1, sizeof(cJSON));
    return c;
}

cJSON *cJSON_CreateObject(void) {
    cJSON *c = calloc(1, sizeof(cJSON));
    return c;
}

cJSON *cJSON_AddStringToObject(cJSON *object, const char *name, const char *string) {
    cJSON *item = calloc(1, sizeof(cJSON));
    item->string = strdup(name);
    item->valuestring = strdup(string);
    if (!object->child) {
        object->child = item;
    } else {
        cJSON *curr = object->child;
        while (curr->next) curr = curr->next;
        curr->next = item;
    }
    return item;
}

cJSON *cJSON_AddNumberToObject(cJSON *object, const char *name, double number) {
    cJSON *item = calloc(1, sizeof(cJSON));
    item->string = strdup(name);
    item->valueint = (int)number;
    if (!object->child) {
        object->child = item;
    } else {
        cJSON *curr = object->child;
        while (curr->next) curr = curr->next;
        curr->next = item;
    }
    return item;
}

void cJSON_AddItemToArray(cJSON *array, cJSON *item) {
    if (!array->child) {
        array->child = item;
    } else {
        cJSON *curr = array->child;
        while (curr->next) curr = curr->next;
        curr->next = item;
    }
}

char *cJSON_PrintUnformatted(const cJSON *item) {
    char *buf = malloc(4096);
    buf[0] = '\0';
    strcat(buf, "[");
    cJSON *curr_obj = item->child;
    bool first_obj = true;
    while (curr_obj) {
        if (!first_obj) strcat(buf, ",");
        strcat(buf, "{");
        cJSON *prop = curr_obj->child;
        bool first_prop = true;
        while (prop) {
            if (!first_prop) strcat(buf, ",");
            if (prop->valuestring) {
                sprintf(buf + strlen(buf), "\"%s\":\"%s\"", prop->string, prop->valuestring);
            } else {
                sprintf(buf + strlen(buf), "\"%s\":%d", prop->string, prop->valueint);
            }
            first_prop = false;
            prop = prop->next;
        }
        strcat(buf, "}");
        first_obj = false;
        curr_obj = curr_obj->next;
    }
    strcat(buf, "]");
    return buf;
}

void cJSON_Delete(cJSON *c) {
    if (!c) return;
    cJSON_Delete(c->child);
    cJSON_Delete(c->next);
    if (c->string) free(c->string);
    if (c->valuestring) free(c->valuestring);
    free(c);
}

// Include the actual source file content under test
#include "../gpio_mapping.c"

// Run unit tests
int main(void) {
    reset_mock_nvs();
    
    printf("Starting GPIO Mapping Module Verification Tests...\n\n");

    // Test Case 1: Normal saving of valid mapping
    printf("Test 1: Saving valid mapping (led -> 4)...\n");
    esp_err_t err = gpio_mapping_save("led", 4, NULL);
    assert(err == ESP_OK);
    
    int pin = 0;
    bool exists = gpio_mapping_get_pin("led", &pin);
    assert(exists == true);
    assert(pin == 4);
    printf("Test 1 Passed!\n\n");

    // Test Case 2: Case insensitivity normalization
    printf("Test 2: Case insensitivity normalization...\n");
    exists = gpio_mapping_exists("LED");
    assert(exists == true);
    
    exists = gpio_mapping_get_pin("LeD", &pin);
    assert(exists == true);
    assert(pin == 4);
    
    // Save with mixed case, retrieve with lowercase
    err = gpio_mapping_save("ReLaY", 5, NULL);
    assert(err == ESP_OK);
    exists = gpio_mapping_get_pin("relay", &pin);
    assert(exists == true);
    assert(pin == 5);
    printf("Test 2 Passed!\n\n");

    // Test Case 3: Reject empty device names
    printf("Test 3: Rejecting empty device names...\n");
    err = gpio_mapping_save("", 4, NULL);
    assert(err == ESP_ERR_INVALID_ARG);
    printf("Test 3 Passed!\n\n");

    // Test Case 4: Reject device name exceeding 10 characters
    printf("Test 4: Rejecting device name > 10 chars...\n");
    err = gpio_mapping_save("relay_sensor", 4, NULL); // 12 characters
    assert(err == ESP_ERR_INVALID_ARG);
    printf("Test 4 Passed!\n\n");

    // Test Case 5: Rejecting invalid GPIO pin
    printf("Test 5: Rejecting invalid GPIO pin (relay -> 99)...\n");
    err = gpio_mapping_save("relay", 99, NULL);
    assert(err == ESP_ERR_INVALID_ARG);
    printf("Test 5 Passed!\n\n");

    // Test Case 6: Listing mapping output
    printf("Test 6: Listing mappings...\n");
    char list_buf[512] = {0};
    err = gpio_mapping_list(list_buf, sizeof(list_buf));
    assert(err == ESP_OK);
    printf("List JSON: %s\n", list_buf);
    assert(strstr(list_buf, "\"device\":\"led\"") != NULL);
    assert(strstr(list_buf, "\"pin\":4") != NULL);
    assert(strstr(list_buf, "\"device\":\"relay\"") != NULL);
    assert(strstr(list_buf, "\"pin\":5") != NULL);
    printf("Test 6 Passed!\n\n");

    // Test Case 7: Deleting mapping
    printf("Test 7: Deleting mapping (led)...\n");
    err = gpio_mapping_delete("LED", NULL);
    assert(err == ESP_OK);
    exists = gpio_mapping_exists("led");
    assert(exists == false);
    
    // Check updated list
    memset(list_buf, 0, sizeof(list_buf));
    err = gpio_mapping_list(list_buf, sizeof(list_buf));
    assert(err == ESP_OK);
    printf("List JSON after delete: %s\n", list_buf);
    assert(strstr(list_buf, "\"device\":\"led\"") == NULL);
    assert(strstr(list_buf, "\"device\":\"relay\"") != NULL);
    printf("Test 7 Passed!\n\n");

    // Test Case 8: Device name of 9 or 10 characters (NVS limit handling)
    printf("Test 8: Saving device name of 9 characters (starts u_gpio_123456789 - 16 characters)...\n");
    err = gpio_mapping_save("123456789", 4, NULL);
    // Key "u_gpio_123456789" is 16 chars, which exceeds the NVS limit of 15.
    // get_device_key should catch this or memory_set should reject it and return ESP_ERR_INVALID_SIZE.
    assert(err == ESP_ERR_INVALID_SIZE);
    printf("Test 8 Passed!\n\n");

    // Test Case 9: Updating an existing mapping (relay -> 5 to relay -> 6)
    printf("Test 9: Updating an existing mapping (relay -> 5 to relay -> 6)...\n");
    // Verify it updates and prints "Mappings have been updated"
    err = gpio_mapping_save("relay", 6, NULL);
    assert(err == ESP_OK);
    exists = gpio_mapping_get_pin("relay", &pin);
    assert(exists == true);
    assert(pin == 6);
    printf("Test 9 Passed!\n\n");

    // Test Case 10: Rejecting duplicate GPIO assignments
    printf("Test 10: Rejecting duplicate GPIO assignments (mapping led to 6, which is already used by relay)...\n");
    // Since relay is on 6, mapping led to 6 should fail with ESP_ERR_INVALID_ARG
    err = gpio_mapping_save("led", 6, NULL);
    assert(err == ESP_ERR_INVALID_ARG);
    printf("Test 10 Passed!\n\n");

    // Test Case 11: Case-insensitivity check (mapping LED -> 4, then saving led -> 4)
    printf("Test 11: Case-insensitivity check (mapping LED -> 4, then saving led -> 4)...\n");
    // LED -> 4 is saved
    err = gpio_mapping_save("LED", 4, NULL);
    assert(err == ESP_OK);
    // Since led is already mapped to 4 (as "LED"), mapping led -> 4 again should succeed and print "Mappings have been updated"
    err = gpio_mapping_save("led", 4, NULL);
    assert(err == ESP_OK);
    // Verify only one entry exists for "led" in the list
    memset(list_buf, 0, sizeof(list_buf));
    err = gpio_mapping_list(list_buf, sizeof(list_buf));
    assert(err == ESP_OK);
    printf("List JSON: %s\n", list_buf);
    // Check that "relay" on 6 and "led" on 4 are the only ones
    assert(strstr(list_buf, "\"device\":\"led\"") != NULL);
    assert(strstr(list_buf, "\"device\":\"relay\"") != NULL);
    // Count how many times "device" appears in the list_buf. It should be exactly 2.
    int count = 0;
    const char *tmp = list_buf;
    while ((tmp = strstr(tmp, "\"device\"")) != NULL) {
        count++;
        tmp += strlen("\"device\"");
    }
    assert(count == 2);
    printf("Test 11 Passed!\n\n");

    // ==========================================
    // PASSWORD PROTECTION UNIT TESTS
    // ==========================================

    // Test Case 12: Set password
    printf("Test 12: Set password to 'secret123'...\n");
    err = gpio_mapping_set_password("secret123", NULL);
    assert(err == ESP_OK);
    printf("Test 12 Passed!\n\n");

    // Test Case 13: Attempt modification without password (should fail)
    printf("Test 13: Attempt to save mapping without password (should fail)...\n");
    err = gpio_mapping_save("buzzer", 7, NULL);
    assert(err == ESP_ERR_INVALID_ARG);
    printf("Test 13 Passed!\n\n");

    // Test Case 14: Attempt modification with wrong password (should fail)
    printf("Test 14: Attempt to save mapping with wrong password (should fail)...\n");
    err = gpio_mapping_save("buzzer", 7, "wrongpass");
    assert(err == ESP_ERR_INVALID_ARG);
    printf("Test 14 Passed!\n\n");

    // Test Case 15: Modification with correct password (should succeed)
    printf("Test 15: Save mapping with correct password...\n");
    err = gpio_mapping_save("buzzer", 7, "secret123");
    assert(err == ESP_OK);
    exists = gpio_mapping_get_pin("buzzer", &pin);
    assert(exists == true);
    assert(pin == 7);
    printf("Test 15 Passed!\n\n");

    // Test Case 16: Reading mappings does not require password
    printf("Test 16: Verify reading mappings does not require password...\n");
    exists = gpio_mapping_get_pin("buzzer", &pin);
    assert(exists == true);
    assert(pin == 7);
    exists = gpio_mapping_exists("buzzer");
    assert(exists == true);
    memset(list_buf, 0, sizeof(list_buf));
    err = gpio_mapping_list(list_buf, sizeof(list_buf));
    assert(err == ESP_OK);
    assert(strstr(list_buf, "\"device\":\"buzzer\"") != NULL);
    printf("Test 16 Passed!\n\n");

    // Test Case 17: Delete mapping with correct and incorrect passwords
    printf("Test 17: Delete mapping password checks...\n");
    // Delete with wrong password (should fail)
    err = gpio_mapping_delete("buzzer", "wrongpass");
    assert(err == ESP_ERR_INVALID_ARG);
    exists = gpio_mapping_exists("buzzer");
    assert(exists == true);
    // Delete with correct password (should succeed)
    err = gpio_mapping_delete("buzzer", "secret123");
    assert(err == ESP_OK);
    exists = gpio_mapping_exists("buzzer");
    assert(exists == false);
    printf("Test 17 Passed!\n\n");

    // Test Case 18: Update and Clear Password
    printf("Test 18: Update and clear password...\n");
    // Update password with wrong current password (should fail)
    err = gpio_mapping_set_password("newpass", "wrongpass");
    assert(err == ESP_ERR_INVALID_ARG);
    // Update password with correct current password (should succeed)
    err = gpio_mapping_set_password("newpass", "secret123");
    assert(err == ESP_OK);
    // Save with old password (should fail)
    err = gpio_mapping_save("buzzer", 7, "secret123");
    assert(err == ESP_ERR_INVALID_ARG);
    // Save with new password (should succeed)
    err = gpio_mapping_save("buzzer", 7, "newpass");
    assert(err == ESP_OK);
    // Clear password with wrong password (should fail)
    err = gpio_mapping_set_password(NULL, "wrongpass");
    assert(err == ESP_ERR_INVALID_ARG);
    // Clear password with correct password (should succeed)
    err = gpio_mapping_set_password(NULL, "newpass");
    assert(err == ESP_OK);
    // Now modifications should work without password again
    err = gpio_mapping_save("buzzer", 8, NULL);
    assert(err == ESP_OK); // buzzer updated to 8
    exists = gpio_mapping_get_pin("buzzer", &pin);
    assert(exists == true);
    assert(pin == 8);
    printf("Test 18 Passed!\n\n");

    // Test Case 19: Verify sensitivity of gpio_pwd_hash key
    printf("Test 19: Verify sensitivity of key 'gpio_pwd_hash'...\n");
    extern bool memory_keys_is_sensitive(const char *key);
    bool is_sens = memory_keys_is_sensitive("gpio_pwd_hash");
    assert(is_sens == true);
    // Double check system keys are still sensitive
    assert(memory_keys_is_sensitive("api_key") == true);
    // Double check user keys are not sensitive
    assert(memory_keys_is_sensitive("u_gpio_led") == false);
    printf("Test 19 Passed!\n\n");

    printf("\nAll Tests Passed Successfully!\n");
    return 0;
}
