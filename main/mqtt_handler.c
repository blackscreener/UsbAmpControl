#include "mqtt_handler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "secrets.h"
#include "usb_driver.h"
#include "esp_wifi.h"
#include "esp_timer.h"

static const char *TAG = "MQTT_HANDLER";
static esp_mqtt_client_handle_t client = NULL;
static volatile bool mqtt_connected = false;

// Deklaracja kolejki z usb_driver.c
extern QueueHandle_t command_queue;

// Tematy MQTT
#define TOPIC_STATE "amp/state"
#define TOPIC_VOL_SET "amp/volume/set"
#define TOPIC_MUTE_SET "amp/mute/set"
#define TOPIC_PRESET_SET "amp/preset/set"
#define TOPIC_EQ_P1_SET "amp/eq_p1/set"
#define TOPIC_EQ_P2_SET "amp/eq_p2/set"
#define TOPIC_EQ_P3_SET "amp/eq_p3/set"
#define TOPIC_SOURCE_P1_SET "amp/source_p1/set"
#define TOPIC_SOURCE_P2_SET "amp/source_p2/set"
#define TOPIC_SOURCE_P3_SET "amp/source_p3/set"
#define TOPIC_USB_CONNECTED "amp/usb_connected"
#define TOPIC_WIFI_RSSI "amp/wifi_rssi"
#define TOPIC_WIFI_QUALITY "amp/wifi_quality"
#define TOPIC_UPTIME_MINUTES "amp/uptime_minutes"
#define TOPIC_UPTIME_FORMATTED "amp/uptime_formatted"  // dni:godziny:minuty:sekundy

// DODAJ TEJ DEKLARACJI (przed wszystkimi funkcjami które jej używają):
void mqtt_publish_usb_connected(bool connected);

// Helper do konwersji source string na enum
static input_source_t string_to_source(const char* source_str) {
    if (strcmp(source_str, "SCAN") == 0) return SOURCE_SCAN;
    if (strcmp(source_str, "XLR") == 0) return SOURCE_XLR;
    if (strcmp(source_str, "RCA") == 0) return SOURCE_RCA;
    if (strcmp(source_str, "SPDIF") == 0) return SOURCE_SPDIF;
    if (strcmp(source_str, "AES") == 0) return SOURCE_AES;
    if (strcmp(source_str, "OPT") == 0) return SOURCE_OPT;
    if (strcmp(source_str, "EXT") == 0) return SOURCE_EXT;
    return SOURCE_SCAN; // default
}

// Helper do konwersji source enum na string
static const char* source_to_string(input_source_t source) {
    switch (source) {
        case SOURCE_SCAN: return "SCAN";
        case SOURCE_XLR: return "XLR";
        case SOURCE_RCA: return "RCA";
        case SOURCE_SPDIF: return "SPDIF";
        case SOURCE_AES: return "AES";
        case SOURCE_OPT: return "OPT";
        case SOURCE_EXT: return "EXT";
        default: return "UNKNOWN";
    }
}

// Funkcja wysyłania pojedynczej encji discovery z opóźnieniem
static void send_single_discovery(const char* type, const char* name, 
                                 const char* uniq_id, const char* payload) {
    char topic[96];
    snprintf(topic, sizeof(topic), "homeassistant/%s/%s/%s/config", 
             type, DEVICE_ID, uniq_id);
    
    esp_mqtt_client_publish(client, topic, payload, 0, 1, 1);
    ESP_LOGI(TAG, "Published discovery: %s", name);
    vTaskDelay(pdMS_TO_TICKS(100)); // Opóźnienie między wiadomościami
}

