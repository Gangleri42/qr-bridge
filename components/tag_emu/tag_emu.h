#ifndef QR_BRIDGE_TAG_EMU_H
#define QR_BRIDGE_TAG_EMU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Size of the virtual tag in 4-byte blocks. NTAG216 tops out at 888
// user bytes; this is a little more so a long descriptor fits.
#define TAG_EMU_BLOCKS 252

// Longest NDEF message the tag holds: the data blocks minus the
// three-byte-length TLV header and the terminator.
#define TAG_EMU_MAX_NDEF ((TAG_EMU_BLOCKS - 4) * 4 - 5)

// The 6-byte anti-collision parameter block the PN532 needs to present
// this tag: SENS_RES, the low three UID bytes, SEL_RES.
void tag_emu_nfca(uint8_t out[6]);

// Load an NDEF message (no NLEN, no TLV) into the virtual tag. Builds
// the capability container and the NDEF TLV and pads to whole blocks.
// Returns false, leaving the tag as it was, if the message does not
// fit.
bool tag_emu_load(const uint8_t *ndef, size_t ndef_len);

// Serve one Type 2 READ: the four blocks from `block` on, zeros past
// the end of the tag.
void tag_emu_read(uint8_t block, uint8_t out[16]);

// True once a data-area READ has covered the block holding the last
// message byte, which is when a sequential reader has the whole
// message.
bool tag_emu_delivered(void);

#ifdef __cplusplus
}
#endif

#endif
