#include "devices.h"

#include "esp_random.h"

static uint32_t s_salt;

esp_err_t devices_init(void)
{
    s_salt = esp_random();
    return ESP_OK;
}

uint16_t devices_id_hash(const uint8_t mac[6])
{
    /* FNV-1a over salt + MAC, folded to 16 bits. Not cryptographic — it only
     * needs to keep raw MACs off the screen and out of logs. */
    uint32_t h = 2166136261u;
    for (int i = 0; i < 4; i++) {
        h = (h ^ ((s_salt >> (8 * i)) & 0xFF)) * 16777619u;
    }
    for (int i = 0; i < 6; i++) {
        h = (h ^ mac[i]) * 16777619u;
    }
    return (uint16_t)(h ^ (h >> 16));
}