// Task do wysyłania discovery z opóźnieniem
static void delayed_discovery_task(void *arg) {
    ESP_LOGI(TAG, "Starting delayed discovery task...");
    
    // Czekamy 5 sekund na ustabilizowanie systemu
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    if (!client) {
        ESP_LOGE(TAG, "MQTT client not available");
        vTaskDelete(NULL);
        return;
    }
    
    
    // Device info Z availability (dla większości encji)
    const char* device_info_with_avail = 
        "\"device\":{"
        "\"ids\":[\"" DEVICE_ID "\"],"
        "\"name\":\"" DEVICE_NAME "\","
        "\"mf\":\"ESP32\","
        "\"mdl\":\"S3-DevKit\","
        "\"sw\":\"1.0\""
        "},"
        "\"avty_t\":\"amp/status\","
        "\"pl_avail\":\"online\","
        "\"pl_not_avail\":\"offline\","
        "\"expire_after\":90";

    // Device info Z availability, ALE BEZ expire_after (dla binary sensora USB)
    const char* device_info_without_expire = 
        "\"device\":{"
        "\"ids\":[\"" DEVICE_ID "\"],"
        "\"name\":\"" DEVICE_NAME "\","
        "\"mf\":\"ESP32\","
        "\"mdl\":\"S3-DevKit\","
        "\"sw\":\"1.0\""
        "},"
        "\"avty_t\":\"amp/status\","
        "\"pl_avail\":\"online\","
        "\"pl_not_avail\":\"offline\"";  // BRAK expire_after!

    // Status Sensor - Z availability (jak wszystkie inne!)
    char status_payload[512];
    snprintf(status_payload, sizeof(status_payload),
        "{\"name\":\"Status\",\"uniq_id\":\"%s_status\",\"stat_t\":\"amp/status\","
        "\"val_tpl\":\"{{ value }}\",\"icon\":\"mdi:power\","
        "\"avty_t\":\"amp/status\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
        "%s}",
        DEVICE_ID, device_info_with_avail);
    send_single_discovery("sensor", "Status", "status", status_payload);

    // Binary Sensor - połączenie USB ze wzmacniaczem - BEZ expire_after
    char usb_connected_payload[512];
    snprintf(usb_connected_payload, sizeof(usb_connected_payload),
        "{\"name\":\"USB Connected\",\"uniq_id\":\"%s_usb_connected\",\"stat_t\":\"%s\","
        "\"pl_on\":\"true\",\"pl_off\":\"false\",\"dev_cla\":\"connectivity\",\"icon\":\"mdi:usb\","
        "%s}",  // Użyj device_info_without_expire!
        DEVICE_ID, TOPIC_USB_CONNECTED, device_info_without_expire);
    send_single_discovery("binary_sensor", "USB Connected", "usb_connected", usb_connected_payload);

// --- POPRAWKA TUTAJ ---
    // Sensor jakości WiFi w procentach
    // USUNIĘTO: \"dev_cla\":\"signal_strength\" (bo to jest tylko dla dBm/dB)
    char wifi_quality_payload[512];
    snprintf(wifi_quality_payload, sizeof(wifi_quality_payload),
        "{\"name\":\"WiFi Quality\",\"uniq_id\":\"%s_wifi_quality\",\"stat_t\":\"%s\","
        "\"unit_of_meas\":\"%%\",\"icon\":\"mdi:wifi\","  
        "\"state_class\":\"measurement\",\"entity_category\":\"diagnostic\","
        "%s}",
        DEVICE_ID, TOPIC_WIFI_QUALITY, device_info_with_avail);
    send_single_discovery("sensor", "WiFi Quality", "wifi_quality", wifi_quality_payload);

    // Sensor RSSI WiFi w dBm
    char wifi_rssi_payload[512];
    snprintf(wifi_rssi_payload, sizeof(wifi_rssi_payload),
        "{\"name\":\"WiFi RSSI\",\"uniq_id\":\"%s_wifi_rssi\",\"stat_t\":\"%s\","
        "\"unit_of_meas\":\"dBm\",\"icon\":\"mdi:wifi\",\"dev_cla\":\"signal_strength\","
        "\"state_class\":\"measurement\",\"entity_category\":\"diagnostic\","
        "%s}",
        DEVICE_ID, TOPIC_WIFI_RSSI, device_info_with_avail);
    send_single_discovery("sensor", "WiFi RSSI", "wifi_rssi", wifi_rssi_payload);

    // Sensor uptime w minutach
    char uptime_minutes_payload[512];
    snprintf(uptime_minutes_payload, sizeof(uptime_minutes_payload),
        "{\"name\":\"Uptime\",\"uniq_id\":\"%s_uptime_minutes\",\"stat_t\":\"%s\","
        "\"unit_of_meas\":\"min\",\"icon\":\"mdi:timer\","
        "\"state_class\":\"total_increasing\",\"entity_category\":\"diagnostic\","
        "\"device_class\":\"duration\","
        "%s}",
        DEVICE_ID, TOPIC_UPTIME_MINUTES, device_info_without_expire);
    send_single_discovery("sensor", "Uptime", "uptime_minutes", uptime_minutes_payload);

    // Sensor uptime sformatowany (Xd Xh Xm Xs)
    char uptime_formatted_payload[512];
    snprintf(uptime_formatted_payload, sizeof(uptime_formatted_payload),
        "{\"name\":\"Uptime Formatted\",\"uniq_id\":\"%s_uptime_formatted\",\"stat_t\":\"%s\","
        "\"icon\":\"mdi:timer\","
        "\"entity_category\":\"diagnostic\","
        "%s}",
        DEVICE_ID, TOPIC_UPTIME_FORMATTED, device_info_without_expire);
    send_single_discovery("sensor", "Uptime Formatted", "uptime_formatted", uptime_formatted_payload);


    // Tablica z wszystkimi encjami do wysłania (Z availability!)
    const struct {
        const char* type;
        const char* name;
        const char* uniq_id;
        const char* payload_template;
        const char* cmd_topic;
        const char* options;
    } entities[] = {
        // Volume
        {"number", "Volume", "volume",
         "{\"name\":\"Volume\",\"uniq_id\":\"%s_vol\",\"stat_t\":\"%s\",\"cmd_t\":\"%s\","
         "\"min\":-99,\"max\":18,\"step\":1,\"unit\":\"dB\","
         "\"val_tpl\":\"{{value_json.volume_db}}\", %s}",
         TOPIC_VOL_SET, NULL},
        
        // Mute
        {"switch", "Mute", "mute",
         "{\"name\":\"Mute\",\"uniq_id\":\"%s_mute\",\"stat_t\":\"%s\",\"cmd_t\":\"%s\","
         "\"pl_on\":\"true\",\"pl_off\":\"false\","
         "\"val_tpl\":\"{{'true' if value_json.is_muted else 'false'}}\", %s}",
         TOPIC_MUTE_SET, NULL},
        
        // Preset
        {"select", "Preset", "preset",
         "{\"name\":\"Preset\",\"uniq_id\":\"%s_preset\",\"stat_t\":\"%s\",\"cmd_t\":\"%s\","
         "\"ops\":%s,\"val_tpl\":\"{{value_json.preset}}\", %s}",
         TOPIC_PRESET_SET, "[\"1\",\"2\",\"3\"]"},
        
        // EQ Preset 1
        {"switch", "EQ Preset 1", "eq_p1",
         "{\"name\":\"EQ Preset 1\",\"uniq_id\":\"%s_eq_p1\",\"stat_t\":\"%s\",\"cmd_t\":\"%s\","
         "\"pl_on\":\"true\",\"pl_off\":\"false\","
         "\"val_tpl\":\"{{'true' if value_json.eq_p1 else 'false'}}\", %s}",
         TOPIC_EQ_P1_SET, NULL},
        
        // EQ Preset 2
        {"switch", "EQ Preset 2", "eq_p2",
         "{\"name\":\"EQ Preset 2\",\"uniq_id\":\"%s_eq_p2\",\"stat_t\":\"%s\",\"cmd_t\":\"%s\","
         "\"pl_on\":\"true\",\"pl_off\":\"false\","
         "\"val_tpl\":\"{{'true' if value_json.eq_p2 else 'false'}}\", %s}",
         TOPIC_EQ_P2_SET, NULL},
        
        // EQ Preset 3
        {"switch", "EQ Preset 3", "eq_p3",
         "{\"name\":\"EQ Preset 3\",\"uniq_id\":\"%s_eq_p3\",\"stat_t\":\"%s\",\"cmd_t\":\"%s\","
         "\"pl_on\":\"true\",\"pl_off\":\"false\","
         "\"val_tpl\":\"{{'true' if value_json.eq_p3 else 'false'}}\", %s}",
         TOPIC_EQ_P3_SET, NULL},
        
        // Source Preset 1
        {"select", "Source Preset 1", "source_p1",
         "{\"name\":\"Source Preset 1\",\"uniq_id\":\"%s_source_p1\",\"stat_t\":\"%s\",\"cmd_t\":\"%s\","
         "\"ops\":%s,\"val_tpl\":\"{{value_json.source_p1}}\", %s}",
         TOPIC_SOURCE_P1_SET, "[\"SCAN\",\"XLR\",\"RCA\",\"SPDIF\",\"AES\",\"OPT\",\"EXT\"]"},
        
        // Source Preset 2
        {"select", "Source Preset 2", "source_p2",
         "{\"name\":\"Source Preset 2\",\"uniq_id\":\"%s_source_p2\",\"stat_t\":\"%s\",\"cmd_t\":\"%s\","
         "\"ops\":%s,\"val_tpl\":\"{{value_json.source_p2}}\", %s}",
         TOPIC_SOURCE_P2_SET, "[\"SCAN\",\"XLR\",\"RCA\",\"SPDIF\",\"AES\",\"OPT\",\"EXT\"]"},
        
        // Source Preset 3
        {"select", "Source Preset 3", "source_p3",
         "{\"name\":\"Source Preset 3\",\"uniq_id\":\"%s_source_p3\",\"stat_t\":\"%s\",\"cmd_t\":\"%s\","
         "\"ops\":%s,\"val_tpl\":\"{{value_json.source_p3}}\", %s}",
         TOPIC_SOURCE_P3_SET, "[\"SCAN\",\"XLR\",\"RCA\",\"SPDIF\",\"AES\",\"OPT\",\"EXT\"]"},
    };
    
    // Bufor dla payload
    char payload[512];
    
    // Wysyłamy wszystkie encje Z availability
    for (int i = 0; i < sizeof(entities)/sizeof(entities[0]); i++) {
        if (entities[i].options) {
            snprintf(payload, sizeof(payload), entities[i].payload_template,
                    DEVICE_ID, TOPIC_STATE, entities[i].cmd_topic, 
                    entities[i].options, device_info_with_avail);
        } else {
            snprintf(payload, sizeof(payload), entities[i].payload_template,
                    DEVICE_ID, TOPIC_STATE, entities[i].cmd_topic, device_info_with_avail);
        }
        
        send_single_discovery(entities[i].type, entities[i].name, 
                            entities[i].uniq_id, payload);
    }
    
    ESP_LOGI(TAG, "Discovery task completed");
    vTaskDelete(NULL);
}

