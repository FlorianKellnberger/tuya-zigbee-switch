#include "button.h"
#include "hal/printf_selector.h"
#include "hal/tasks.h"
#include "hal/timer.h"
#include <stdbool.h>
#include <stddef.h>

#define LONG_PRESS_RESET_TIME  10000


extern void on_reset_clicked(void *arg);

void _btn_gpio_callback(hal_gpio_pin_t pin, void *arg);
void _btn_update_callback(void *arg);
void btn_update_debounced(button_t *button, uint8_t is_pressed,
                          uint32_t changed_at);

void btn_init(button_t *button) {
    // During device startup, button may be already pressed, but this should not
    // be detected as user press. So, to avoid such situation, special init is
    // required.
    uint8_t state = hal_gpio_read(button->pin);

    button->pressed      = state == button->pressed_when_high;
    button->long_pressed = false;
    button->debounce_last_state = state;
    button->update_task.handler = _btn_update_callback;
    button->update_task.arg     = button;
    hal_tasks_init(&button->update_task);
    hal_gpio_callback(button->pin, _btn_gpio_callback, button);
}

void _btn_gpio_callback(hal_gpio_pin_t pin, void *arg) {
    button_t *button    = (button_t *)arg;
    uint8_t   new_state = hal_gpio_read(button->pin);

    if (new_state == button->debounce_last_state) {
        return;
    }

    hal_tasks_unschedule(&button->update_task);
    button->debounce_last_state  = new_state;
    button->debounce_last_change = hal_millis();
    hal_tasks_schedule(&button->update_task, button->debounce_delay_ms);
}

void _btn_update_callback(void *arg) {
    button_t *button = (button_t *)arg;

    btn_update_debounced(button,
                         button->debounce_last_state == button->pressed_when_high,
                         button->debounce_last_change);

    if (button->pressed && !button->reset_triggered) {
        uint32_t now = hal_millis();
        uint32_t pressed_for = now - button->pressed_at_ms;
        uint32_t next_check_ms = 100; // Default: check every 100ms while held

        if (!button->long_pressed) {
            // If we haven't hit the first long press yet, 
            // schedule specifically for that timing
            next_check_ms = (pressed_for < button->long_press_duration_ms) 
                            ? (button->long_press_duration_ms - pressed_for) 
                            : 0;
        } else {
            // We already hit the 2s long press, now we are waiting for the 10s reset
            // Schedule specifically for the 10s mark
            next_check_ms = (pressed_for < LONG_PRESS_RESET_TIME)
                            ? (LONG_PRESS_RESET_TIME - pressed_for)
                            : 100; // Safety poll
        }

        hal_tasks_schedule(&button->update_task, next_check_ms);
    }
}

void btn_update_debounced(button_t *button, uint8_t is_pressed,
                          uint32_t changed_at) {
    if (!button->pressed && is_pressed) {
        printf("Press detected\r\n");
        button->pressed_at_ms = changed_at;
        button->pressed       = true;
        if (button->on_press != NULL) {
            button->on_press(button->callback_param);
        }
        if (changed_at - button->released_at_ms < button->multi_press_duration_ms) {
            button->multi_press_cnt += 1;
            printf("Multi press detected: %d\r\n", button->multi_press_cnt);
            if (button->on_multi_press != NULL) {
                button->on_multi_press(button->callback_param, button->multi_press_cnt);
            }
        } else {
            button->multi_press_cnt = 1;
        }
    } else if (button->pressed && !is_pressed) {
        printf("Release detected\r\n");
        button->released_at_ms = changed_at;
        button->pressed          = false;
        button->long_pressed     = false;
        button->reset_triggered  = false;
        if (button->on_release != NULL) {
            button->on_release(button->callback_param);
        }
    }
    button->pressed = is_pressed;

    uint32_t now = hal_millis();
    if (is_pressed && !button->long_pressed &&
        (button->long_press_duration_ms <= (now - button->pressed_at_ms))) {
        button->long_pressed = true;
        printf("Long press detected\r\n");
        if (button->on_long_press != NULL) {
            button->on_long_press(button->callback_param);
        }
    }
    // long button press for reset
    if (is_pressed && !button->reset_triggered &&
        (LONG_PRESS_RESET_TIME <= (now - button->pressed_at_ms))) {
        printf("Long reset press detected\r\n");
        button->reset_triggered = true;
        on_reset_clicked(NULL); // Call it directly by name
    }
}
