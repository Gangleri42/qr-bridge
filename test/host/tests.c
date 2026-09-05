// tests.c -- byte-level checks for text_record and tag_emu, built and
// run on the host. Each check names what a reader on the other side
// of the air gap would see.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tag_emu.h"
#include "text_record.h"

static int failures;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            failures++;                                     \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);     \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
        }                                                   \
    } while (0)

static void test_text_record_short(void) {
    uint8_t out[16];
    size_t n = text_record_build((const uint8_t *)"hi", 2, out, sizeof(out));
    static const uint8_t want[] = {0xd1, 0x01, 0x05, 'T', 0x02, 'e', 'n', 'h', 'i'};
    CHECK(n == sizeof(want), "short record length %zu", n);
    CHECK(memcmp(out, want, sizeof(want)) == 0, "short record bytes");
}

static void test_text_record_long(void) {
    uint8_t payload[300];
    memset(payload, 'a', sizeof(payload));
    uint8_t out[320];
    size_t n = text_record_build(payload, sizeof(payload), out, sizeof(out));
    static const uint8_t want[] = {0xc1, 0x01, 0x00, 0x00, 0x01, 0x2f, 'T', 0x02, 'e', 'n'};
    CHECK(n == 310, "long record length %zu", n);
    CHECK(memcmp(out, want, sizeof(want)) == 0, "long record header");
    CHECK(out[10] == 'a' && out[309] == 'a', "long record payload");
}

static void test_text_record_boundary(void) {
    uint8_t payload[253];
    memset(payload, 'x', sizeof(payload));
    uint8_t out[270];
    CHECK(text_record_build(payload, 252, out, sizeof(out)) == 259 && out[0] == 0xd1,
          "252-byte payload takes the short form");
    CHECK(text_record_build(payload, 253, out, sizeof(out)) == 263 && out[0] == 0xc1,
          "253-byte payload takes the long form");
    CHECK(text_record_build(payload, 253, out, 262) == 0, "too small a buffer is refused");
}

// Read the tag the way the SeedHammer does: block 3 for the CC, then
// as many data blocks as the CC advertises, 16 bytes at a time from
// block 4, which runs past the message into padding. Gathers the data
// area into tlv and returns the address of the read after which the
// tag first reported delivery, or -1.
static int read_like_reader(uint8_t *tlv, size_t max) {
    uint8_t blocks[16];
    tag_emu_read(3, blocks);
    CHECK(blocks[0] == 0xe1 && blocks[1] == 0x10 && blocks[3] == 0x00, "CC magic/version/access");
    CHECK(!tag_emu_delivered(), "reading the CC is not delivery");
    size_t data_blocks = (size_t)blocks[2] * 2;
    int delivered_at = -1;
    for (size_t b = 4; b < 4 + data_blocks; b += 4) {
        tag_emu_read((uint8_t)b, blocks);
        if (delivered_at < 0 && tag_emu_delivered()) {
            delivered_at = (int)b;
        }
        size_t off = (b - 4) * 4;
        if (off + 16 <= max) {
            memcpy(tlv + off, blocks, 16);
        }
    }
    CHECK(data_blocks * 4 <= max, "test buffer holds the data area");
    return delivered_at;
}

static void test_tag_emu_small(void) {
    static const uint8_t ndef[] = {0xd1, 0x01, 0x05, 'T', 0x02, 'e', 'n', 'h', 'i'};
    CHECK(tag_emu_load(ndef, sizeof(ndef)), "load a 9-byte message");
    CHECK(!tag_emu_delivered(), "not delivered before any read");

    uint8_t blocks[16];
    tag_emu_read(0, blocks);
    static const uint8_t uid[] = {0x08, 0x11, 0x22, 0x33};
    CHECK(memcmp(blocks, uid, 4) == 0, "block 0 carries the UID");
    CHECK(blocks[7] == (0x08 ^ 0x11 ^ 0x22 ^ 0x33), "BCC");
    CHECK(!tag_emu_delivered(), "reading the UID blocks is not delivery");

    uint8_t nfca[6];
    tag_emu_nfca(nfca);
    CHECK(nfca[0] == 0x44 && nfca[1] == 0x00 && nfca[5] == 0x00, "SENS_RES/SEL_RES");
    CHECK(nfca[2] == uid[1] && nfca[3] == uid[2] && nfca[4] == uid[3], "anti-collision UID bytes");

    // 2 + 9 + 1 = 12 TLV bytes fill three blocks; the last message
    // byte sits in block 6, inside the reader's first data read.
    uint8_t tlv[64];
    int at = read_like_reader(tlv, sizeof(tlv));
    CHECK(tlv[0] == 0x03 && tlv[1] == 9, "short TLV header");
    CHECK(memcmp(&tlv[2], ndef, sizeof(ndef)) == 0, "message bytes");
    CHECK(tlv[11] == 0xfe, "terminator");
    CHECK(at == 4, "delivered by the read at block 4, got %d", at);
}

static void test_tag_emu_limits(void) {
    uint8_t ndef[TAG_EMU_MAX_NDEF + 1];
    memset(ndef, 0x5a, sizeof(ndef));
    CHECK(!tag_emu_load(ndef, sizeof(ndef)), "one byte over the limit is refused");
    CHECK(tag_emu_load(ndef, TAG_EMU_MAX_NDEF), "the limit itself fits");

    // The last message byte lands in block 251, the tag's last block,
    // so delivery shows on the reader's final data read at 248.
    static uint8_t tlv[TAG_EMU_BLOCKS * 4];
    int at = read_like_reader(tlv, sizeof(tlv));
    CHECK(tlv[0] == 0x03 && tlv[1] == 0xff, "long TLV header");
    CHECK(tlv[2] == (TAG_EMU_MAX_NDEF >> 8) && tlv[3] == (TAG_EMU_MAX_NDEF & 0xff), "TLV length");
    CHECK(memcmp(&tlv[4], ndef, TAG_EMU_MAX_NDEF) == 0, "message bytes");
    CHECK(tlv[4 + TAG_EMU_MAX_NDEF] == 0xfe, "terminator in the last block");
    CHECK(at == 248, "delivered by the read at block 248, got %d", at);

    uint8_t blocks[16];
    tag_emu_read(255, blocks);
    static const uint8_t zeros[16] = {0};
    CHECK(memcmp(blocks, zeros, 16) == 0, "reads past the end are zeros");
}

int main(void) {
    test_text_record_short();
    test_text_record_long();
    test_text_record_boundary();
    test_tag_emu_small();
    test_tag_emu_limits();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    printf("ok\n");
    return EXIT_SUCCESS;
}
