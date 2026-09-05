#ifndef QR_BRIDGE_SCANNER_H
#define QR_BRIDGE_SCANNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

// Longest code the scanner may type before Enter. A longer one is
// discarded whole.
#define SCANNER_MAX_TEXT 4096

// One decoded code: the characters the scanner typed before Enter.
typedef struct {
    size_t len;
    uint8_t text[SCANNER_MAX_TEXT];
} scan_t;

// Install the USB host stack and the HID host driver and start the
// tasks that service them. Every boot-protocol keyboard that
// enumerates (the barcode scanner) is opened; its keystrokes
// accumulate until Enter completes a scan.
void scanner_init(void);

// Wait up to `ticks` for a completed scan and copy it to `out`. Only
// the latest scan is kept: a code scanned before the previous one was
// taken replaces it. Returns false if none arrived in time.
bool scanner_take(scan_t *out, TickType_t ticks);

#ifdef __cplusplus
}
#endif

#endif
