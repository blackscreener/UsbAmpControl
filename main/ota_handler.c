#include "ota_handler.h"
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "esp_https_ota.h"
#include "esp_flash_partitions.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"

static const char *TAG = "OTA_HANDLER";

// Zmienne globalne OTA
static esp_ota_handle_t ota_handle = 0;
static const esp_partition_t *update_partition = NULL;
static volatile ota_status_t ota_status = OTA_IDLE;
static char ota_last_error[256] = {0};
static TaskHandle_t ota_task_handle = NULL;
static ota_progress_cb_t progress_callback = NULL;
static ota_complete_cb_t complete_callback = NULL;
static SemaphoreHandle_t ota_mutex = NULL;
static int ota_total_size = 0;
static int ota_received_size = 0;

// Inicjalizacja OTA
void ota_init(void) {
    ESP_LOGI(TAG, "Initializing OTA handler");
    
    // Utwórz mutex dla bezpiecznego dostępu
    ota_mutex = xSemaphoreCreateMutex();
    if (ota_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create OTA mutex");
    }
    
    // Wyświetl informacje o partycjach
    ota_print_partition_info();
    
    ESP_LOGI(TAG, "OTA handler initialized");
}

// Wyświetl informacje o partycjach
void ota_print_partition_info(void) {
    ESP_LOGI(TAG, "=== OTA Partition Info ===");
    
    // Bieżąca partycja
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        ESP_LOGI(TAG, "Running partition: type=%d, subtype=%d, address=0x%08x, size=%d",
                running->type, running->subtype, running->address, running->size);
        ESP_LOGI(TAG, "Running partition label: %s", running->label);
    }
    
    // Partycja boot
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (boot) {
        ESP_LOGI(TAG, "Boot partition: %s (0x%08x)", boot->label, boot->address);
    }
    
    // Następna partycja dla OTA
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (next) {
        ESP_LOGI(TAG, "Next OTA partition: %s (0x%08x)", next->label, next->address);
    }
    
    ESP_LOGI(TAG, "==========================");
}

// Pobierz wersję firmware
const char* ota_get_firmware_version(void) {
    // Możesz zwrócić stałą lub odczytać z partition
    return "1.0.0";
}

// Sprawdź status OTA
ota_status_t ota_get_status(void) {
    return ota_status;
}

// Ostatni błąd
const char* ota_get_last_error(void) {
    return ota_last_error;
}

// Czy OTA jest w trakcie
bool ota_is_in_progress(void) {
    return (ota_status == OTA_IN_PROGRESS);
}

