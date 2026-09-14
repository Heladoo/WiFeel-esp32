#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "touch_bsp.h"
#include "i2c_bsp.h"
#include "user_config.h"
#include "esp_log.h"


void disp_touch_init(void)
{
  uint8_t data = 0x00;
  uint8_t count = 0x00;
  for(;;)
  {
    if((ESP_OK == i2c_writr_buff(disp_touch_dev_handle,0x86,&data,1)) || (count == 3))//Switch to normal mode
    {
      return ;
    }
    count++;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
uint8_t touch_read_coords(uint16_t *x,uint16_t *y)
{
  uint8_t data = 0;
  uint8_t buf[4];
  i2c_read_buff(disp_touch_dev_handle,0x02,&data,1);
  if(data == 1)
  {
    i2c_read_buff(disp_touch_dev_handle,0x03,buf,4);
    *x = (((uint16_t)buf[0] & 0x0f)<<8) | (uint16_t)buf[1];
    *y = (((uint16_t)buf[2] & 0x0f)<<8) | (uint16_t)buf[3];   
    if(*x > EXAMPLE_LCD_H_RES)
    *x = EXAMPLE_LCD_H_RES;
    if(*y > EXAMPLE_LCD_V_RES)
    *y = EXAMPLE_LCD_V_RES;
    //ESP_LOGI("Touch","(%d,%d)",*x,*y);
    return 1;
  }
  return 0;
}