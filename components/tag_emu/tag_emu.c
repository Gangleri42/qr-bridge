// tag_emu.c -- virtual NFC Forum Type 2 tag (NTAG21x shape).
//
// The bridge presents the armed payload to the SeedHammer as a passive
// tag: the SeedHammer's always-on reader scans the emulated NTAG the
// way it scans a sticker, so nothing on the SeedHammer side has to
// enter card-emulation mode.
//
// Layout mirrors what the SeedHammer's nfc/type2 reader expects:
//   block 3: CC = E1 10 <size> 00   (size in 8-byte units)
//   blocks 4+: NDEF TLV: 03 [FF hi lo] <ndef> FE, padded to 4-byte
//   blocks.
// Anti-collision (REQA/ATQA, UID, SAK) is handled by the PN532's
// target-mode hardware; only READ commands reach this layer.

#include "tag_emu.h"

#include <string.h>

#define BLOCK_SIZE 4

// Virtual tag size in blocks. NTAG216 tops out at 888 user bytes; the
// CC advertises a little more so a long descriptor fits (977 payload
// bytes after the Text record and TLV overhead).
#define MAX_BLOCKS 252

static uint8_t vmem[MAX_BLOCKS * BLOCK_SIZE];
static size_t vmem_len;          // Bytes in use, a whole number of blocks.
static size_t tlv_end_block;     // Block holding the TLV terminator.
static bool delivered;

bool tag_emu_load(const uint8_t *ndef, size_t ndef_len) {
    // TLV header: 03 len for short messages, 03 FF hi lo above 254.
    size_t hdr = ndef_len < 255 ? 2 : 4;
    size_t tlv_bytes = hdr + ndef_len + 1;  // + terminator FE.
    size_t data_blocks = (tlv_bytes + BLOCK_SIZE - 1) / BLOCK_SIZE;
    // Blocks 4 .. 4+data_blocks-1 hold the TLV; the reader stops at
    // memBlocks = cc[2]*8/4 = cc[2]*2, which must reach the last block.
    size_t mem_blocks = 4 + data_blocks;
    if (mem_blocks > MAX_BLOCKS) {
        return false;
    }
    size_t cc_size = (mem_blocks + 1) / 2;  // 8-byte units, rounded up.
    size_t total = mem_blocks * BLOCK_SIZE;
    if (total > sizeof(vmem)) {
        return false;
    }

    memset(vmem, 0, total);
    // UID pages: block 0 carries the 4-byte UID the PN532 presents in
    // anti-collision (08 11 22 33), block 1 the lock/OTP area. Readers
    // that check the read-back UID against anti-collision need this.
    vmem[0] = 0x08; vmem[1] = 0x11; vmem[2] = 0x22; vmem[3] = 0x33;
    vmem[4] = 0x00; vmem[5] = 0x00; vmem[6] = 0x00;
    vmem[7] = (uint8_t)(0x08 ^ 0x11 ^ 0x22 ^ 0x33);  // BCC0.
    vmem[8] = 0x00; vmem[9] = 0x00; vmem[10] = 0x00;
    vmem[11] = (uint8_t)(0x08 ^ 0x11 ^ 0x22 ^ 0x33); // BCC1.
    // Capability container at block 3.
    vmem[12] = 0xe1;            // Magic.
    vmem[13] = 0x10;            // Version.
    vmem[14] = (uint8_t)cc_size;
    vmem[15] = 0x00;

    // NDEF TLV starting at block 4 (offset 16).
    size_t off = 16;
    vmem[off++] = 0x03;         // NDEF TLV type.
    if (ndef_len < 255) {
        vmem[off++] = (uint8_t)ndef_len;
    } else {
        vmem[off++] = 0xff;
        vmem[off++] = (uint8_t)(ndef_len >> 8);
        vmem[off++] = (uint8_t)(ndef_len & 0xff);
    }
    memcpy(&vmem[off], ndef, ndef_len);
    off += ndef_len;
    vmem[off++] = 0xfe;         // TLV terminator.

    vmem_len = total;
    tlv_end_block = 4 + data_blocks - 1;
    delivered = false;
    return true;
}

bool tag_emu_read(uint8_t block, uint8_t out[16]) {
    size_t off = (size_t)block * BLOCK_SIZE;
    if (off >= vmem_len) {
        // Read past the end (reader clamped to its own memBlocks):
        // serve zeros.
        memset(out, 0, 16);
        return true;
    }
    size_t n = vmem_len - off;
    if (n > 16) {
        n = 16;
    }
    memcpy(out, &vmem[off], n);
    if (n < 16) {
        memset(&out[n], 0, 16 - n);
    }
    if (block + 3 >= tlv_end_block) {
        delivered = true;
    }
    return true;
}

bool tag_emu_is_delivered(void) {
    return delivered;
}