// Anuluj OTA
void ota_cancel(void) {
    if (xSemaphoreTake(ota_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (ota_status == OTA_IN_PROGRESS && ota_handle != 0) {
            ESP_LOGI(TAG, "Cancelling OTA update");
            esp_ota_abort(ota_handle);
            ota_handle = 0;
            ota_status = OTA_FAILED;
            strlcpy(ota_last_error, "Update cancelled by user", sizeof(ota_last_error));
            
            if (complete_callback) {
                complete_callback(false, "Update cancelled");
            }
            
            if (ota_task_handle != NULL) {
                vTaskDelete(ota_task_handle);
                ota_task_handle = NULL;
            }
        }
        xSemaphoreGive(ota_mutex);
    }
}

// Aktualizuj progress
static void ota_update_progress(int received, int total, const char* status) {
    if (progress_callback) {
        int percent = 0;
        if (total > 0) {
            percent = (received * 100) / total;
        }
        progress_callback(percent, status);
    }
    
    // Logowanie co 10%
    static int last_percent = -10;
    int current_percent = (received * 100) / total;
    if (current_percent - last_percent >= 10 || current_percent == 100) {
        ESP_LOGI(TAG, "OTA progress: %d%% (%d/%d) - %s", 
                current_percent, received, total, status);
        last_percent = current_percent;
    }
}

// Handler uploadu przez HTTP
esp_err_t ota_http_upload_handler(httpd_req_t *req) {
    esp_err_t err = ESP_OK;
    char *buf = NULL;
    int total_len = req->content_len;
    int cur_len = 0;
    int received = 0;
    int content_length = 0;
    
    // Pobierz Content-Length
    char content_len_str[32];
    if (httpd_req_get_hdr_value_len(req, "Content-Length") > 0) {
        httpd_req_get_hdr_value_str(req, "Content-Length", content_len_str, sizeof(content_len_str));
        content_length = atoi(content_len_str);
        ESP_LOGI(TAG, "Content-Length: %d", content_length);
    }
    
    if (total_len <= 0) {
        ESP_LOGE(TAG, "Invalid content length: %d", total_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_FAIL;
    }
    
    // Sprawdź czy OTA już trwa
    if (xSemaphoreTake(ota_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_503_SERVICE_UNAVAILABLE, "OTA system busy");
        return ESP_FAIL;
    }
    
    if (ota_status == OTA_IN_PROGRESS) {
        xSemaphoreGive(ota_mutex);
        httpd_resp_send_err(req, HTTPD_409_CONFLICT, "OTA already in progress");
        return ESP_FAIL;
    }
    
    // Rozpocznij nową aktualizację
    ota_status = OTA_IN_PROGRESS;
    ota_total_size = total_len;
    ota_received_size = 0;
    strlcpy(ota_last_error, "", sizeof(ota_last_error));
    
    // Pobierz partycję do aktualizacji
    update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        xSemaphoreGive(ota_mutex);
        ESP_LOGE(TAG, "No OTA partition found");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        ota_status = OTA_FAILED;
        strlcpy(ota_last_error, "No OTA partition found", sizeof(ota_last_error));
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Writing to partition: %s, subtype %d, offset 0x%x, size %d",
             update_partition->label, update_partition->subtype, 
             update_partition->address, update_partition->size);
    
    // Sprawdź czy plik mieści się w partycji
    if (total_len > update_partition->size) {
        xSemaphoreGive(ota_mutex);
        ESP_LOGE(TAG, "Firmware too large: %d > %d", total_len, update_partition->size);
        httpd_resp_send_err(req, HTTPD_413_REQUEST_ENTITY_TOO_LARGE, "Firmware too large");
        ota_status = OTA_FAILED;
        strlcpy(ota_last_error, "Firmware too large for partition", sizeof(ota_last_error));
        return ESP_FAIL;
    }
    
    // Rozpocznij OTA
    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        xSemaphoreGive(ota_mutex);
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        ota_status = OTA_FAILED;
        snprintf(ota_last_error, sizeof(ota_last_error), "OTA begin failed: %s", esp_err_to_name(err));
        return err;
    }
    
    xSemaphoreGive(ota_mutex);
    
    // Bufor do odbioru danych
    buf = malloc(4096);
    if (buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        esp_ota_abort(ota_handle);
        ota_handle = 0;
        ota_status = OTA_FAILED;
        strlcpy(ota_last_error, "Memory allocation failed", sizeof(ota_last_error));
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "Starting OTA update, total size: %d bytes", total_len);
    ota_update_progress(0, total_len, "Receiving firmware...");
    
    // Odbieraj dane
    while (cur_len < total_len) {
        received = httpd_req_recv(req, buf, MIN(4096, total_len - cur_len));
        
        if (received < 0) {
            // Błąd odbioru
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "Socket timeout, continuing...");
                continue;
            }
            ESP_LOGE(TAG, "Receive error: %d", received);
            err = ESP_FAIL;
            break;
        } else if (received == 0) {
            // Połączenie zamknięte
            ESP_LOGE(TAG, "Connection closed by client");
            err = ESP_FAIL;
            break;
        }
        
        // Zapisz dane do partycji OTA
        err = esp_ota_write(ota_handle, (const void *)buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            break;
        }
        
        cur_len += received;
        ota_received_size = cur_len;
        ota_update_progress(cur_len, total_len, "Writing firmware...");
    }
    
    free(buf);
    
    if (xSemaphoreTake(ota_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA system error");
        return ESP_FAIL;
    }
    
    if (err == ESP_OK) {
        // Zakończ OTA
        ESP_LOGI(TAG, "OTA write complete, validating...");
        ota_update_progress(total_len, total_len, "Validating firmware...");
        
        err = esp_ota_end(ota_handle);
        if (err != ESP_OK) {
            if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
                ESP_LOGE(TAG, "Image validation failed - image is corrupted");
                snprintf(ota_last_error, sizeof(ota_last_error), 
                        "Image validation failed: corrupted or incompatible");
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware validation failed");
            } else {
                ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
                snprintf(ota_last_error, sizeof(ota_last_error), 
                        "OTA end failed: %s", esp_err_to_name(err));
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end failed");
            }
            ota_status = OTA_FAILED;
            xSemaphoreGive(ota_mutex);
            return err;
        }
        
        // Ustaw nową partycję bootową
        ESP_LOGI(TAG, "Setting boot partition...");
        err = esp_ota_set_boot_partition(update_partition);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
            snprintf(ota_last_error, sizeof(ota_last_error), 
                    "Boot partition set failed: %s", esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Boot partition set failed");
            ota_status = OTA_FAILED;
            xSemaphoreGive(ota_mutex);
            return err;
        }
        
        ota_status = OTA_SUCCESS;
        ESP_LOGI(TAG, "OTA update successful! Restarting in 3 seconds...");
        
        // Wyślij odpowiedź
        const char *success_resp = 
            "{\"status\":\"success\",\"message\":\"Firmware updated successfully. Device will restart.\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, success_resp, strlen(success_resp));
        
        xSemaphoreGive(ota_mutex);
        
        // Restart po opóźnieniu
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
        
    } else {
        // Anuluj OTA w przypadku błędu
        ESP_LOGE(TAG, "OTA update failed, aborting...");
        esp_ota_abort(ota_handle);
        ota_handle = 0;
        ota_status = OTA_FAILED;
        
        const char *error_resp = 
            "{\"status\":\"error\",\"message\":\"OTA update failed\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, error_resp, strlen(error_resp));
        
        xSemaphoreGive(ota_mutex);
    }
    
    return err;
}

// Task dla OTA przez HTTP download
static void ota_http_download_task(void *pvParameters) {
    char *url = (char *)pvParameters;
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 10000,
        .keep_alive_enable = true,
    };
    
    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };
    
    ESP_LOGI(TAG, "Starting OTA from URL: %s", url);
    
    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA successful, restarting...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));
        if (complete_callback) {
            complete_callback(false, esp_err_to_name(ret));
        }
    }
    
    vTaskDelete(NULL);
}

// Rozpocznij OTA z URL
bool ota_start_from_url(const char *url, ota_progress_cb_t progress_cb, ota_complete_cb_t complete_cb) {
    if (ota_status == OTA_IN_PROGRESS) {
        ESP_LOGE(TAG, "OTA already in progress");
        return false;
    }
    
    progress_callback = progress_cb;
    complete_callback = complete_cb;
    
    // Utwórz task dla downloadu OTA
    BaseType_t result = xTaskCreate(ota_http_download_task, "ota_download", 8192, 
                                   (void*)url, 5, &ota_task_handle);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA task");
        return false;
    }
    
    ota_status = OTA_IN_PROGRESS;
    return true;
}
