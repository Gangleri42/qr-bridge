#ifndef QR_BRIDGE_PN532_H
#define QR_BRIDGE_PN532_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Set up UART1 to the PN532 (HSU mode). Call once.
void pn532_init(void);

// Wake the chip, read its firmware version and run SAMConfiguration,
// which is the whole target-mode setup. Returns false if the chip does
// not answer; safe to call again.
bool pn532_start(void);

// Target (card) mode: present the PN532 as a passive ISO 14443A tag
// so an external reader can read the emulated content. nfca is the
// 6-byte MIFARE parameter block (SENS_RES, NFCID1 low three bytes,
// SEL_RES); nfcid3t is 10 bytes (zeros are fine for Type A). Blocks
// until an initiator activates the card, then copies the initiator's
// first command (typically a Type 2 READ) to first_cmd. Returns its
// length, or -1 when no initiator showed up within timeout_ms.
int pn532_tg_init_as_target(const uint8_t *nfca, const uint8_t *nfcid3t,
                            uint8_t *first_cmd, size_t max,
                            uint32_t timeout_ms);

// Target mode: fetch the next command the initiator sent
// (TgGetInitiatorCommand). Returns its length, or -1 on error or when
// the initiator is gone.
int pn532_tg_get_command(uint8_t *cmd, size_t max, uint32_t timeout_ms);

// Target mode: answer the initiator's last command
// (TgResponseToInitiator). Returns 0, or -1 on error.
int pn532_tg_respond(const uint8_t *data, size_t len, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
