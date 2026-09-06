# qr-bridge

A USB barcode scanner on one side, an emulated NFC tag on the other.

Scan a QR code (a wallet descriptor, BIP-39 words, a text note) with the
scanner. The bridge holds the decoded text in RAM and presents it to the
SeedHammer as an NTAG-shaped NFC Forum Type 2 tag carrying one NDEF Text
record. A scan too long for one tag goes out as a series of tags
carrying BBQr parts, which the SeedHammer joins. Hold the SeedHammer's
reader against the PN532 antenna the way you would tap a sticker, and
the SeedHammer takes it from there. No screen, no storage, no radio
silicon.

## Hardware

| Part | Role |
| --- | --- |
| Waveshare ESP32-P4-Pico | MCU. USB host on the MX1.25 header, UART console on the USB-C connector, no radio on the die. | 11$
| 2D barcode scanner | Any scanner that enumerates as a USB HID keyboard and sends Enter after the code will do. Plugs into the MX1.25 USB OTG header. | 15$
| PN532 breakout | Mode switches set to HSU. Wired to UART1. | 2$

PN532 wiring, from `components/board/board.h`:

| PN532 | ESP32-P4 |
| --- | --- |
| TXD | GPIO18 |
| RXD | GPIO17 |
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

The build is reproducible: the same commit on the same ESP-IDF release
gives a byte-identical `qr-bridge.bin`, with no compile date and no
build paths inside.

## What it does

1. Boot: power the bridge via USB-C
2. Scan: the scanner types the QR content as keystrokes and finishes
   with Enter. The scanner's own beep is the "armed" signal. Scanning
   again replaces the payload.
3. Tap: while a payload is armed the PN532 sits in target mode as a
   Type 2 tag. The SeedHammer reads it block by block; once it has read
   the block holding the last message byte the payload counts as
   delivered and is cleared. The SeedHammer's preview screen is the
   confirmation.
4. A long scan is a series. As soon as the SeedHammer has read one
   part the bridge presents the next, round-robin, and the SeedHammer
   counts PART n OF m on its screen; keep holding until the preview
   appears, a second or two per part. The series is cleared once every
   part has been read and the reader has stayed away for 3 s. Pulled
   away early, the bridge keeps the series armed and the next tap
   resumes it.

Limits:

- 977 bytes of scanned text per tag. The virtual tag is 252 blocks, a
  little above an NTAG216, and the SeedHammer's reader addresses no
  more. Longer scans go as BBQr parts of 605 bytes each: a 2.8 KB
  descriptor takes five tags, a second or two each under the reader.
  The SeedHammer joins series of up to 16 KB.
- Keyboard-mode scanners carry printable ASCII only, and the keymap is
  the US layout. Scans over 8192 characters are discarded whole; the
  largest QR code holds 4296 alphanumeric characters, or 7089 digits.
- The payload is one text record, joined from its parts first when it
  came as a series. The SeedHammer decides what it is: seed words, a
  SeedQR in its digit form, a descriptor, codex32, nip19, or a plain
  text plate.

## Console

UART0 logs state changes and byte counts: scanner connected, scan of N
bytes, armed, reader activated, part 2 of 5 read, delivered. The
scanned text itself is never logged.

## Tests

The three pure components, the Text record, the virtual tag and the
BBQr splitter, build on the host with the checks that pin their byte
layouts; the splitter is checked byte for byte against parts made by
the SeedHammer's own:

```sh
make -C test/host run
```

CI runs them alongside the firmware build. Everything else needs the
hardware.

## Layout

- `main/` arms the payload and serves the tag.
- `components/board/` pin map.
- `components/scanner/` USB host and HID keyboard parser.
- `components/pn532/` PN532 HSU driver, target mode only.
- `components/tag_emu/` the virtual Type 2 tag: CC, NDEF TLV, block reads.
- `components/text_record/` one NDEF Text record.
- `components/bbqr/` BBQr parts for a scan that does not fit one tag.
- `test/host/` host build and tests for the three components above.

## License

Unlicense, see LICENSE.
