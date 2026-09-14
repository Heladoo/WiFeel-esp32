#include "board.h"

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "board";
static bool s_external_antenna = false;

#if CONFIG_IDF_TARGET_ESP32C6
/* Seeed XIAO ESP32-C6 antenna switch (FM8625H RF switch):
 *   GPIO3  LOW  = enable RF switch control
 *   GPIO14 HIGH = route to the external u.FL antenna (LOW = onboard ceramic)
 * Confirmed against Seeed's XIAO ESP32C6 wiki — see CLAUDE.md pin reference. */
#define ANTENNA_SWITCH_ENABLE_GPIO GPIO_NUM_3
#define ANTENNA_SWITCH_SELECT_GPIO GPIO_NUM_14

static esp_err_t select_external_antenna(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << ANTENNA_SWITCH_ENABLE_GPIO) | (1ULL << ANTENNA_SWITCH_SELECT_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "antenna switch gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = gpio_set_level(ANTENNA_SWITCH_ENABLE_GPIO, 0);
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_set_level(ANTENNA_SWITCH_SELECT_GPIO, 1);
    if (err != ESP_OK) {
        return err;
    }

    s_external_antenna = true;
    return ESP_OK;
}
#endif /* CONFIG_IDF_TARGET_ESP32C6 */

esp_err_t board_init(void)
{
#if CONFIG_IDF_TARGET_ESP32C6
    esp_err_t err = select_external_antenna();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to select external antenna, staying on onboard ceramic: %s",
                  esp_err_to_name(err));
    }
    return err;
#else
    ESP_LOGI(TAG, "no board-specific antenna switch for this target");
    return ESP_OK;
#endif
}

bool board_has_external_antenna(void)
{
    return s_external_antenna;
}
