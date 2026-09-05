# qr-bridge

A USB barcode scanner on one side, an emulated NFC tag on the other.

Scan a QR code (a wallet descriptor, BIP-39 words, a text note) with the
scanner. The bridge holds the decoded text in RAM and presents it to the
SeedHammer as an NTAG-shaped NFC Forum Type 2 tag carrying one NDEF Text
record. Hold the SeedHammer's reader against the PN532 antenna the way
you would tap a sticker, and the SeedHammer takes it from there. No
screen, no storage, no radio silicon.

## Hardware

| Part | Role |
| --- | --- |
| Waveshare ESP32-P4-Pico | MCU. USB host on the MX1.25 header, UART console on the USB-C connector, no radio on the die. |
| NT-1228BL 2D barcode scanner | Any scanner that enumerates as a USB HID keyboard and sends Enter after the code will do. Plugs into the MX1.25 USB OTG header. |
| PN532 breakout | Mode switches set to HSU. Wired to UART1. |

PN532 wiring, from `components/board/board.h`:

| PN532 | ESP32-P4 |
| --- | --- |
| TXD | GPIO18 |
| RXD | GPIO17 |
| RSTPDN | GPIO19, configured as a pulled-up input and never driven |
| VCC, GND | 3V3, GND |

Power the board over USB-C. The same connector carries the console at
115200 baud.

## Build and flash

ESP-IDF 5.4.4 with the esp32p4 target installed. The USB HID host class
comes from Espressif's component registry; `dependencies.lock` pins its
version.

```sh
. $IDF_PATH/export.sh
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

The port is the CH343P behind the USB-C connector (`/dev/ttyUSB0` on
older kernels).

## What it does

1. Boot: the USB host stack comes up and the PN532 answers
   GetFirmwareVersion and SAMConfiguration. If the PN532 is silent the
   NFC task exits and the console says so; the board has no other way
   to show it.
2. Scan: the scanner types the QR content as keystrokes and finishes
   with Enter. The scanner's own beep is the "armed" signal. Scanning
   again replaces the payload.
3. Tap: while a payload is armed the PN532 sits in target mode as a
   Type 2 tag. The SeedHammer reads it block by block; once it has read
   the block holding the NDEF terminator the payload counts as
   delivered and is cleared. The SeedHammer's preview screen is the
   confirmation.

Limits:

- 977 bytes of scanned text. The virtual tag is 252 blocks, a little
  above an NTAG216, and a long multisig descriptor fits.
- Keyboard-mode scanners carry printable ASCII only, and the keymap is
  the US layout.
- The payload is one text record. The SeedHammer decides what it is:
  seed words, a descriptor, codex32, nip19, or a plain text plate.

## Console

UART0 is a debug port. At the current log level it echoes every HID
report, which is every keystroke of the scanned code. Keep it
unattached while scanning secrets. Turning that down is on the review
list before the first release.

## Layout

- `main/` arms the payload and serves the tag.
- `components/board/` pin map.
- `components/scanner/` USB host and HID keyboard parser.
- `components/pn532/` PN532 HSU driver, target mode only.
- `components/tag_emu/` the virtual Type 2 tag: CC, NDEF TLV, block reads.
- `components/text_record/` one NDEF Text record.

## License

Unlicense, see LICENSE.
