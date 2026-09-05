// text_record.c -- single NDEF well-known Text record (TNF 1).
//
// Byte layout matches ndeflib's TextRecord encoder and the SeedHammer's
// NDEF reader:
//
//   short (payload_len <= 255):
//     d1 01 <plen> 54 02 65 6e <payload>
//   long (payload_len > 255):
//     c1 01 <plen 4-byte> 54 02 65 6e <payload>
//
// where plen = 3 + payload_len (status byte 0x02 = UTF-8, "en", then
// the payload), header d1/c1 = MB|ME|SR(+TNF 1), and the message is
// the record bytes as-is. NLEN = message length.

#include "text_record.h"

#include <string.h>

// Build the message for `payload` into `out`. Returns the message
// length, or 0 if the payload does not fit.
size_t text_record_build(const uint8_t *payload, size_t payload_len,
                         uint8_t *out, size_t out_max) {
    size_t plen = payload_len + 3;  // status + "en" + payload.
    size_t msg_len;
    if (plen <= 255) {
        msg_len = 7 + payload_len;
        if (msg_len > out_max) {
            return 0;
        }
        out[0] = 0xd1;  // MB|ME|SR, TNF well-known.
        out[1] = 0x01;  // Type length.
        out[2] = (uint8_t)plen;
        out[3] = 'T';
    } else {
        msg_len = 10 + payload_len;
        if (msg_len > out_max) {
            return 0;
        }
        out[0] = 0xc1;  // MB|ME, TNF well-known (long form).
        out[1] = 0x01;
        out[2] = (uint8_t)(plen >> 24);
        out[3] = (uint8_t)(plen >> 16);
        out[4] = (uint8_t)(plen >> 8);
        out[5] = (uint8_t)(plen & 0xff);
        out[6] = 'T';
    }
    size_t o = (plen <= 255) ? 4 : 7;
    out[o++] = 0x02;  // Status: UTF-8, 2-byte language.
    out[o++] = 'e';
    out[o++] = 'n';
    memcpy(&out[o], payload, payload_len);
    return msg_len;
}