void mqtt_send_hass_discovery(void) {
    // Tworzymy zadanie discovery z opóźnieniem
    xTaskCreate(delayed_discovery_task, "disc_task", 8192, NULL, 1, NULL);
}

// Funkcja wysyłania stanu z wszystkimi danymi
void mqtt_notify_state_changed(const state_t *state) {
    if (client == NULL || state == NULL) return;
    
    // Statyczny bufor dla JSON - większy, aby pomieścić wszystkie dane
    static char json_buffer[512];
    
    snprintf(json_buffer, sizeof(json_buffer),
        "{\"volume_db\":%.1f,\"is_muted\":%s,\"preset\":%d,"
        "\"eq_p1\":%s,\"eq_p2\":%s,\"eq_p3\":%s,"
        "\"source_p1\":\"%s\",\"source_p2\":\"%s\",\"source_p3\":\"%s\","
        "\"current_source\":\"%s\"}",
        state->volume_db,
        state->is_muted ? "true" : "false",
        (int)state->preset,
        state->is_eq_on[0] ? "true" : "false",
        state->is_eq_on[1] ? "true" : "false",
        state->is_eq_on[2] ? "true" : "false",
        source_to_string(state->preset_source[0]),
        source_to_string(state->preset_source[1]),
        source_to_string(state->preset_source[2]),
        source_to_string(state->current_source));
    
    esp_mqtt_client_publish(client, TOPIC_STATE, json_buffer, 0, 1, 0); // retain=0 dla stanu
}

