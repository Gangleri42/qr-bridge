#ifndef QR_BRIDGE_PN532_H
#define QR_BRIDGE_PN532_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Init UART1 to the PN532 (HSU), wake it, check the firmware version
// and run SAMConfiguration, which is the whole target-mode setup.
// Returns false if the chip does not answer.
bool pn532_init(void);

// Target (card) mode: present the PN532 as a passive ISO 14443A tag
// so an external reader can read the emulated NDEF content. nfca is
// the 6-byte MIFARE parameter block (SENS_RES, NFCID1 low three bytes,
// SEL_RES); nfcid3t is 10 bytes (zeros are fine for Type A). Blocks
// until an initiator activates the card, then returns the initiator's
// first command (typically a Type 2 READ). Returns command bytes or -1.
int pn532_tg_init_as_target(const uint8_t *nfca, const uint8_t *nfcid3t,
                            uint8_t *first_cmd, size_t max,
                            uint32_t timeout_ms);

// Target mode: fetch the next command the initiator sent. Returns the
// number of command bytes, 0 when none arrived yet, or -1 on error.
int pn532_tg_get_data(uint8_t *data, size_t max, uint32_t timeout_ms);

// Target mode: send a response frame to the initiator.
int pn532_tg_set_data(const uint8_t *data, size_t len, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
