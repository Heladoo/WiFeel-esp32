#include "wifeel_proto.h"
#include <string.h>

/* CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflect, no xorout).
 * Bit-by-bit rather than table-driven: these frames are tiny (<=~120
 * bytes) and sent at most a few hundred times/sec, so the table's ROM
 * isn't worth spending on a board this memory-constrained. */
static uint16_t crc16_ccitt_false(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

size_t wifeel_proto_pack(uint8_t *out, wifeel_msg_type_t type, uint16_t seq,
                          const void *payload, size_t payload_len)
{
    if (!out || payload_len > sizeof(wifeel_msg_payload_t)) {
        return 0;
    }
    if (payload_len > 0 && !payload) {
        return 0;
    }

    wifeel_msg_header_t hdr = {
        .magic   = WIFEEL_PROTO_MAGIC,
        .version = WIFEEL_PROTO_VERSION,
        .type    = (uint8_t)type,
        .seq     = seq,
    };
    memcpy(out, &hdr, sizeof(hdr));
    if (payload_len > 0) {
        memcpy(out + sizeof(hdr), payload, payload_len);
    }

    size_t body_len = sizeof(hdr) + payload_len;
    uint16_t crc = crc16_ccitt_false(out, body_len);
    out[body_len]     = (uint8_t)(crc & 0xFF);
    out[body_len + 1] = (uint8_t)((crc >> 8) & 0xFF);

    return body_len + sizeof(uint16_t);
}

bool wifeel_proto_unpack(const uint8_t *buf, size_t len,
                          wifeel_msg_type_t *type, uint16_t *seq,
                          const void **payload_out, size_t *payload_len_out)
{
    if (!buf || len < sizeof(wifeel_msg_header_t) + sizeof(uint16_t)) {
        return false;
    }

    wifeel_msg_header_t hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.magic != WIFEEL_PROTO_MAGIC || hdr.version != WIFEEL_PROTO_VERSION) {
        return false;
    }

    size_t body_len = len - sizeof(uint16_t);
    uint16_t recv_crc = (uint16_t)buf[body_len] | ((uint16_t)buf[body_len + 1] << 8);
    uint16_t calc_crc = crc16_ccitt_false(buf, body_len);
    if (recv_crc != calc_crc) {
        return false;
    }

    size_t payload_len = body_len - sizeof(hdr);
    if (payload_len > sizeof(wifeel_msg_payload_t)) {
        return false; /* corrupt/malicious frame claiming an oversized payload */
    }

    if (type) {
        *type = (wifeel_msg_type_t)hdr.type;
    }
    if (seq) {
        *seq = hdr.seq;
    }
    if (payload_out) {
        *payload_out = buf + sizeof(hdr);
    }
    if (payload_len_out) {
        *payload_len_out = payload_len;
    }
    return true;
}

const char *wifeel_vendor_name(uint8_t vendor)
{
    switch (vendor) {
        case WIFEEL_VENDOR_APPLE:     return "Apple";
        case WIFEEL_VENDOR_SAMSUNG:   return "Samsung";
        case WIFEEL_VENDOR_GOOGLE:    return "Google";
        case WIFEEL_VENDOR_MICROSOFT: return "Microsoft";
        case WIFEEL_VENDOR_OTHER:     return "Other";
        default:                      return "Unknown";
    }
}