// Handler zdarzeń MQTT
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, 
                               int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT Connected");
            mqtt_connected = true;  // <-- USTAW true
            // Publikuj status "online" z retain=1
            esp_mqtt_client_publish(client, "amp/status", "online", 0, 1, 1);

            // Subskrybujemy wszystkie tematy z opóźnieniami
            esp_mqtt_client_subscribe(client, TOPIC_VOL_SET, 1);
            vTaskDelay(pdMS_TO_TICKS(50));
            esp_mqtt_client_subscribe(client, TOPIC_MUTE_SET, 1);
            vTaskDelay(pdMS_TO_TICKS(50));
            esp_mqtt_client_subscribe(client, TOPIC_PRESET_SET, 1);
            vTaskDelay(pdMS_TO_TICKS(50));
            esp_mqtt_client_subscribe(client, TOPIC_EQ_P1_SET, 1);
            vTaskDelay(pdMS_TO_TICKS(50));
            esp_mqtt_client_subscribe(client, TOPIC_EQ_P2_SET, 1);
            vTaskDelay(pdMS_TO_TICKS(50));
            esp_mqtt_client_subscribe(client, TOPIC_EQ_P3_SET, 1);
            vTaskDelay(pdMS_TO_TICKS(50));
            esp_mqtt_client_subscribe(client, TOPIC_SOURCE_P1_SET, 1);
            vTaskDelay(pdMS_TO_TICKS(50));
            esp_mqtt_client_subscribe(client, TOPIC_SOURCE_P2_SET, 1);
            vTaskDelay(pdMS_TO_TICKS(50));
            esp_mqtt_client_subscribe(client, TOPIC_SOURCE_P3_SET, 1);
            
            // WYŚLIJ AKTUALNY STAN PO POŁĄCZENIU
            if (is_device_connected()) {
                state_t current_state;
                get_state(&current_state);
                mqtt_notify_state_changed(&current_state);
            }

            // WAŻNE: Publikuj WiFi dane NATYCHMIAST!
            mqtt_publish_wifi_quality();

            // DODAJ: Publikuj czas pracy przy połączeniu
            mqtt_publish_uptime();

            // Discovery wywołamy później w osobnym tasku
            mqtt_send_hass_discovery();
            break;
            
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT Disconnected");
            mqtt_connected = false; // <-- USTAW false
            break;
            
        case MQTT_EVENT_DATA: {
            char val_buf[32] = {0};
            int len = (event->data_len < 31) ? event->data_len : 31;
            memcpy(val_buf, event->data, len);
            val_buf[len] = '\0';
            
            control_action_t action;
            
            if (strncmp(event->topic, TOPIC_VOL_SET, event->topic_len) == 0) {
                action.action = ACTION_SET_VOLUME;
                action.value = (int8_t)atoi(val_buf);
            } 
            else if (strncmp(event->topic, TOPIC_MUTE_SET, event->topic_len) == 0) {
                action.action = ACTION_SET_MUTE;
                action.value = (strcmp(val_buf, "true") == 0) ? 1 : 0;
            } 
            else if (strncmp(event->topic, TOPIC_PRESET_SET, event->topic_len) == 0) {
                action.action = ACTION_SET_PRESET;
                action.value = (int8_t)atoi(val_buf);
            }
            else if (strncmp(event->topic, TOPIC_EQ_P1_SET, event->topic_len) == 0) {
                action.action = ACTION_SET_EQ_P1;
                action.value = (strcmp(val_buf, "true") == 0) ? 1 : 0;
            }
            else if (strncmp(event->topic, TOPIC_EQ_P2_SET, event->topic_len) == 0) {
                action.action = ACTION_SET_EQ_P2;
                action.value = (strcmp(val_buf, "true") == 0) ? 1 : 0;
            }
            else if (strncmp(event->topic, TOPIC_EQ_P3_SET, event->topic_len) == 0) {
                action.action = ACTION_SET_EQ_P3;
                action.value = (strcmp(val_buf, "true") == 0) ? 1 : 0;
            }
            else if (strncmp(event->topic, TOPIC_SOURCE_P1_SET, event->topic_len) == 0) {
                action.action = ACTION_SET_SOURCE_P1;
                action.value = (int8_t)string_to_source(val_buf);
            }
            else if (strncmp(event->topic, TOPIC_SOURCE_P2_SET, event->topic_len) == 0) {
                action.action = ACTION_SET_SOURCE_P2;
                action.value = (int8_t)string_to_source(val_buf);
            }
            else if (strncmp(event->topic, TOPIC_SOURCE_P3_SET, event->topic_len) == 0) {
                action.action = ACTION_SET_SOURCE_P3;
                action.value = (int8_t)string_to_source(val_buf);
            }
            else {
                // Nieznany temat
                return;
            }
            
            // Wysyłamy komendę do kolejki
            xQueueSend(command_queue, &action, 0);
            break;
        }
        
        default:
            break;
    }
}


