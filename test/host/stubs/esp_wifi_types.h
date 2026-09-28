#pragma once
#include <stdint.h>
#include <stdbool.h>
/* Only the fields components/wifeel_csi actually touches. */
typedef struct { uint32_t timestamp; int8_t rssi; } wifi_pkt_rx_ctrl_t;
typedef struct {
    wifi_pkt_rx_ctrl_t rx_ctrl;
    uint8_t  mac[6];
    int8_t  *buf;
    uint16_t len;
    bool     first_word_invalid;
} wifi_csi_info_t;
