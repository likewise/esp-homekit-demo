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

#include <homekit/homekit.h>
#include <homekit/characteristics.h>
#include <wifi_config.h>
#include <rboot-api.h>
#include <stdout_redirect.h>

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

// IO02 used as GPIO input for zero-cross detector
const int zerocross_gpio = 3;
// triac_gpio used as output driving triac
const int triac_gpio = 1;

#ifndef DIMMER
void switch_on_callback(homekit_characteristic_t *_ch, homekit_value_t on, void *context);
#endif
void button_callback(uint8_t gpio, bool button_is_pressed, uint32_t period);

void led_write(bool on) {
    //gpio_write(led_gpio, on ? 0 : 1);
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
    vTaskDelay(1000 / portTICK_PERIOD_MS);

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

#ifndef DIMMER
/* callback on HomeKit event */
homekit_characteristic_t switch_on = HOMEKIT_CHARACTERISTIC_(
    ON, false, .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(switch_on_callback)
);
#endif

/* brightness get and set */
int g_brightness = 100;
homekit_value_t light_bri_get() { return HOMEKIT_INT(g_brightness); }
void light_bri_set(homekit_value_t value) {
    if (value.format != homekit_format_int) {
        printf("Invalid bri-value format: %d\n", value.format);
        return;
    }
    g_brightness = value.int_value;
    printf("brightness: %d\n", value.int_value);
}

/* light get and set */
bool g_on;

homekit_value_t light_on_get(void);
void light_on_set(homekit_value_t value);
homekit_characteristic_t lightbulb_on = HOMEKIT_CHARACTERISTIC_(ON, false, .getter=light_on_get, .setter=light_on_set);

homekit_value_t light_on_get() { return HOMEKIT_BOOL(g_on); }
void light_on_set(homekit_value_t value) {
    if (value.format != homekit_format_bool) {
        printf("Invalid on-value format: %d\n", value.format);
        return;
    }
    relay_write(value.bool_value);
    g_on = value.bool_value;
    printf("light on: %d\n", (int)value.bool_value);
}


static int zerocrossing_triggered = 0;
static int timer_count = 0;
static void IRAM frc1_interrupt_handler(void *arg)
{
    timer_count++;
    timer_set_run(FRC1, false);
    gpio_write(triac_gpio, 0);
    gpio_write(2, 1);
    gpio_write(led_gpio, 1);
    zerocrossing_triggered = 0;
}

static int zerocross_irq_count = 0;
static int zerocross_count = 0;

/* called on each positive edge of the zero-cross detector
 *
 * The RobotDyn AC Light Dimmer Module provides a very slow flank
 * on its zero-crossing detector output.
 * Along with noise on the power rail, this means the zero-to-one
 * edge detector in the ESP8266 will trigger multiple times during
 * the flank. (In my case ~10 times as visible on the scope).
 * 
 * This interrupt callback will be fired in rapid succession for the
 * same flank. Thus we must only act on the first trigger condition
 * for each interval, otherwise jitter and randomness occurs.
 * 
 * Reset the trigger condition once we have handled the interval
 * through the timer
 */
void zerocross_intr_callback(uint8_t gpio) {
    /* count each interrupt */
    zerocross_irq_count++;

#if 0
    /* ignore if already triggered */
    if (zerocrossing_triggered == 1) return;
#endif
    zerocross_count++;

    if (g_on && (g_brightness == 100)) {
        gpio_write(triac_gpio, 0);
        gpio_write(2, 1);
        gpio_write(led_gpio, 1);

        return;
    }
    else if (!g_on || (g_brightness == 0)) {
        gpio_write(triac_gpio, 1);
        gpio_write(2, 0);
        gpio_write(led_gpio, 0);
        return;
    }

    zerocrossing_triggered++;
    if ((zerocrossing_triggered == 1) || (zerocrossing_triggered == 100)) {

        /* short pulse on each zero-crossing*/
        gpio_write(triac_gpio, 1);
        gpio_write(2, 0);
        gpio_write(led_gpio, 0);

        timer_set_divider(FRC1, TIMER_CLKDIV_16);
        uint32_t interval_us = 7500;
        interval_us = 10000 - (((g_brightness * 10000/*10ms(100Hz)*/) + 50) / 100);
        /* single shot timer */
        uint32_t count = timer_time_to_count(FRC1, interval_us, TIMER_CLKDIV_16);
        //timer_set_timeout(FRC1, 6000/*6 ms*/);
        timer_set_load(FRC1, count);
        timer_set_reload(FRC1, false);
        timer_set_run(FRC1, true);
    }
}

void gpio_init() {
    gpio_enable(led_gpio, GPIO_OUTPUT);
    led_write(false);

    gpio_enable(relay_gpio, GPIO_OUTPUT);
#ifdef DIMMER
    relay_write(lightbulb_on.value.bool_value);
#else
    relay_write(switch_on.value.bool_value);
#endif
#if 1
    gpio_enable(triac_gpio, GPIO_OUTPUT);
    gpio_write(triac_gpio, 0);

    gpio_enable(2, GPIO_OUTPUT);
    gpio_write(2, 0);

    //gpio_set_pullup(triac_gpio, true, true);
#endif
#if 1
    gpio_enable(zerocross_gpio, GPIO_INPUT);
    gpio_set_interrupt(zerocross_gpio, GPIO_INTTYPE_EDGE_POS, zerocross_intr_callback);
#endif

    _xt_isr_attach(INUM_TIMER_FRC1, frc1_interrupt_handler, NULL);
    timer_set_reload(FRC1, false);
    timer_set_interrupts(FRC1, true);
    timer_set_run(FRC1, false);
}

#ifndef DIMMER
void switch_on_callback(homekit_characteristic_t *_ch, homekit_value_t on, void *context) {
    relay_write(switch_on.value.bool_value);
}
#endif

void button_callback(uint8_t gpio, bool button_is_pressed, uint32_t period) {
    if (button_is_pressed) {
      printf("button is being pressed (after %ums non-activity)\n", period);
      return;
    }
    printf("button was kept pressed for %ums and now released.\n", period);
    if (period < 1000) {
        printf("Toggling relay.\n");
#ifdef DIMMER
        lightbulb_on.value.bool_value = !lightbulb_on.value.bool_value;
        g_on = lightbulb_on.value.bool_value;
        relay_write(lightbulb_on.value.bool_value);
        homekit_characteristic_notify(&lightbulb_on, lightbulb_on.value);
#else            
        switch_on.value.bool_value = !switch_on.value.bool_value;
        relay_write(switch_on.value.bool_value);
        homekit_characteristic_notify(&switch_on, switch_on.value);
#endif
    } else if ((period > 2000) && (period <= 5000)) {
        reset_configuration(FLAG_UPDATE_OTA);
    } else if ((period > 5000) && (period <= 10000)) {
        reset_configuration(FLAG_RESET_HOMEKIT);
    } else if (period > 10000) {
        reset_configuration(FLAG_RESET_WIFI | FLAG_RESET_HOMEKIT | FLAG_UPDATE_OTA);
    }
    printf("zero-cross count: %u/%u, timer_count: %u\n", zerocross_count, zerocross_irq_count, timer_count);
}

void switch_identify_task(void *_args) {
    /* flash the LED so the user can identify this device */
    for (int i=0; i<3; i++) {
        for (int j=0; j<2; j++) {
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

void switch_identify(homekit_value_t _value) {
    printf("Switch identify\n");
    xTaskCreate(switch_identify_task, "Switch identify", 128, NULL, 2, NULL);
}

homekit_characteristic_t name = HOMEKIT_CHARACTERISTIC_(NAME, "Sonoff Switch");
homekit_characteristic_t serial = HOMEKIT_CHARACTERISTIC_(SERIAL_NUMBER, "000000000000");

homekit_accessory_t *accessories[] = {
    HOMEKIT_ACCESSORY(.id=1, .category=homekit_accessory_category_switch, .services=(homekit_service_t*[]){
        HOMEKIT_SERVICE(ACCESSORY_INFORMATION, .characteristics=(homekit_characteristic_t*[]){
            &name,
            HOMEKIT_CHARACTERISTIC(MANUFACTURER, "iTEAD"),
            &serial,
            HOMEKIT_CHARACTERISTIC(MODEL, "Basic"),
            HOMEKIT_CHARACTERISTIC(FIRMWARE_REVISION, "0.5.48"),
            HOMEKIT_CHARACTERISTIC(IDENTIFY, switch_identify),
            NULL
        }),
#ifdef DIMMER        
        HOMEKIT_SERVICE(LIGHTBULB, .primary=true,
            .characteristics=(homekit_characteristic_t*[]){
                HOMEKIT_CHARACTERISTIC(NAME, "Sonoff Dimmer"),
                &lightbulb_on,
                HOMEKIT_CHARACTERISTIC(BRIGHTNESS, 100, .getter=light_bri_get, .setter=light_bri_set),
            NULL
        }),
#else
        HOMEKIT_SERVICE(SWITCH, .primary=true, .characteristics=(homekit_characteristic_t*[]){
            HOMEKIT_CHARACTERISTIC(NAME, "Sonoff Switch"),
            &switch_on,
            NULL
        }),
#endif
        NULL
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

void user_init(void) {
    uart_set_baud(0, 115200);

    create_accessory_name_and_serial();
    printf("wifi_config_init()\n");
#if 1
    wifi_config_init("sonoff-switch", NULL, on_wifi_ready);
#else
    wifi_config_init2("sonoff-switch", NULL, on_wifi_event);
#endif
    gpio_init();

    if (button_create(button_gpio, 0, button_callback)) {
        printf("Failed to initialize button\n");
    }
}
