#ifndef QR_BRIDGE_TEXT_RECORD_H
#define QR_BRIDGE_TEXT_RECORD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bytes the record adds to its payload: 7 up to 252 payload bytes, 10
// above.
#define TEXT_RECORD_OVERHEAD 10

// Build a single-Text-record NDEF message for payload into out.
// Returns the message length, or 0 if it does not fit out_max.
size_t text_record_build(const uint8_t *payload, size_t payload_len,
                         uint8_t *out, size_t out_max);

#ifdef __cplusplus
}
#endif

#endif
