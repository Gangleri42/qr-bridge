// board.h -- pin map for the Waveshare ESP32-P4-Pico.
//
// ESP32-P4NRW32: RISC-V dual-core, 32 MB PSRAM, 32 MB flash, no radio
// silicon. USB OTG on the 4-pin MX1.25 header (VBUS, D-, D+, GND);
// UART0 console through the CH343P on the USB-C connector at 115200.

#ifndef QR_BRIDGE_BOARD_H
#define QR_BRIDGE_BOARD_H

// PN532 breakout in HSU (UART) mode on UART1, via the 2x20 header.
#define PN532_UART_PORT       UART_NUM_1
#define PN532_UART_BAUD       115200
#define PN532_UART_TX_PIN     17   // ESP32 TX -> PN532 RXD
#define PN532_UART_RX_PIN     18   // ESP32 RX <- PN532 TXD
#define PN532_RST_PIN         19   // Active-low reset, left deasserted

#endif
