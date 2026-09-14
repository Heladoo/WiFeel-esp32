#ifndef TOUCH_BSP_H
#define TOUCH_BSP_H

#ifdef __cplusplus
extern "C" {
#endif

void disp_touch_init(void);
uint8_t touch_read_coords(uint16_t *x,uint16_t *y);

#ifdef __cplusplus
}
#endif

#endif
