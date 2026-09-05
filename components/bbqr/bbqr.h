#ifndef QR_BRIDGE_BBQR_H
#define QR_BRIDGE_BBQR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Length of a part header: B$, the encoding, the file type, two base
// 36 digits of part count and two of part index.
#define BBQR_HEADER_LEN 8

// Largest part count two base 36 digits express.
#define BBQR_MAX_PARTS 1295

// Characters the unpadded base32 of n bytes takes.
#define BBQR_BASE32_LEN(n) (((n) * 8 + 4) / 5)

// Encode n bytes as unpadded RFC 4648 base32 into out, which holds
// BBQR_BASE32_LEN(n) characters. Returns the character count.
size_t bbqr_base32(const uint8_t *in, size_t n, char *out);

// A series: one base32 payload cut into parts that each fit a tag.
typedef struct {
    const char *encoded;  // The base32 payload.
    size_t len;           // Its length in characters.
    size_t per_part;      // Characters in every part but the last.
    size_t parts;
    char file_type;
} bbqr_series_t;

// Plan a series whose parts, header included, are at most capacity
// characters long. Mirrors the reference splitter: every part but the
// last carries the same number of characters, a multiple of 8 so it
// decodes to whole bytes, and the last carries the rest. Returns false
// if the payload needs more than BBQR_MAX_PARTS parts or capacity has
// no room for a body.
bool bbqr_split(bbqr_series_t *s, const char *encoded, size_t len,
                char file_type, size_t capacity);

// Write part i, header and body, to out, which holds capacity
// characters. Returns its length.
size_t bbqr_part(const bbqr_series_t *s, size_t i, char *out);

#ifdef __cplusplus
}
#endif

#endif
