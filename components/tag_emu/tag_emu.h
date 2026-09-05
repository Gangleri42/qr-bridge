#ifndef QR_BRIDGE_TAG_EMU_H
#define QR_BRIDGE_TAG_EMU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Load an NDEF message (no NLEN, no TLV) into the virtual Type 2 tag
// the bridge presents to the SeedHammer's reader. Builds the CC and the
// NDEF TLV, pads to whole 4-byte blocks. Returns false if the message
// does not fit the virtual tag.
bool tag_emu_load(const uint8_t *ndef, size_t ndef_len);

// Serve one Type 2 READ command for `block` (block 3 = CC, block 4+
// = NDEF TLV). Fills out[16] with the four blocks.
bool tag_emu_read(uint8_t block, uint8_t out[16]);

// True once a READ has covered the block holding the TLV terminator,
// i.e. the initiator consumed the whole NDEF message.
bool tag_emu_is_delivered(void);

#ifdef __cplusplus
}
#endif

#endif