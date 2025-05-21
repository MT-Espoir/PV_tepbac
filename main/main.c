/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/FreeRTOSConfig.h"
#include "freertos/projdefs.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"

#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "driver/i2c.h" 
#include "sdkconfig.h"
#include "esp_netif.h"
#include "wifi_config.h"
#include "event_config.h"

static int get_mock_random(void) {
    return 0;
}

#ifndef CONFIG_LOG_MAXIMUM_LEVEL
#define CONFIG_LOG_MAXIMUM_LEVEL 3
#endif

#define LED_PIN GPIO_NUM_48
#define BUTTON_PIN GPIO_NUM_0
#define SDA_PIN GPIO_NUM_11
#define SCL_PIN GPIO_NUM_12

#define DEFAULT_WIFI_SSID "TRUC ANH"
#define DEFAULT_WIFI_PASSWORD "23230903"
#define MQTT_URI "mqtt://test.mosquitto.org"
#define MQTT_TOPIC "nhatminh/data"
#define MQTT_SUB_TOPIC "nhatminh/control"

// Khai báo cho I2C 
#define I2C_MASTER_NUM I2C_NUM_0
#define I2C_MASTER_FREQ_HZ 400000
#define I2C_MASTER_TIMEOUT_MS 1000

static const char *TAG = "MAIN";

static esp_mqtt_client_handle_t mqtt_client = NULL;
static TimerHandle_t reconnect_timer;
static uint32_t blinkInterval = 500;

// Khai báo thêm biến
static bool mqtt_connected = false;
static bool wifi_connected = false;

// WiFi credentials
char WIFI_SSID[32] = DEFAULT_WIFI_SSID;
char WIFI_PASSWORD[64] = DEFAULT_WIFI_PASSWORD;

static void save_wifi_to_nvs(const char *ssid, const char *password) {
    nvs_handle_t nvs;
    if (nvs_open("wifi-config", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "ssid", ssid);
        nvs_set_str(nvs, "password", password);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static void load_wifi_from_nvs() {
    nvs_handle_t nvs;
    size_t ssid_len = sizeof(WIFI_SSID);
    size_t pass_len = sizeof(WIFI_PASSWORD);
    
    // Xóa các biến trước khi tải
    memset(WIFI_SSID, 0, sizeof(WIFI_SSID));
    memset(WIFI_PASSWORD, 0, sizeof(WIFI_PASSWORD));
    
    // Mở namespace NVS để đọc cấu hình WiFi
    esp_err_t err = nvs_open("wifi-config", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Không thể mở NVS (err=%d)", err);
        return;
    }
    
    // Đọc SSID
    err = nvs_get_str(nvs, "ssid", WIFI_SSID, &ssid_len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "Lỗi khi đọc SSID từ NVS (err=%d)", err);
    }
    
    // Đọc password
    err = nvs_get_str(nvs, "password", WIFI_PASSWORD, &pass_len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "Lỗi khi đọc password từ NVS (err=%d)", err);
    }
    
    nvs_close(nvs);
    
    ESP_LOGI(TAG, "Thông tin WiFi đã được tải từ NVS (SSID có sẵn: %s)", 
             strlen(WIFI_SSID) > 0 ? "Có" : "Không");
}

