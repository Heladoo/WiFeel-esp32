#include "bsp_amoled.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "bsp_amoled";

/* --- Pin assignments (confirmed, see this file's header comment) --- */
#define LCD_HOST          SPI2_HOST
#define PIN_LCD_CS        GPIO_NUM_10
#define PIN_LCD_PCLK      GPIO_NUM_11
#define PIN_LCD_DATA0     GPIO_NUM_4
#define PIN_LCD_DATA1     GPIO_NUM_5
#define PIN_LCD_DATA2     GPIO_NUM_6
#define PIN_LCD_DATA3     GPIO_NUM_7
#define PIN_LCD_RST       GPIO_NUM_3

#define LCD_BITS_PER_PIXEL 16
#define BYTES_PER_PIXEL    (LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565))
/* Two-line-of-50px double buffers — the exact size confirmed working on
 * real (no-PSRAM) DISP-1 hardware via Waveshare's own example. */
#define LVGL_BUF_HEIGHT     50
#define BUFF_SIZE           (WIFEEL_LCD_H_RES * LVGL_BUF_HEIGHT * BYTES_PER_PIXEL)

#define LVGL_TICK_PERIOD_MS    2
#define LVGL_TASK_MAX_DELAY_MS 500
#define LVGL_TASK_MIN_DELAY_MS 1
#define LVGL_TASK_STACK_SIZE   (8 * 1024)
#define LVGL_TASK_PRIORITY     5

static SemaphoreHandle_t s_lvgl_mux;
static SemaphoreHandle_t s_flush_done_sem;
static esp_lcd_panel_io_handle_t s_io_handle;

/* SH8601 init sequence: sleep-out, then panel-specific setup, then
 * display-on with brightness at max. Copied verbatim from the vendor
 * example — these are opaque vendor register writes, not something to
 * second-guess without a datasheet and a board to test against. */
static const sh8601_lcd_init_cmd_t s_lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 80},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 1},
    {0x63, (uint8_t[]){0xFF}, 1, 1},
    {0x51, (uint8_t[]){0x00}, 1, 1},
    {0x29, (uint8_t[]){0x00}, 0, 10},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
};

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                     esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    (void)user_ctx;
    BaseType_t high_task_awoken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done_sem, &high_task_awoken);
    return high_task_awoken == pdTRUE;
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *color_p)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);
    lv_draw_sw_rgb565_swap(color_p, lv_area_get_width(area) * lv_area_get_height(area));
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_p);
}

static void lvgl_flush_wait_cb(lv_display_t *disp)
{
    (void)disp;
    xSemaphoreTake(s_flush_done_sem, portMAX_DELAY);
}

/* SH8601 addresses even pixel pairs; round every invalidated area out to
 * an even boundary so partial-render flushes stay aligned. */
static void lvgl_rounder_cb(lv_event_t *e)
{
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

bool bsp_amoled_lvgl_lock(int timeout_ms)
{
    if (!s_lvgl_mux) {
        return false;
    }
    const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(s_lvgl_mux, timeout_ticks) == pdTRUE;
}

void bsp_amoled_lvgl_unlock(void)
{
    if (s_lvgl_mux) {
        xSemaphoreGive(s_lvgl_mux);
    }
}

static void lvgl_port_task(void *arg)
{
    (void)arg;
    uint32_t task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
    for (;;) {
        if (bsp_amoled_lvgl_lock(-1)) {
            task_delay_ms = lv_timer_handler();
            bsp_amoled_lvgl_unlock();
        }
        if (task_delay_ms > LVGL_TASK_MAX_DELAY_MS) {
            task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
        } else if (task_delay_ms < LVGL_TASK_MIN_DELAY_MS) {
            task_delay_ms = LVGL_TASK_MIN_DELAY_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
    }
}

void bsp_amoled_set_brightness(uint8_t brightness)
{
    if (!s_io_handle) {
        return;
    }
    uint32_t lcd_cmd = 0x51;
    lcd_cmd &= 0xff;
    lcd_cmd <<= 8;
    lcd_cmd |= 0x02 << 24;
    esp_lcd_panel_io_tx_param(s_io_handle, lcd_cmd, &brightness, 1);
}

esp_err_t bsp_amoled_init(void)
{
    s_flush_done_sem = xSemaphoreCreateBinary();
    if (!s_flush_done_sem) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "initializing SPI bus");
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_PCLK,
        .data0_io_num = PIN_LCD_DATA0,
        .data1_io_num = PIN_LCD_DATA1,
        .data2_io_num = PIN_LCD_DATA2,
        .data3_io_num = PIN_LCD_DATA3,
        .max_transfer_sz = WIFEEL_LCD_H_RES * WIFEEL_LCD_V_RES * LCD_BITS_PER_PIXEL / 8,
        .isr_cpu_id = ESP_INTR_CPU_AFFINITY_0,
    };
    esp_err_t err = spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "installing panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .on_color_trans_done = notify_lvgl_flush_ready,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags.quad_mode = true,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle);
    if (err != ESP_OK) {
        return err;
    }
    s_io_handle = io_handle;

    sh8601_vendor_config_t vendor_config = {
        .init_cmds = s_lcd_init_cmds,
        .init_cmds_size = sizeof(s_lcd_init_cmds) / sizeof(s_lcd_init_cmds[0]),
        .flags.use_qspi_interface = 1,
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
        .vendor_config = &vendor_config,
    };

    ESP_LOGI(TAG, "installing SH8601 panel driver");
    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_set_gap(panel_handle, 0x06, 0x00));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ESP_LOGI(TAG, "initializing LVGL");
    lv_init();
    lv_display_t *disp = lv_display_create(WIFEEL_LCD_H_RES, WIFEEL_LCD_V_RES);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);
    lv_display_set_flush_wait_cb(disp, lvgl_flush_wait_cb);

    uint8_t *buf_1 = heap_caps_malloc(BUFF_SIZE, MALLOC_CAP_DMA);
    uint8_t *buf_2 = heap_caps_malloc(BUFF_SIZE, MALLOC_CAP_DMA);
    if (!buf_1 || !buf_2) {
        ESP_LOGE(TAG, "failed to allocate %d-byte DMA LVGL buffers (free heap too low?)", BUFF_SIZE);
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_buffers(disp, buf_1, buf_2, BUFF_SIZE, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(disp, panel_handle);
    lv_display_add_event_cb(disp, lvgl_rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    const esp_timer_create_args_t tick_timer_args = {
        .callback = &lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_timer_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    s_lvgl_mux = xSemaphoreCreateMutex();
    if (!s_lvgl_mux) {
        return ESP_ERR_NO_MEM;
    }
    xTaskCreate(lvgl_port_task, "lvgl_port", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);

    return ESP_OK;
}
