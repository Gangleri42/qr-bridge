#ifndef QR_BRIDGE_PN532_H
#define QR_BRIDGE_PN532_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Outcomes below zero.
#define PN532_TIMEOUT (-1)  // No response within the wait.
#define PN532_ERROR (-2)    // The chip reported an error, or sent a malformed frame.

// Set up UART1 to the PN532 (HSU mode). Call once.
void pn532_init(void);

// Wake the chip, read its firmware version and run SAMConfiguration,
// which is the whole target-mode setup. Returns false if the chip does
// not answer; safe to call again.
bool pn532_start(void);

// Target (card) mode: present the PN532 as a passive ISO 14443A tag
// so an external reader can read the emulated content. nfca is the
// 6-byte MIFARE parameter block (SENS_RES, NFCID1 low three bytes,
// SEL_RES); nfcid3t is 10 bytes (zeros are fine for Type A). The chip
// then waits for an initiator: poll with pn532_tg_wait_initiator, or
// leave with pn532_abort. Returns false if the chip did not take the
// command.
bool pn532_tg_listen(const uint8_t *nfca, const uint8_t *nfcid3t);

// Wait up to timeout_ms for an initiator to activate the tag, then
// copy its first command (typically a Type 2 READ) to first_cmd and
// return its length. PN532_TIMEOUT means nobody came yet and the chip
// is still listening: call again, or pn532_abort to stop. PN532_ERROR
// means the chip gave up; listen again.
int pn532_tg_wait_initiator(uint8_t *first_cmd, size_t max, uint32_t timeout_ms);

// Target mode: fetch the next command the initiator sent
// (TgGetInitiatorCommand). Returns its length, or a negative value
// when the initiator is gone; the chip is idle again either way.
int pn532_tg_get_command(uint8_t *cmd, size_t max, uint32_t timeout_ms);

// Target mode: answer the initiator's last command
// (TgResponseToInitiator). Returns 0, or a negative value when the
// initiator is gone; the chip is idle again either way.
int pn532_tg_respond(const uint8_t *data, size_t len, uint32_t timeout_ms);

// Abort the command in progress, if any, so the chip is idle for the
// next one.
void pn532_abort(void);

#ifdef __cplusplus
}
#endif

#endif