static void mqtt_publish_status() {
    char message[128];
    snprintf(message, sizeof(message), "{\"status\":\"online\",\"blinkInterval\":%u}", (unsigned int)blinkInterval);
    esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC, message, 0, 1, 0);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_t *event = (esp_mqtt_event_t*)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG,"MQTT Connected\n");
        mqtt_connected = true;
        esp_mqtt_client_subscribe(mqtt_client, MQTT_SUB_TOPIC, 0);
        mqtt_publish_status();
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT Disconnected");
        mqtt_connected = false;
        // Kích hoạt timer để kết nối lại sau một khoảng thời gian
        xTimerStart(reconnect_timer, 0);
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT Subscribed to topic");
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT Unsubscribed from topic");
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT Error");
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "Received on topic: %.*s", event->topic_len, event->topic);
        char *data = strndup(event->data, event->data_len);
        cJSON *json = cJSON_Parse(data);
        if (json) {
            if (cJSON_IsTrue(cJSON_GetObjectItem(json, "show_wifi"))) {
                char msg[256];
                snprintf(msg, sizeof(msg), "{\"ssid\":\"%s\",\"connected\":%s}", WIFI_SSID, esp_wifi_sta_get_ap_info(NULL) == ESP_OK ? "true" : "false");
                esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC, msg, 0, 1, 0);
            }            if (cJSON_IsTrue(cJSON_GetObjectItem(json, "clear_wifi"))) {
                nvs_handle_t nvs;
                nvs_open("wifi-config", NVS_READWRITE, &nvs);
                nvs_erase_all(nvs);
                nvs_close(nvs);
                strncpy(WIFI_SSID, DEFAULT_WIFI_SSID, sizeof(WIFI_SSID));
                strncpy(WIFI_PASSWORD, DEFAULT_WIFI_PASSWORD, sizeof(WIFI_PASSWORD));
                
                // phản hồi thành công 
                char response[128];
                snprintf(response, sizeof(response), "{\"status\":\"success\",\"message\":\"WiFi credentials cleared, default values restored\"}");
                esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC, response, 0, 1, 0);
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }if (cJSON_IsTrue(cJSON_GetObjectItem(json, "wifi_config"))) {
                const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(json, "ssid"));
                const char *pass = cJSON_GetStringValue(cJSON_GetObjectItem(json, "password"));
                if (ssid && pass) {
                    save_wifi_to_nvs(ssid, pass);
                    strncpy(WIFI_SSID, ssid, sizeof(WIFI_SSID));
                    strncpy(WIFI_PASSWORD, pass, sizeof(WIFI_PASSWORD));
                    // phản hồi thành công
                    char response[128];
                    snprintf(response, sizeof(response), "{\"status\":\"success\",\"message\":\"WiFi credentials updated\"}");
                    esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC, response, 0, 1, 0);
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_restart();
                }
            }
            cJSON_Delete(json);
        }
        free(data);
        break;
    default:
        break;
    }
}

// Callback cho timer kết nối lại MQTT
static void mqtt_reconnect_timer_callback(TimerHandle_t xTimer) {
    if (mqtt_client != NULL) {
        printf("Reconnecting to MQTT...\n");
        esp_mqtt_client_start(mqtt_client);
    }
}
// Cấu hình MQTT với Last Will and Testament
static void mqtt_app_start(void) {    
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_URI,
    };
    
    char client_id[32];
    snprintf(client_id, sizeof(client_id), "esp32_%u", get_mock_random());
    mqtt_cfg.credentials.client_id = client_id;  
    mqtt_cfg.session.last_will.topic = MQTT_TOPIC;
    mqtt_cfg.session.last_will.msg = "{\"status\":\"offline\"}";
    mqtt_cfg.session.last_will.msg_len = strlen("{\"status\":\"offline\"}");
    mqtt_cfg.session.last_will.qos = 1;
    mqtt_cfg.session.last_will.retain = 0;
    
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    
    // Đăng ký event handler cho MQTT
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

// Xử lý sự kiện WiFi
static void wifi_event_handler(void *arg, esp_event_base_t event_base, 
                              int32_t event_id, void *event_data) {
    static int retry_count = 0;
    static bool using_default_wifi = false;
    
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
            retry_count = 0;
            ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", WIFI_SSID);
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_err_reason_t reason = ((wifi_event_sta_disconnected_t*)event_data)->reason;
            ESP_LOGI(TAG, "WiFi disconnected (reason: %d). Reconnecting... (attempt %d)", reason, retry_count + 1);
            wifi_connected = false;
            
            // Nếu không kết nối được sau 5 lần thử và không phải đang dùng wifi mặc định
            if (retry_count >= 5 && !using_default_wifi && 
                strcmp(WIFI_SSID, DEFAULT_WIFI_SSID) != 0) {
                ESP_LOGI(TAG, "Không thể kết nối với mạng WiFi sau nhiều lần thử. Chuyển sang WiFi mặc định");
                strncpy(WIFI_SSID, DEFAULT_WIFI_SSID, sizeof(WIFI_SSID));
                strncpy(WIFI_PASSWORD, DEFAULT_WIFI_PASSWORD, sizeof(WIFI_PASSWORD));
                save_wifi_to_nvs(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASSWORD);
                ESP_LOGI(TAG, "Đã lưu thông tin WiFi mặc định vào NVS");
                
                // Cập nhật cấu hình WiFi và kết nối lại
                wifi_config_t wifi_config = {};
                strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
                strncpy((char *)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password));
                
                ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
                using_default_wifi = true;
                retry_count = 0;
                ESP_LOGI(TAG, "Đã chuyển sang sử dụng WiFi mặc định: %s", DEFAULT_WIFI_SSID);
            } else {
                retry_count++;
            }
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
            ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
            wifi_connected = true;
            if (mqtt_client == NULL) {
                mqtt_app_start();
            } else {
                esp_mqtt_client_start(mqtt_client);
            }
        }
    }
}

