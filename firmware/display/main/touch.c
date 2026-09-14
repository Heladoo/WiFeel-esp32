#include "touch.h"

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "bsp_amoled.h"

static const char *TAG = "touch";

#define PIN_I2C_SCL    GPIO_NUM_8
#define PIN_I2C_SDA    GPIO_NUM_18
#define TOUCH_I2C_ADDR 0x38

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_touch_dev;

static esp_err_t touch_reg_write(uint8_t reg, const uint8_t *buf, size_t len)
{
    uint8_t tx[1 + 8];
    if (len > sizeof(tx) - 1) {
        return ESP_ERR_INVALID_ARG;
    }
    tx[0] = reg;
    for (size_t i = 0; i < len; i++) {
        tx[1 + i] = buf[i];
    }
    return i2c_master_transmit(s_touch_dev, tx, 1 + len, pdMS_TO_TICKS(1000));
}

static esp_err_t touch_reg_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_touch_dev, &reg, 1, buf, len, pdMS_TO_TICKS(1000));
}

static uint8_t touch_read_coords(uint16_t *x, uint16_t *y)
{
    uint8_t touch_count = 0;
    if (touch_reg_read(0x02, &touch_count, 1) != ESP_OK || touch_count != 1) {
        return 0;
    }
    uint8_t buf[4];
    if (touch_reg_read(0x03, buf, sizeof(buf)) != ESP_OK) {
        return 0;
    }
    *x = (((uint16_t)buf[0] & 0x0f) << 8) | (uint16_t)buf[1];
    *y = (((uint16_t)buf[2] & 0x0f) << 8) | (uint16_t)buf[3];
    if (*x > WIFEEL_LCD_H_RES) {
        *x = WIFEEL_LCD_H_RES;
    }
    if (*y > WIFEEL_LCD_V_RES) {
        *y = WIFEEL_LCD_V_RES;
    }
    return 1;
}

static void lvgl_indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    uint16_t x, y;
    if (touch_read_coords(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

esp_err_t touch_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = PIN_I2C_SCL,
        .sda_io_num = PIN_I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOUCH_I2C_ADDR,
        .scl_speed_hz = 300000,
    };
    err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_touch_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Switch the FT6146 to normal mode — retry a few times since it can be
     * mid-power-on right after the panel reset. */
    uint8_t normal_mode = 0x00;
    for (int attempt = 0; attempt < 4; attempt++) {
        if (touch_reg_write(0x86, &normal_mode, 1) == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, lvgl_indev_read_cb);

    return ESP_OK;
}
