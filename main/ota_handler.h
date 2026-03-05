#ifndef OTA_HANDLER_H
#define OTA_HANDLER_H

#include <stdbool.h>
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Status OTA
typedef enum {
    OTA_IDLE,
    OTA_IN_PROGRESS,
    OTA_SUCCESS,
    OTA_FAILED
} ota_status_t;

// Typ aktualizacji
typedef enum {
    OTA_TYPE_HTTP_UPLOAD,
    OTA_TYPE_HTTP_DOWNLOAD,
    OTA_TYPE_MQTT_TRIGGER
} ota_type_t;

// Callback dla UI/progressu
typedef void (*ota_progress_cb_t)(int progress_percent, const char* status);
typedef void (*ota_complete_cb_t)(bool success, const char* message);

// Inicjalizacja OTA
void ota_init(void);

// Handler do uploadu przez HTTP
esp_err_t ota_http_upload_handler(httpd_req_t *req);

// Rozpocznij OTA z URL (HTTP download)
bool ota_start_from_url(const char *url, ota_progress_cb_t progress_cb, ota_complete_cb_t complete_cb);

// Sprawdź status OTA
ota_status_t ota_get_status(void);

// Pobierz wersję firmware
const char* ota_get_firmware_version(void);

// Pobierz informacje o partycji
void ota_print_partition_info(void);

// Ostatni błąd
const char* ota_get_last_error(void);

// Sprawdź czy OTA jest w trakcie
bool ota_is_in_progress(void);

// Anuluj aktualizację OTA
void ota_cancel(void);

#ifdef __cplusplus
}
#endif

#endif // OTA_HANDLER_H
