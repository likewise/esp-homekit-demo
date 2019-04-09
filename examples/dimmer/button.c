#include <string.h>
#include <esplibs/libmain.h>

//#include "udplogger.c"
#include "button.h"

typedef struct _button {
    /* configuration */
    uint8_t gpio_num;
    uint16_t debounce_time;
    bool pressed_value;

    /* debounce */
    uint32_t last_intr_time;
    uint16_t glitches;

    /* last debounced transition */
    uint32_t last_transition_time;
    bool last_transition_pressed;

    /* registered callback function */
    button_callback_fn callback;

    struct _button *next;
} button_t;


button_t *buttons = NULL;

static button_t *button_find_by_gpio(const uint8_t gpio_num) {
    button_t *button = buttons;
    while (button && button->gpio_num != gpio_num)
        button = button->next;

    return button;
}

void button_intr_callback(uint8_t gpio) {
    button_t *button = button_find_by_gpio(gpio);
    if (!button)
        return;

    uint32_t now = xTaskGetTickCountFromISR();

    /* is the new button stable for at least the debounce time? */
    if ((now - button->last_intr_time) * portTICK_PERIOD_MS < button->debounce_time) {
        /* remember time, we want the button to be stable for at least debounce time */
        button->last_intr_time = now;
        button->last_transition_time = now;

        /* just for statistics */
        button->glitches++;
        /* during debounce time ignore events */
        return;
    }

    /* read button through GPIO */
    bool button_is_pressed = (gpio_read(button->gpio_num) == button->pressed_value);

    /* no transition? */
    if (button->last_transition_pressed == button_is_pressed) {
        button->last_intr_time = now;
        return;
    }

    /* valid transition */
    uint32_t button_state_time = ((now - button->last_intr_time) * portTICK_PERIOD_MS);
    printf("button %sed after %ums of being %sed (%u glitches)\n",
        button_is_pressed?"press":"releas", button_state_time,
        !button_is_pressed?"press":"releas",(int)button->glitches);
    if (button->callback) button->callback(button->gpio_num, button_is_pressed, button_state_time);
    button->last_transition_pressed = button_is_pressed;
    button->last_transition_time = now;
    button->last_intr_time = now;
    button->glitches = 0;
}

int button_create(const uint8_t gpio_num, bool pressed_value, button_callback_fn callback) {
    button_t *button = button_find_by_gpio(gpio_num);
    if (button)
        return -1;

    button = malloc(sizeof(button_t));
    memset(button, 0, sizeof(*button));
    button->gpio_num = gpio_num;
    button->pressed_value = pressed_value;
    // times in milliseconds
    button->debounce_time = 50;

    button->callback = callback;

    uint32_t now = xTaskGetTickCountFromISR();
    button->last_transition_pressed = (gpio_read(button->gpio_num) == button->pressed_value);
    button->last_transition_time = now;
    //button->glitches = 0;

    button->next = buttons;
    buttons = button;

    gpio_set_pullup(button->gpio_num, true, true);
    gpio_set_interrupt(button->gpio_num, GPIO_INTTYPE_EDGE_ANY, button_intr_callback);

    return 0;
}

void button_delete(const uint8_t gpio_num) {
    if (!buttons)
        return;

    button_t *button = NULL;
    if (buttons->gpio_num == gpio_num) {
        button = buttons;
        buttons = buttons->next;
    } else {
        button_t *b = buttons;
        while (b->next) {
            if (b->next->gpio_num == gpio_num) {
                button = b->next;
                b->next = b->next->next;
                break;
            }
        }
    }

    if (button) {
        gpio_set_interrupt(gpio_num, GPIO_INTTYPE_EDGE_ANY, NULL);
    }
}
