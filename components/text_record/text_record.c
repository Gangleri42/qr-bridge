// text_record.c -- one NDEF well-known Text record (TNF 1).
//
// Byte layout matches ndeflib's TextRecord encoder and the
// SeedHammer's NDEF reader:
//
//   short (plen <= 255):  d1 01 <plen> 54 02 65 6e <payload>
//   long  (plen > 255):   c1 01 <plen, 4 bytes> 54 02 65 6e <payload>
//
// where plen = 3 + payload_len (status byte 0x02 = UTF-8 with a
// two-byte language code, "en", then the payload) and the header is
// MB|ME|SR (d1) or MB|ME (c1) with TNF 1. The message is the record.

#include "text_record.h"

#include <string.h>

size_t text_record_build(const uint8_t *payload, size_t payload_len,
                         uint8_t *out, size_t out_max) {
    size_t plen = payload_len + 3;
    size_t hdr = plen <= 255 ? 4 : 7;
    size_t msg_len = hdr + 3 + payload_len;
    if (msg_len > out_max) {
        return 0;
    }
    if (plen <= 255) {
        out[0] = 0xd1;  // MB|ME|SR, TNF well-known.
        out[1] = 0x01;  // Type length.
        out[2] = (uint8_t)plen;
        out[3] = 'T';
    } else {
        out[0] = 0xc1;  // MB|ME, TNF well-known.
        out[1] = 0x01;
        out[2] = (uint8_t)(plen >> 24);
        out[3] = (uint8_t)(plen >> 16);
        out[4] = (uint8_t)(plen >> 8);
        out[5] = (uint8_t)plen;
        out[6] = 'T';
    }
    out[hdr] = 0x02;  // UTF-8, two-byte language code.
    out[hdr + 1] = 'e';
    out[hdr + 2] = 'n';
    memcpy(&out[hdr + 3], payload, payload_len);
    return msg_len;
}