void mqtt_publish_uptime(void) {
    // Szybkie wyjście, jeśli nie ma połączenia
    if (client == NULL) {
        ESP_LOGE(TAG, "MQTT client is NULL, cannot publish uptime");
        return;
    }
    
    // Użyj lokalnej kopii wskaźnika dla bezpieczeństwa
    esp_mqtt_client_handle_t local_client = client;
    if (local_client == NULL || !mqtt_connected) {
        ESP_LOGE(TAG, "MQTT not connected, cannot publish uptime");
        return;
    }
    
    static int64_t last_published_time = 0;
    static bool first_publish = true;
    
    int64_t uptime_microseconds = esp_timer_get_time();
    int64_t uptime_seconds = uptime_microseconds / 1000000;
    int64_t uptime_minutes = uptime_seconds / 60;
    
    // Publikuj zawsze przy pierwszym wywołaniu, potem co 120 sekund
    bool should_publish = first_publish || ((uptime_seconds - last_published_time) >= 120);
    
    if (should_publish) {
        ESP_LOGI(TAG, "Preparing to publish uptime...");
        
        // 1. Publikuj w minutach
        char uptime_min_str[32];
        int len1 = snprintf(uptime_min_str, sizeof(uptime_min_str), "%lld", uptime_minutes);
        if (len1 <= 0 || len1 >= sizeof(uptime_min_str)) {
            ESP_LOGE(TAG, "Failed to format uptime minutes");
            return;
        }
        
        ESP_LOGI(TAG, "Publishing uptime (minutes): %s", uptime_min_str);
        
        int msg_id_minutes = esp_mqtt_client_publish(local_client, TOPIC_UPTIME_MINUTES, 
                                                    uptime_min_str, 0, 1, 1);
        
        // 2. Publikuj sformatowany czas
        uint64_t days = uptime_seconds / 86400;
        uint64_t hours = (uptime_seconds % 86400) / 3600;
        uint64_t minutes = (uptime_seconds % 3600) / 60;
        uint64_t seconds = uptime_seconds % 60;
        
        char formatted_str[64];  // Zwiększ bufor dla bezpieczeństwa
        int len2 = snprintf(formatted_str, sizeof(formatted_str), 
                           "%llud %lluh %llum %llus", 
                           days, hours, minutes, seconds);
        if (len2 <= 0 || len2 >= sizeof(formatted_str)) {
            ESP_LOGE(TAG, "Failed to format uptime string");
            return;
        }
        
        ESP_LOGI(TAG, "Publishing uptime (formatted): %s", formatted_str);
        
        int msg_id_formatted = esp_mqtt_client_publish(local_client, TOPIC_UPTIME_FORMATTED, 
                                                      formatted_str, 0, 1, 1);
        
        if (msg_id_minutes >= 0 && msg_id_formatted >= 0) {
            last_published_time = uptime_seconds;
            first_publish = false;
            ESP_LOGI(TAG, "Uptime published successfully");
        } else {
            ESP_LOGE(TAG, "Failed to publish uptime: minutes=%d, formatted=%d", 
                    msg_id_minutes, msg_id_formatted);
        }
    }
}


