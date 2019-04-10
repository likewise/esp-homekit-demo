/*
 * Example of using esp-homekit library to control
 * a simple $5 Sonoff Basic using HomeKit.
 * The esp-wifi-config library is also used in this
 * example. This means you don't have to specify
 * your network's SSID and password before building.
 *
 * In order to flash the sonoff basic you will have to
 * have a 3.3V (logic level) FTDI adapter.
 *
 * To flash this example connect 3.3V, TX, RX, GND
 * in this order, beginning in the (square) pin header
 * next to the button.
 * Next hold down the button and connect the FTDI adapter
 * to your computer. The sonoff is now in flash mode and
 * you can flash the custom firmware.
 *
 * WARNING: Do not connect the sonoff to AC while it's
 * connected to the FTDI adapter! This may fry your
 * computer and sonoff.
 *
 */

#include <stdio.h>
#include <espressif/esp_wifi.h>
#include <espressif/esp_sta.h>
#include <espressif/esp_common.h>
#include <esp/uart.h>
#include <esp8266.h>
#include <FreeRTOS.h>
#include <task.h>
#include <timers.h>

#include <homekit/homekit.h>
#include <homekit/characteristics.h>
#include <wifi_config.h>
#include <rboot-api.h>
#include <stdout_redirect.h>

/* from RavenCore */
#include "custom_characteristics.h"
/* from LifeCycleManager */
#include "udplogger.h"
#include "button.h"

// The GPIO pin that is connected to the relay on the Sonoff Basic.
const int relay_gpio = 12;
// The GPIO pin that is connected to the LED on the Sonoff Basic.
const int led_gpio = 13;
// The GPIO pin that is connected to the button on the Sonoff Basic.
const int button_gpio = 0;

// UART TX = 1
// UART RX = 3
// IO2 = 2

// GPIO3/RX used as input for PIR
const int pir_gpio = 3;

int enable_ota_after_powerup_press = 1;

void button_callback(uint8_t gpio, bool button_is_pressed, uint32_t period);

void led_write(bool on) {
    gpio_write(led_gpio, on ? 0 : 1);
}

void led_flash_task(void *_args) {
    led_write(true);
    vTaskDelay(250 / portTICK_PERIOD_MS);
    led_write(false);
    vTaskDelete(NULL);
}

void relay_write(bool on) {
    gpio_write(relay_gpio, on ? 1 : 0);
    xTaskCreate(led_flash_task, "LED flash", 128, NULL, 2, NULL);
    printf("relay o%s\n", on? "n": "ff");
}

#define FLAG_RESET_WIFI (1 << 0)
#define FLAG_RESET_HOMEKIT (1 << 1)
#define FLAG_UPDATE_OTA (1 << 2)

