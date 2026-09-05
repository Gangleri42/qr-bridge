#ifndef QR_BRIDGE_TEXT_RECORD_H
#define QR_BRIDGE_TEXT_RECORD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Build a single-Text-record NDEF message for `payload`. Returns the
// message length written to `out`, or 0 if it does not fit.
size_t text_record_build(const uint8_t *payload, size_t payload_len,
                         uint8_t *out, size_t out_max);

#ifdef __cplusplus
}
#endif

#endif