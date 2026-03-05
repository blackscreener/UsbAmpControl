#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H

#include "usb_driver.h"

void mqtt_app_start_task(void *arg);
void mqtt_notify_state_changed(const state_t *state);
void mqtt_send_hass_discovery(void);
void mqtt_publish_usb_connected(bool connected);
void mqtt_publish_wifi_quality(void);
void mqtt_publish_uptime(void);

#endif