static void wifi_init_sta(void) {
    // Khởi tạo TCP/IP stack và event loop và netif
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    
    // Đăng ký xử lý sự kiện cho WiFi events và IP events
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler,NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
    
    // Khởi tạo WiFi với Cấu hình WiFi và thiết lập
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "wifi_init_sta finished. Connecting to AP SSID:%s", WIFI_SSID);
}

// Hàm khởi tạo I2C
static esp_err_t i2c_master_init(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = SDA_PIN,
        .scl_io_num = SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    
    esp_err_t err = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (err != ESP_OK) {
        return err;
    }
    
    return i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
}

void led_task(void *pvParameter) {
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    bool state = false;
    while (1) {
        gpio_set_level(LED_PIN, state);
        state = !state;
        vTaskDelay(pdMS_TO_TICKS(blinkInterval));
    }
}

void button_task(void *pvParameter) {
    gpio_reset_pin(BUTTON_PIN);
    gpio_set_direction(BUTTON_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON_PIN, GPIO_PULLUP_ONLY);

    bool lastState = true;
    uint32_t lastChange = 0;

    while (1) {
        bool current = gpio_get_level(BUTTON_PIN);
        if (!current && lastState && (xTaskGetTickCount() - lastChange) > pdMS_TO_TICKS(200)) {
            if (blinkInterval == 500) blinkInterval = 1000;
            else if (blinkInterval == 1000) blinkInterval = 100;
            else blinkInterval = 500;
            mqtt_publish_status();
            lastChange = xTaskGetTickCount();
        }
        lastState = current;
    vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// Status task function declaration
void status_task(void *pvParameter) {
    while (1) {
        if (mqtt_connected) {
            mqtt_publish_status();
        }
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting application...");
    
    // Khởi tạo NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
      // Tạo timer cho việc kết nối lại MQTT
    reconnect_timer = xTimerCreate("mqtt_reconnect_timer", pdMS_TO_TICKS(10000), pdFALSE, (void*)0, mqtt_reconnect_timer_callback);
    
    // Tải thông tin WiFi từ NVS
    load_wifi_from_nvs();
    ESP_LOGI(TAG, "Thông tin WiFi được tải từ NVS: SSID=%s", WIFI_SSID);
    
    // Kiểm tra nếu không có thông tin WiFi trong NVS, sử dụng giá trị mặc định
    if (strlen(WIFI_SSID) == 0) {
        ESP_LOGI(TAG, "Không tìm thấy thông tin WiFi trong NVS, sử dụng giá trị mặc định");
        strncpy(WIFI_SSID, DEFAULT_WIFI_SSID, sizeof(WIFI_SSID));
        strncpy(WIFI_PASSWORD, DEFAULT_WIFI_PASSWORD, sizeof(WIFI_PASSWORD));
        
        // Lưu thông tin WiFi mặc định vào NVS
        save_wifi_to_nvs(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASSWORD);
    }
    
    ESP_ERROR_CHECK(i2c_master_init());
    ESP_LOGI(TAG, "I2C initialized successfully");
    
    wifi_init_sta();
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Tạo các task cho đèn LED và nút nhấn
    xTaskCreate(led_task, "led_task", 4096, NULL, 5, NULL);
    xTaskCreate(button_task, "button_task", 4096, NULL, 5, NULL);
      // Task giám sát và thông báo trạng thái
    xTaskCreate(status_task, "status_task", 4096, NULL, 3, NULL);
}
