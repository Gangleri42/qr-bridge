#ifndef QR_BRIDGE_SCANNER_H
#define QR_BRIDGE_SCANNER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Install the USB host stack and the HID host driver and start the
// task that services them. Keystrokes from a HID keyboard (the
// scanner) accumulate until Enter.
void scanner_init(void);

// Called on the HID host task with the complete payload when Enter
// arrives. Implemented by main.
void scanner_on_payload(const uint8_t *payload, size_t len);

// Drop the partial payload and the key state (after a delivery).
void scanner_clear_payload(void);

#ifdef __cplusplus
}
#endif

#endif
