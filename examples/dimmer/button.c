#include <string.h>
#include <esplibs/libmain.h>

#include "timers.h"

#include "button.h"

typedef struct _button {
    /* configuration */
    uint8_t gpio_num;
    bool pressed_value;
#if 0    
    uint16_t debounce_time;

    /* debounce */
    uint32_t last_intr_time;
    uint16_t glitches;

    /* last debounced transition */
    uint32_t last_transition_time;
    bool last_transition_pressed;
#endif
    /* registered callback function */
    button_callback_fn callback;

    TimerHandle_t timer;
    bool timer_running;
    /* debounce state */
    int32_t integrator;
    /* debounced button state */
    bool is_pressed;
    int pressed_ticks;
    int released_ticks;

    struct _button *next;
} button_t;

button_t *buttons = NULL;

static button_t *button_find_by_gpio(const uint8_t gpio_num) {
    button_t *button = buttons;
    while (button && button->gpio_num != gpio_num)
        button = button->next;

    return button;
}

static button_t *button_find_by_timer(TimerHandle_t timer) {
    button_t *button = buttons;
    while (button && button->timer != timer)
        button = button->next;

    return button;
}

#define DEBOUNCE_TIME       0.1
#define SAMPLE_FREQUENCY    100
#define MAXIMUM         (DEBOUNCE_TIME * SAMPLE_FREQUENCY)

void button_timer_cb(void *pvParameters) {
    TimerHandle_t timer = (TimerHandle_t)pvParameters;
    button_t *button = button_find_by_timer(timer);
    if (!button) return;

    /* read current button state through GPIO */
    int sample_is_pressed = (gpio_read(button->gpio_num) == button->pressed_value);
    int button_changed = 0;

    /* integrating debounce algorithm */
    /* https://hackaday.com/2010/11/09/debounce-code-one-post-to-rule-them-all/
     * Original by Kenneth A. Kuhn */
    if (!sample_is_pressed) {
        /* update integrator */
        if (button->integrator > 0) {
            button->integrator -= 1;
            if (button->integrator <= 0) {
                /* change debounced button state */
                button_changed = button->is_pressed;
                button->is_pressed = 0;
                /* bound integrator */
                button->integrator = 0;
            }
        }
    } else {
        /* update integrator */
        if (button->integrator < MAXIMUM) {
            button->integrator += 1;
            if (button->integrator >= MAXIMUM) {
                /* change debounced button state */
                button_changed = !button->is_pressed;
                button->is_pressed = 1;
                /* bound integrator */
                button->integrator = MAXIMUM;
            }
        }
    }

    /* increment persistence timer of asserted debounced button state */
    if (button->is_pressed) {
        button->pressed_ticks++;
    } else {
        button->released_ticks++;
    }

    /* stop timer after button release */
    if (!button->is_pressed && (button->integrator == 0)) {
        if ((button->timer) && (button->timer_running)) {
            button->timer_running = 0;
            xTimerStop(button->timer, 0);
        }
    }

    printf("int: %d, pressed: %d, changed: %d, ticks: %d\n", button->integrator, (int)button->is_pressed, button_changed, button->pressed_ticks);

    /* button was just released? */
    if (button_changed && !button->is_pressed) {
        printf("button released after %d ms.\n", button->pressed_ticks * (1000 / SAMPLE_FREQUENCY));
        if (button->callback) button->callback(button->gpio_num, button->is_pressed, button->pressed_ticks * (1000 / SAMPLE_FREQUENCY));
        button->pressed_ticks = 0;
    /* button is still being pressed? */
    } else if (button->is_pressed && !button_changed) {
        /* report pressed interval every second */
        if ((button->pressed_ticks % SAMPLE_FREQUENCY) == 0) {
            printf("button is already held for %d ms.\n", button->pressed_ticks * (1000 / SAMPLE_FREQUENCY));
            if (button->callback) button->callback(button->gpio_num, button->is_pressed, button->pressed_ticks * (1000 / SAMPLE_FREQUENCY));
        }
    }

    /* reset persistence timer of de-asserted debounced button state */
    if (button->is_pressed) {
        button->released_ticks = 0;
    }
#if 0
    else {
        button->pressed_ticks = 0;
    }
#endif
}

void button_intr_callback(uint8_t gpio) {
    button_t *button = button_find_by_gpio(gpio);
    if (!button)
        return;

#if 0
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
#endif

    if ((button->timer) && (!button->timer_running)) {
        //xTimerChangePeriodFromISR(button->timer, pdMS_TO_TICKS(1000 / SAMPLE_FREQUENCY), 0);
        xTimerStartFromISR(button->timer, 0);
        button->timer_running = 1;
    }
#if 0
    /* read current button state through GPIO */
    int button_is_pressed = (gpio_read(button->gpio_num) == button->pressed_value);

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
    //if (button->callback) button->callback(button->gpio_num, button_is_pressed, button_state_time);
    button->last_transition_pressed = button_is_pressed;
    button->last_transition_time = now;
    button->last_intr_time = now;
    button->glitches = 0;
#endif    
}

int button_create(const uint8_t gpio_num, bool pressed_value, button_callback_fn callback) {
    button_t *button = button_find_by_gpio(gpio_num);
    if (button)
        return -1;

    button = malloc(sizeof(button_t));
    memset(button, 0, sizeof(*button));
    button->gpio_num = gpio_num;

    button->pressed_value = pressed_value;
    button->callback = callback;

#if 0
    // times in milliseconds
    button->debounce_time = 50;

    uint32_t now = xTaskGetTickCountFromISR();
    button->last_transition_pressed = (gpio_read(button->gpio_num) == button->pressed_value);
    button->last_transition_time = now;
    //button->glitches = 0;
#endif
    button->timer = xTimerCreate(NULL/*name*/, pdMS_TO_TICKS(1000 / SAMPLE_FREQUENCY), pdTRUE/*reload*/, 0, button_timer_cb);

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
        if (buttons == NULL) {
            /* last button deleted */
        }
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