// Funkcja do publikowania stanu połączenia USB
void mqtt_publish_usb_connected(bool connected) {
    if (client == NULL) return;
    
    ESP_LOGI(TAG, "Publishing USB connected: %s", connected ? "true" : "false");
    esp_mqtt_client_publish(client, TOPIC_USB_CONNECTED, 
                           connected ? "true" : "false", 0, 1, 1); // retain=1
}

static void mqtt_keepalive_task(void *arg) {
    ESP_LOGI(TAG, "Starting MQTT keepalive task");
    bool last_usb_state = false;
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000)); // Co 30 sekund
        
        // Sprawdzamy, czy klient istnieje I czy jest połączony
        if (client != NULL && mqtt_connected) {
            
            // Ponieważ sprawdziliśmy mqtt_connected, możemy bezpiecznie publikować
            esp_mqtt_client_publish(client, "amp/status", "online", 0, 1, 1);
            ESP_LOGD(TAG, "Keepalive sent"); // Używam LOGD żeby nie śmiecić w logach co 30s

            // Publikuj stan USB jeśli się zmienił
            bool current_usb_state = is_device_connected();
            if (current_usb_state != last_usb_state) {
                mqtt_publish_usb_connected(current_usb_state);
                last_usb_state = current_usb_state;
            }
            
            // Publikuj jakość WiFi
            mqtt_publish_wifi_quality();

            // DODAJ: Publikuj czas pracy
            mqtt_publish_uptime();
            
        } else {
            // DODAJ TUTAJ RECONNECT LOGIC
            ESP_LOGW(TAG, "MQTT disconnected, attempting reconnect");
            
            if (client != NULL) {
                if (esp_mqtt_client_reconnect(client) == ESP_OK) {
                    ESP_LOGI(TAG, "Reconnect initiated");
                } else {
                    ESP_LOGE(TAG, "Reconnect failed");
                }}
            }
    }
}

