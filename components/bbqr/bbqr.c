// bbqr.c -- the splitting half of BBQr, Coinkite's format for carrying
// one payload over a series of QR codes, here over a series of tags.
//
// Part layout, all characters from the QR alphanumeric set:
//
//   B$2U0300<body>
//   B$      fixed prefix
//   2       encoding: base32 (the only one this bridge emits)
//   U       file type: UTF-8 text
//   03 00   part count and part index, two base 36 digits each
//
// The SeedHammer decodes each part on its own and joins the bytes, so
// every part but the last must hold a multiple of 8 characters. The
// split rule is the reference implementation's with the tag's capacity
// in place of a QR version's character count.

#include "bbqr.h"

#include <string.h>

#define ENCODING '2'

static const char alphabet[32] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

size_t bbqr_base32(const uint8_t *in, size_t n, char *out) {
    size_t o = 0;
    uint32_t bits = 0;
    int have = 0;
    for (size_t i = 0; i < n; i++) {
        bits = bits << 8 | in[i];
        have += 8;
        while (have >= 5) {
            have -= 5;
            out[o++] = alphabet[(bits >> have) & 31];
        }
    }
    if (have > 0) {
        out[o++] = alphabet[(bits << (5 - have)) & 31];
    }
    return o;
}

bool bbqr_split(bbqr_series_t *s, const char *encoded, size_t len,
                char file_type, size_t capacity) {
    if (capacity < BBQR_HEADER_LEN + 8 || len == 0) {
        return false;
    }
    size_t body = capacity - BBQR_HEADER_LEN;
    body -= body % 8;
    size_t parts = (len + body - 1) / body;
    if (parts > BBQR_MAX_PARTS) {
        return false;
    }
    s->encoded = encoded;
    s->len = len;
    s->per_part = parts == 1 ? len : body;
    s->parts = parts;
    s->file_type = file_type;
    return true;
}

static void base36(size_t n, char *out) {
    static const char digits[36] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    out[0] = digits[n / 36];
    out[1] = digits[n % 36];
}

size_t bbqr_part(const bbqr_series_t *s, size_t i, char *out) {
    size_t off = i * s->per_part;
    size_t n = s->len - off;
    if (n > s->per_part) {
        n = s->per_part;
    }
    out[0] = 'B';
    out[1] = '$';
    out[2] = ENCODING;
    out[3] = s->file_type;
    base36(s->parts, &out[4]);
    base36(i, &out[6]);
    memcpy(&out[BBQR_HEADER_LEN], &s->encoded[off], n);
    return BBQR_HEADER_LEN + n;
}
