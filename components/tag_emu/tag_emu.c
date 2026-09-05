// tag_emu.c -- virtual NFC Forum Type 2 tag (NTAG21x shape).
//
// The bridge presents the armed payload to the SeedHammer as a passive
// tag: the SeedHammer's always-on reader scans the emulated NTAG the
// way it scans a sticker, so nothing on the SeedHammer side has to
// enter card-emulation mode.
//
// Memory layout, as the SeedHammer's Type 2 reader expects it:
//   blocks 0-2: UID, BCC and lock bytes
//   block 3:    CC = E1 10 <size> 00   (size in 8-byte units)
//   blocks 4+:  NDEF TLV 03 [len | FF hi lo] <ndef> FE, padded to
//               whole blocks
// Anti-collision (REQA/ATQA, UID, SAK) is handled by the PN532's
// target-mode hardware; only READ commands reach this layer.

#include "tag_emu.h"

#include <string.h>

#define BLOCK_SIZE 4
#define READ_SIZE 16

// A single-size UID. 0x08 in the first byte marks it as random per
// ISO 14443-3, so no reader expects a manufacturer code.
static const uint8_t uid[4] = {0x08, 0x11, 0x22, 0x33};

static uint8_t vmem[TAG_EMU_BLOCKS * BLOCK_SIZE];
static size_t vmem_len;       // Bytes in use, a whole number of blocks.
static size_t last_block;     // Block holding the last message byte.
static bool delivered;

void tag_emu_nfca(uint8_t out[6]) {
    out[0] = 0x44;  // SENS_RES: single-size UID, bit-frame anticollision.
    out[1] = 0x00;
    out[2] = uid[1];
    out[3] = uid[2];
    out[4] = uid[3];
    out[5] = 0x00;  // SEL_RES: no ISO-DEP, a plain Type 2 tag.
}

bool tag_emu_load(const uint8_t *ndef, size_t ndef_len) {
    if (ndef_len > TAG_EMU_MAX_NDEF) {
        return false;
    }
    // TLV header: 03 len up to 254 bytes, 03 FF hi lo above.
    size_t hdr = ndef_len < 255 ? 2 : 4;
    size_t tlv_bytes = hdr + ndef_len + 1;  // Plus the terminator.
    size_t data_blocks = (tlv_bytes + BLOCK_SIZE - 1) / BLOCK_SIZE;
    size_t mem_blocks = 4 + data_blocks;
    // The reader stops at cc[2] * 8 bytes, which must reach the last
    // block: round up to whole 8-byte units.
    size_t cc_size = (mem_blocks + 1) / 2;
    size_t total = mem_blocks * BLOCK_SIZE;

    memset(vmem, 0, total);
    uint8_t bcc = uid[0] ^ uid[1] ^ uid[2] ^ uid[3];
    memcpy(&vmem[0], uid, sizeof(uid));
    vmem[7] = bcc;
    vmem[11] = bcc;
    vmem[12] = 0xe1;  // CC magic.
    vmem[13] = 0x10;  // Type 2 mapping version 1.0.
    vmem[14] = (uint8_t)cc_size;
    vmem[15] = 0x00;  // Read and write allowed.

    size_t off = 16;
    vmem[off++] = 0x03;  // NDEF message TLV.
    if (ndef_len < 255) {
        vmem[off++] = (uint8_t)ndef_len;
    } else {
        vmem[off++] = 0xff;
        vmem[off++] = (uint8_t)(ndef_len >> 8);
        vmem[off++] = (uint8_t)ndef_len;
    }
    memcpy(&vmem[off], ndef, ndef_len);
    off += ndef_len;
    vmem[off] = 0xfe;  // Terminator TLV.

    vmem_len = total;
    last_block = (off - 1) / BLOCK_SIZE;
    delivered = false;
    return true;
}

void tag_emu_read(uint8_t block, uint8_t out[READ_SIZE]) {
    size_t off = (size_t)block * BLOCK_SIZE;
    size_t n = off < vmem_len ? vmem_len - off : 0;
    if (n > READ_SIZE) {
        n = READ_SIZE;
    }
    memcpy(out, &vmem[off], n);
    memset(&out[n], 0, READ_SIZE - n);
    // The reader fetches the CC at block 3 (which for a tiny message
    // already shows the whole TLV) before it reads the data area, so
    // only a data read counts.
    if (block >= 4 && (size_t)block + READ_SIZE / BLOCK_SIZE - 1 >= last_block) {
        delivered = true;
    }
}

bool tag_emu_delivered(void) {
    return delivered;
}
