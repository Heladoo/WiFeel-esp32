#ifndef USER_CONFIG_H
#define USER_CONFIG_H

//spi handle
#define SDSPI_HOST SPI2_HOST
#define LCD_HOST SPI2_HOST

// I2C
#define ESP32_SCL_NUM (GPIO_NUM_8)
#define ESP32_SDA_NUM (GPIO_NUM_18)

//  DISP
#define EXAMPLE_LCD_H_RES              466
#define EXAMPLE_LCD_V_RES              466
#define LVGL_BUF_HEIGHT                50 

//#define Backlight_Testing
//#define EXAMPLE_Rotate_90

#define EXAMPLE_PIN_NUM_LCD_CS            (GPIO_NUM_10)
#define EXAMPLE_PIN_NUM_LCD_PCLK          (GPIO_NUM_11) 
#define EXAMPLE_PIN_NUM_LCD_DATA0         (GPIO_NUM_4)
#define EXAMPLE_PIN_NUM_LCD_DATA1         (GPIO_NUM_5)
#define EXAMPLE_PIN_NUM_LCD_DATA2         (GPIO_NUM_6)
#define EXAMPLE_PIN_NUM_LCD_DATA3         (GPIO_NUM_7)
#define EXAMPLE_PIN_NUM_LCD_RST           (GPIO_NUM_3)

#define DISP_TOUCH_ADDR                   0x38
#define EXAMPLE_PIN_NUM_TOUCH_RST         (-1)
#define EXAMPLE_PIN_NUM_TOUCH_INT         (-1)


#endif