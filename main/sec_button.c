/*
 * sec_button.c — switch Bluetooth security from the board's BOOT button.
 *
 * The /security page is the normal way to switch it, but a bridge without
 * WiFi can't be reached from a phone whose dial-in fails at the current
 * setting (an S60 2nd Edition phone with security off, or a renamed S60v1
 * phone in auto). The button breaks that loop:
 *
 *   long press (hold >= SEC_BTN_HOLD_MS) — step the saved mode
 *       off -> on -> auto -> off, blink the LED once for off, 3 times for on
 *       or twice for auto, then reboot to apply it
 *   short press — blink the mode in effect now, same code, no change
 *
 * The button is read only after boot. Holding BOOT while the board resets
 * still enters the ROM download mode, as on any ESP32, so the reboot waits
 * for the button to be released.
 *
 * Boards without a user LED on SEC_LED_GPIO still switch; the serial log and
 * the /security page show the result.
 */

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gateway.h"

static const char *TAG = "sec_btn";

#define SEC_BTN_GPIO        GPIO_NUM_0  /* BOOT button, pressed = low */
#define SEC_LED_GPIO        GPIO_NUM_2  /* blue LED on DevKit V1 boards */
#define SEC_LED_ON_LEVEL    1
#define SEC_BTN_HOLD_MS     3000
#define SEC_BTN_DEBOUNCE_MS 50
#define SEC_BTN_POLL_MS     20
#define SEC_BLINK_MS        250

static bool btn_down(void) {
    return gpio_get_level(SEC_BTN_GPIO) == 0;
}

static void led_set(bool on) {
    gpio_set_level(SEC_LED_GPIO, on ? SEC_LED_ON_LEVEL : !SEC_LED_ON_LEVEL);
}

/* 1 blink = off, 3 = on, 2 = auto. */
static void led_show(btsec_mode_t mode) {
    int n = mode == BTSEC_OFF ? 1 : mode == BTSEC_ON ? 3 : 2;
    for (int i = 0; i < n; i++) {
        led_set(true);
        vTaskDelay(pdMS_TO_TICKS(SEC_BLINK_MS));
        led_set(false);
        vTaskDelay(pdMS_TO_TICKS(SEC_BLINK_MS));
    }
}

static void wait_release(void) {
    while (btn_down()) vTaskDelay(pdMS_TO_TICKS(SEC_BTN_POLL_MS));
}

static void sec_button_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SEC_BTN_POLL_MS));
        if (!btn_down()) continue;

        uint32_t held = 0;
        while (btn_down() && held < SEC_BTN_HOLD_MS) {
            vTaskDelay(pdMS_TO_TICKS(SEC_BTN_POLL_MS));
            held += SEC_BTN_POLL_MS;
        }
        if (held < SEC_BTN_DEBOUNCE_MS) continue;

        if (held < SEC_BTN_HOLD_MS) {
            led_show(ppp_link_secure_get());
            continue;
        }

        /* Step from the saved value, not the one in effect: a change saved
         * from the web UI and still waiting for a reboot counts as current. */
        btsec_mode_t cur = ppp_link_secure_saved();
        btsec_mode_t next = cur == BTSEC_OFF ? BTSEC_ON :
                            cur == BTSEC_ON  ? BTSEC_AUTO : BTSEC_OFF;
        ESP_LOGI(TAG, "BOOT long press: Bluetooth security -> %s",
                 btsec_mode_str(next));
        ppp_link_secure_save(next);
        led_show(next);
        wait_release();
        ESP_LOGI(TAG, "rebooting to apply");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
}

void sec_button_start(void) {
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << SEC_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn);

    gpio_config_t led = {
        .pin_bit_mask = 1ULL << SEC_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led);
    led_set(false);

    gw_safe_task_create(sec_button_task, "sec_btn", 2560, NULL, 2, NULL);
}