void mqtt_publish_wifi_quality(void) {
    // Szybkie wyjście, jeśli nie ma połączenia
    if (client == NULL || !mqtt_connected) {
        return;
    }
    
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK && ap_info.rssi != 0) {
        
        int8_t rssi = ap_info.rssi;
        int quality;
        
        if (rssi >= -30) quality = 100;
        else if (rssi <= -100) quality = 0;
        else quality = 100 * (rssi + 100) / 70;
        
        if (quality > 100) quality = 100;
        if (quality < 0) quality = 0;
        
        char rssi_str[8];
        snprintf(rssi_str, sizeof(rssi_str), "%d", rssi);
        esp_mqtt_client_publish(client, TOPIC_WIFI_RSSI, rssi_str, 0, 1, 1);
        
        char quality_str[8];
        snprintf(quality_str, sizeof(quality_str), "%d", quality);
        esp_mqtt_client_publish(client, TOPIC_WIFI_QUALITY, quality_str, 0, 1, 1);
        
    } else {
        // Jeśli nie można pobrać danych WiFi, ale MQTT działa -> wyślij zera
        esp_mqtt_client_publish(client, TOPIC_WIFI_QUALITY, "0", 0, 1, 1);
        esp_mqtt_client_publish(client, TOPIC_WIFI_RSSI, "0", 0, 1, 1);
    }
}


// Task startowy MQTT
void mqtt_app_start_task(void *arg) {
    ESP_LOGI(TAG, "Starting MQTT task...");
    
    // Dajemy czas na uruchomienie się Wi-Fi
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    // Konfiguracja MQTT z LWT
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URL,
        .session.last_will.topic = "amp/status",
        .session.last_will.msg = "offline",
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
        .session.keepalive = 20,  // DODAJ TO - 20 sekund
    };
    
    #ifdef MQTT_USERNAME
    mqtt_cfg.credentials.username = MQTT_USERNAME;
    #endif
    
    #ifdef MQTT_PASSWORD
    mqtt_cfg.credentials.authentication.password = MQTT_PASSWORD;
    #endif
    
    client = esp_mqtt_client_init(&mqtt_cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        vTaskDelete(NULL);
        return;
    }
    
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    
    esp_err_t err = esp_mqtt_client_start(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
    } else {
        // Utwórz task keepalive po udanym połączeniu
        xTaskCreate(mqtt_keepalive_task, "mqtt_keepalive", 4096, NULL, 1, NULL);
    }
    
    // Task działa w tle
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