void reset_configuration_task(void *pvParameters) {
    uint32_t flags =  *(uint32_t *)pvParameters;
    //Flash the LED first before we start the reset
    for (int i=0; i<3; i++) {
        led_write(true);
        vTaskDelay(100 / portTICK_PERIOD_MS);
        led_write(false);
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
    if (flags & FLAG_RESET_WIFI) {
        printf("Resetting Wifi Config\n");
        wifi_config_reset();
    }
    if (flags & FLAG_RESET_HOMEKIT) {
        printf("Resetting HomeKit Config\n");
        homekit_server_reset();
    }
    if (flags & FLAG_UPDATE_OTA) {
        printf("Requesting OTA Update\n");
        rboot_set_temp_rom(1);
    }
    //vTaskDelay(1000 / portTICK_PERIOD_MS);

    printf("Restarting\n");
    
    sdk_system_restart();
    
    vTaskDelete(NULL);
}

void reset_configuration(uint32_t flags) {
    static uint32_t flags_static;
    /* make a static copy of the flags, which can be safely referenced by the task (much) later;
     * the function argument is on the stack only during this function execution, not thereafter! */
    flags_static = flags;
    xTaskCreate(reset_configuration_task, "Reset configuration", 256, (void *)&flags_static, 2, NULL);
}

/* switch state */
bool g_switch_state;

homekit_value_t switch_state_get(void);
void switch_state_set(homekit_value_t value);

homekit_characteristic_t switch_state = HOMEKIT_CHARACTERISTIC_(ON, false, .getter=switch_state_get, .setter=switch_state_set);

/* switch state getter and setter for HomeKit */
homekit_value_t switch_state_get() { return HOMEKIT_BOOL(g_switch_state); }
void switch_state_set(homekit_value_t value) {
    if (value.format != homekit_format_bool) {
        printf("Invalid on-value format: %d\n", value.format);
        return;
    }
    relay_write(value.bool_value);
    g_switch_state = value.bool_value;
    printf("HomeKit turned switch %s\n", value.bool_value ? "on": "off");
    /* @TODO noticed a hang once right after previous log entry */
}

/* switch toggle from this application */
void switch_toggle() {
    switch_state.value.bool_value = !switch_state.value.bool_value;
    g_switch_state = switch_state.value.bool_value;
    relay_write(switch_state.value.bool_value);
    homekit_characteristic_notify(&switch_state, switch_state.value);
}

/* switch set from this application */
void switch_set(bool state) {
    switch_state.value.bool_value = state;
    g_switch_state = switch_state.value.bool_value;
    relay_write(switch_state.value.bool_value);
    homekit_characteristic_notify(&switch_state, switch_state.value);
}

TimerHandle_t absence_timer;

int g_pir_state = 0;

void absence_timer_cb(void *pvParameters) {
    (void)pvParameters;
    printf("Absence timer triggered.\n");
    if (!g_pir_state) {
        switch_set(g_pir_state);
    }
}

/* forward declarations */
homekit_value_t pir_getter();
homekit_characteristic_t pir_characteristic;

TaskHandle_t pir_event_task;
void pir_event_work(void *pvParameters) {

    while (true) {
        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

        if (notified == 1) {
            g_pir_state = gpio_read(pir_gpio);
            /* occupancy detected? */
            if (g_pir_state) {
                printf("Motion detected by PIR sensor. Enable switch.\n");
                switch_set(g_pir_state);
                xTimerStopFromISR(absence_timer, 0);
            /* no presence detected */
            } else {
                printf("No motion detected by PIR sensor. Starting absence timer.\n");
                /* start absence timer */
                xTimerStartFromISR(absence_timer, 0);
            }
            homekit_characteristic_notify(&pir_characteristic, pir_getter());
        }
    }
}

/* PIR GPIO event callback */
void pir_intr_callback(uint8_t gpio) {
    if (pir_event_task) {
        vTaskNotifyGiveFromISR(pir_event_task, NULL);
    }
}

void accessory_identify_task(void *pvParameters) {
    /* instead of pointer, carry blink count */
    uint32_t blink_count = (uint32_t)pvParameters;
    /* flash the LED so the user can identify this device */
    for (uint32_t i = 0; i < blink_count; i++) {
        for (int j = 0; j < 2; j++) {
            led_write(true);
            vTaskDelay(100 / portTICK_PERIOD_MS);
            led_write(false);
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
        vTaskDelay(250 / portTICK_PERIOD_MS);
    }
    led_write(false);
    vTaskDelete(NULL);
}

void accessory_identify(homekit_value_t _value) {
    printf("Accessory identify\n");
    xTaskCreate(accessory_identify_task, "Accessory identify", 128, (void *)3/*blink_count in pvParameters*/, 2, NULL);
}

void button_callback(uint8_t gpio, bool button_is_pressed, uint32_t pressed_period) {
    if (button_is_pressed) {
        printf("button is still being pressed for %ums\n", pressed_period);
        if (pressed_period == 2000) {
            xTaskCreate(accessory_identify_task, "Accessory identify", 128, (void *)2, 2, NULL);
        } else if (pressed_period == 5000) {
            xTaskCreate(accessory_identify_task, "Accessory identify", 128, (void *)3, 2, NULL);
        } else if (pressed_period == 10000) {
            xTaskCreate(accessory_identify_task, "Accessory identify", 128, (void *)4, 2, NULL);
        }
        return;
    }
    printf("button was kept pressed for %u ms and now released.\n", pressed_period);
    if ((pressed_period > 100) && (pressed_period < 1000)) {
        if (enable_ota_after_powerup_press) {
            printf("Request OTA Update, restarting\n");
            rboot_set_temp_rom(1);
            sdk_system_restart();
        } else {
            printf("Toggling relay.\n");
            switch_toggle();
        }
    } else
    if ((pressed_period > 2000) && (pressed_period <= 5000)) {
        reset_configuration(FLAG_UPDATE_OTA);
    } else if ((pressed_period > 5000) && (pressed_period <= 10000)) {
        reset_configuration(FLAG_RESET_HOMEKIT);
    } else if ((pressed_period > 10000) && (pressed_period <= 15000)) {
        reset_configuration(FLAG_RESET_WIFI | FLAG_RESET_HOMEKIT | FLAG_UPDATE_OTA);
    }
}

homekit_value_t pir_getter() {
    printf("PIR state (%d) was requested.\n", g_pir_state);
    return HOMEKIT_UINT8(g_pir_state ? 1 : 0);
}
homekit_characteristic_t pir_characteristic = HOMEKIT_CHARACTERISTIC_(OCCUPANCY_DETECTED, 0,
    .getter = pir_getter,
    .setter = NULL,
    NULL
);

homekit_characteristic_t name = HOMEKIT_CHARACTERISTIC_(NAME, "Sonoff Switch");
homekit_characteristic_t serial = HOMEKIT_CHARACTERISTIC_(SERIAL_NUMBER, "000000000000");


void ota_firmware_callback();
homekit_characteristic_t ota_firmware = HOMEKIT_CHARACTERISTIC_(CUSTOM_OTA_UPDATE, false, .id=110, .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(ota_firmware_callback));
void ota_firmware_callback() {
    if (ota_firmware.value.bool_value) {
        reset_configuration(FLAG_UPDATE_OTA);
    }
}

homekit_characteristic_t setup_service_name = HOMEKIT_CHARACTERISTIC_(NAME, "Setup", .id=100);

homekit_accessory_t *accessories[] = {
    HOMEKIT_ACCESSORY(.id=1, .category=homekit_accessory_category_switch, .services=(homekit_service_t*[]){
        HOMEKIT_SERVICE(ACCESSORY_INFORMATION, .characteristics=(homekit_characteristic_t*[]){
            &name,
            HOMEKIT_CHARACTERISTIC(MANUFACTURER, "iTEAD"),
            &serial,
            HOMEKIT_CHARACTERISTIC(MODEL, "Sonoff"),
            HOMEKIT_CHARACTERISTIC(FIRMWARE_REVISION, "0.1.11"),
            HOMEKIT_CHARACTERISTIC(IDENTIFY, accessory_identify),
            NULL
        }),
        HOMEKIT_SERVICE(
            OCCUPANCY_SENSOR,
            .primary=true,
            .characteristics=(homekit_characteristic_t*[]) {
                HOMEKIT_CHARACTERISTIC(NAME, "Occupancy"),
                &pir_characteristic,
                NULL
            },
        ),
        HOMEKIT_SERVICE(SWITCH, .primary=false, .characteristics=(homekit_characteristic_t*[]){
            HOMEKIT_CHARACTERISTIC(NAME, "Switch"),
            &switch_state,
            NULL
        }),        
        NULL /* zero terminate services */
    }),
    NULL
};

homekit_server_config_t config = {
    .accessories = accessories,
    .password = "111-11-111"
};

void on_wifi_ready() {
    /* enable UDP logging to port 45678, define printf to UDPLOG */
    /* capture with: sudo tcpdump -i enp3s0 udp port 45678 -v -X */

    UDPLOG("on_wifi_ready(); redirecting printf() to udplog_writer\n");
    xTaskCreate(udplog_send, "logsend", 256, NULL, 4/*prio*/, NULL);
    set_write_stdout(udplog_write);

    /* this can be used to reset redirection, typically printf() */
    //set_write_stdout(NULL);

    printf("homekit_server_init()\n");
    homekit_server_init(&config);
}

#if 0
void on_wifi_event(wifi_config_event_t event) {
    if (event == WIFI_CONFIG_CONNECTED) {
        printf("Connected to WiFi\n");
    } else if (event == WIFI_CONFIG_DISCONNECTED) {
        printf("Disconnected from WiFi\n");
    }
}
#endif

void create_accessory_name_and_serial() {
    uint8_t macaddr[6];
    sdk_wifi_get_macaddr(STATION_IF, macaddr);

    /* name includes last half of MAC address */
    int name_len = snprintf(NULL, 0, "Sonoff Switch %02X%02X%02X",
        macaddr[3], macaddr[4], macaddr[5]);
    char *name_value = malloc(name_len+1);
    (void)snprintf(name_value, name_len+1, "Sonoff Switch %02X%02X%02X",
        macaddr[3], macaddr[4], macaddr[5]);
    name.value = HOMEKIT_STRING(name_value);

    /* serial number is MAC address */
    int serial_len = snprintf(NULL, 0, "%02X%02X%02X%02X%02X%02X",
        macaddr[0], macaddr[1], macaddr[2], macaddr[3], macaddr[4], macaddr[5]);
    char *serial_value = malloc(serial_len+1);
    (void)snprintf(serial_value, serial_len+1, "%02X%02X%02X%02X%02X%02X",
        macaddr[0], macaddr[1], macaddr[2], macaddr[3], macaddr[4], macaddr[5]);
    serial.value = HOMEKIT_STRING(serial_value);
}

void ota_timeout_cb(void *pvParameters) {
    enable_ota_after_powerup_press = 0;
    printf("Disabling OTA button function.\n");
}

void gpio_init() {
    gpio_enable(led_gpio, GPIO_OUTPUT);
    led_write(false);

    gpio_enable(relay_gpio, GPIO_OUTPUT);
    relay_write(switch_state.value.bool_value);

#if 0
    gpio_set_pullup(pir_gpi, true, true);
#endif
    gpio_enable(pir_gpio, GPIO_INPUT);
    gpio_set_interrupt(pir_gpio, GPIO_INTTYPE_EDGE_ANY, pir_intr_callback);
}

void user_init(void) {
    uart_set_baud(0, 115200);

    create_accessory_name_and_serial();
    printf("wifi_config_init()\n");
#if 1
    wifi_config_init("sonoff-occupancy", NULL, on_wifi_ready);
#else
    wifi_config_init2("sonoff-occupancy", NULL, on_wifi_event);
#endif
    gpio_init();

    if (button_create(button_gpio, 0, button_callback)) {
        printf("Failed to initialize button\n");
    }

    TimerHandle_t ota_timer = xTimerCreate(NULL/*name*/, pdMS_TO_TICKS(8000), pdFALSE/*reload*/, 0, ota_timeout_cb);
    if (ota_timer) xTimerStart(ota_timer, 0);

    absence_timer = xTimerCreate(NULL/*name*/, pdMS_TO_TICKS(30000), pdFALSE/*reload*/, 0, absence_timer_cb);

    xTaskCreate(pir_event_work, "PIR", 256, (void *)NULL, 2, &pir_event_task);
}
