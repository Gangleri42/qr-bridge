// pn532.c -- PN532 HSU/UART driver, target (card) mode only.
//
// Framing: 00 00 FF LEN LCS TFI BODY DCS 00, LCS=(256-LEN)&0xFF,
// DCS=(256-sum(BODY))&0xFF. ACK = 00 00 FF 00 FF 00. Errors carry a
// 0x7F error code.
//
// The bare module auto-sleeps between commands, so every command is
// prefixed with the datasheet wake pattern (55 55 + 14 zero bytes),
// except inside a target session where the 3 ms wake delay would
// overrun the initiator's transceive timeout.

#include "pn532.h"
#include "board.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "pn532";

// UART buffer size
#define PN532_UART_BUF_SIZE   1024

// Preamble + start code + LEN + LCS + TFI + DCS + postamble, without
// the body.
#define PN532_FRAME_OVERHEAD  7

// Max body length we accept. PN532 supports up to 255-byte bodies.
#define PN532_MAX_BODY        255

#define PN532_PREAMBLE        0x00
#define PN532_STARTCODE       0xff
#define PN532_POSTAMBLE       0x00

#define PN532_HOST_TO_PN532   0xd4
#define PN532_PN532_TO_HOST   0xd5

// Command codes.
#define PN532_CMD_GETFIRMWAREVERSION  0x02
#define PN532_CMD_SAMCONFIGURATION    0x14

// Target (card) mode: present the PN532 as a passive ISO 14443A card.
// PICC (card) mode command exchange: the correct pipe for answering a
// Type 2 READ from an initiator (TgGetData/TgSetData 0x86/0x8E are
// DEP-mode only).
#define PN532_CMD_TGINITASTARGET      0x8c
#define PN532_CMD_TGGETINITIATORCMD   0x88
#define PN532_CMD_TGRESPONSETOINIT    0x90

static bool pn532_rx(uint8_t *buf, size_t len, uint32_t timeout_ms) {
    uint32_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    size_t got = 0;
    while (got < len) {
        uint32_t now = xTaskGetTickCount();
        if (now >= deadline) {
            return false;
        }
        int c = -1;
        uint8_t byte;
        int r = uart_read_bytes(PN532_UART_PORT, &byte, 1, pdMS_TO_TICKS(1));
        if (r == 1) {
            c = byte;
        }
        if (c >= 0) {
            buf[got++] = (uint8_t)c;
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    return true;
}

// Read a normal response frame (00 00 FF LEN LCS D5 BODY DCS 00) into
// buf. Returns the body length, or -1 on failure. The ACK frame is
// consumed by pn532_command before this is called, but the chip can
// emit a second ACK (e.g. after a wake while a target session is
// active), so ACK frames encountered here are skipped and the next
// frame read.
static int pn532_read_response(uint8_t *buf, size_t max, uint32_t timeout_ms) {
    uint8_t hdr[5];
    uint8_t len;
    uint8_t lcs;
    for (;;) {
        if (!pn532_rx(hdr, 5, timeout_ms)) {
            return -1;
        }
        // Preamble, start code, LEN, LCS.
        if (hdr[0] != PN532_PREAMBLE || hdr[1] != PN532_PREAMBLE ||
            hdr[2] != PN532_STARTCODE) {
            ESP_LOGD(TAG, "bad preamble: %02x %02x %02x", hdr[0], hdr[1], hdr[2]);
            return -1;
        }
        len = hdr[3];
        lcs = hdr[4];
        if ((uint8_t)(len + lcs) != 0) {
            ESP_LOGD(TAG, "bad len/lcs: %02x %02x", len, lcs);
            return -1;
        }
        if (len == 0) {
            // ACK frame (00 00 FF 00 FF 00): consume the trailing byte
            // and look for the real response.
            uint8_t post;
            if (!pn532_rx(&post, 1, timeout_ms) || post != PN532_POSTAMBLE) {
                return -1;
            }
            continue;
        }
        break;
    }
    // Body (minus the TFI byte) + DCS + postamble.
    size_t body_len = len - 1;
    if (body_len > PN532_MAX_BODY) {
        return -1;
    }
    uint8_t tfi;
    if (!pn532_rx(&tfi, 1, timeout_ms) ||
        tfi != PN532_PN532_TO_HOST) {
        return -1;
    }
    // Always drain the full frame even if the caller's buffer is too
    // small; otherwise the leftover bytes corrupt the next read.
    uint8_t scratch[PN532_MAX_BODY];
    uint8_t *dest = scratch;
    size_t copy = body_len < max ? body_len : max;
    if (buf != NULL && copy > 0) {
        dest = buf;
    }
    if (!pn532_rx(dest, copy, timeout_ms)) {
        return -1;
    }
    if (copy < body_len) {
        if (!pn532_rx(scratch, body_len - copy, timeout_ms)) {
            return -1;
        }
    }
    uint8_t dcs;
    if (!pn532_rx(&dcs, 1, timeout_ms)) {
        return -1;
    }
    uint8_t sum = tfi;
    for (size_t i = 0; i < copy; i++) {
        sum += dest[i];
    }
    for (size_t i = 0; i < body_len - copy; i++) {
        sum += scratch[i];
    }
    if ((uint8_t)(sum + dcs) != 0) {
        ESP_LOGD(TAG, "checksum mismatch: sum=%02x dcs=%02x", sum, dcs);
        return -1;
    }
    uint8_t post;
    if (!pn532_rx(&post, 1, timeout_ms) || post != PN532_POSTAMBLE) {
        return -1;
    }
    return (int)body_len;
}

// Send wake pattern to revive PN532 from sleep
static void pn532_wake(void) {
    uint8_t wake[16] = {0x55, 0x55};
    uart_write_bytes(PN532_UART_PORT, (const char *)wake, sizeof(wake));
    uart_wait_tx_done(PN532_UART_PORT, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(3));
}

// Drain any stale bytes still sitting in the RX FIFO.
static void pn532_flush_rx(void) {
    uint8_t byte;
    while (uart_read_bytes(PN532_UART_PORT, &byte, 1, 0) > 0) {
        // discard
    }
}

// Send a command (TFI + body) and return the response body. The body
// includes the echoed command code as its first byte (PN532 convention).
// Returns body length or -1. response may be NULL to read-and-discard.
// When pn532_skip_wake is set, the wake burst is skipped: the target-
// mode serve loop answers each initiator command within the reader's
// short transceive timeout, and the 3 ms wake delay would blow it.
static bool pn532_skip_wake = false;

static int pn532_command(const uint8_t *body, size_t body_len,
                         uint8_t *response, size_t max,
                         uint32_t timeout_ms) {
    uint8_t frame[PN532_FRAME_OVERHEAD + 1 + PN532_MAX_BODY];
    size_t len = body_len + 1;  // + TFI.
    frame[0] = PN532_PREAMBLE;
    frame[1] = PN532_PREAMBLE;
    frame[2] = PN532_STARTCODE;
    frame[3] = (uint8_t)len;
    frame[4] = (uint8_t)(256 - len);
    frame[5] = PN532_HOST_TO_PN532;
    memcpy(&frame[6], body, body_len);
    uint8_t sum = PN532_HOST_TO_PN532;
    for (size_t i = 0; i < body_len; i++) {
        sum += body[i];
    }
    frame[6 + body_len] = (uint8_t)(256 - sum);
    frame[7 + body_len] = PN532_POSTAMBLE;

    // The chip auto-sleeps between commands; a 55 55 + 14 zero wake
    // prefix revives it, and it tolerates the wake when already awake.
    // Retry once: a stale error frame from the wake can steal the ACK
    // slot, so flush and try again.
    for (int attempt = 0; attempt < 2; attempt++) {
        pn532_flush_rx();
        if (!pn532_skip_wake) {
            pn532_wake();
        }
        uart_write_bytes(PN532_UART_PORT, (const char *)frame, 8 + body_len);
        uart_wait_tx_done(PN532_UART_PORT, pdMS_TO_TICKS(100));
        ESP_LOGD(TAG, "frame sent (attempt %d)", attempt + 1);

        // ACK is the 6-byte frame 00 00 FF 00 FF 00.
        uint8_t ack[6];
        if (!pn532_rx(ack, 6, timeout_ms)) {
            ESP_LOGW(TAG, "ack timeout");
            continue;
        }
        static const uint8_t ack_pat[6] = {0x00, 0x00, 0xff, 0x00, 0xff, 0x00};
        if (memcmp(ack, ack_pat, sizeof(ack_pat)) != 0) {
            ESP_LOGW(TAG, "bad ack %02x %02x %02x %02x %02x %02x",
                    ack[0], ack[1], ack[2], ack[3], ack[4], ack[5]);
            continue;
        }
        ESP_LOGD(TAG, "ack ok");
        // Stage the response first: the echoed command code is stripped
        // below, and shifting in place would overflow any caller whose
        // buffer is smaller than the response.
        uint8_t staged[PN532_MAX_BODY];
        int n = pn532_read_response(staged, sizeof(staged), timeout_ms);
        if (n < 1) {
            continue;
        }
        // The response body echoes the command code first (0x7F for
        // errors). Move the data bytes down one and return n-1.
        if (response != NULL && n > 1) {
            size_t keep = (size_t)(n - 1);
            if (keep > max) {
                keep = max;
            }
            memcpy(response, staged + 1, keep);
        }
        return n - 1;
    }
    return -1;
}

bool pn532_init(void) {
    // The module self-boots on power and RST is never driven. A
    // pulled-up input keeps it deasserted if the pin is wired.
    gpio_config_t rst_cfg = {
        .pin_bit_mask = (1ULL << PN532_RST_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&rst_cfg));

    uart_config_t uart_config = {
        .baud_rate = PN532_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(PN532_UART_PORT, PN532_UART_BUF_SIZE, PN532_UART_BUF_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(PN532_UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(PN532_UART_PORT, PN532_UART_TX_PIN, PN532_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "wake");
    pn532_wake();
    ESP_LOGI(TAG, "wake sent");

    // GetFirmwareVersion: sanity check the chip is alive.
    uint8_t body[] = {PN532_CMD_GETFIRMWAREVERSION};
    uint8_t resp[5];  // echoed cmd + IC, ver, rev, support.
    int n = pn532_command(body, sizeof(body), resp, sizeof(resp), 500);
    ESP_LOGI(TAG, "GetFirmwareVersion -> %d", n);
    if (n != 4) {
        return false;
    }
    ESP_LOGI(TAG, "PN532 firmware: IC=0x%02x Ver=%d.%d Support=0x%02x",
             resp[0], resp[1], resp[2], resp[3]);

    // SAMConfiguration: normal mode, timeout 0x14, IRQ on. This is the
    // whole target-mode setup. A timeout of 0 makes the chip reject
    // TgInitAsTarget, and so do nfcpy's extra steps (SetParameters,
    // RFConfiguration items 2/4, the CIU_Mode register write), which
    // are ACR122U-specific.
    uint8_t sam[] = {PN532_CMD_SAMCONFIGURATION, 0x01, 0x14, 0x01};
    uint8_t sam_resp[1];
    n = pn532_command(sam, sizeof(sam), sam_resp, sizeof(sam_resp), 500);
    ESP_LOGI(TAG, "SAMConfig -> %d", n);
    if (n < 0) {
        return false;
    }

    // RFConfiguration item 5 (finite passive-activation retries) is
    // deliberately not sent: this chip rejects TgInitAsTarget after it.
    // Only initiator-side polling needs it, and the bridge never polls.

    return true;
}

// Present the PN532 as a passive ISO 14443A card (Type A, 106 kbps).
// nfca is the 6-byte MIFARE target parameter block: SENS_RES(2),
// NFCID1(3, the low three UID bytes), SEL_RES(1). The command blocks
// until an initiator activates the card; the initiator's first command
// (typically a Type 2 READ of the capability container) is returned in
// first_cmd. Returns the number of command bytes, or -1 on timeout.
int pn532_tg_init_as_target(const uint8_t *nfca, const uint8_t *nfcid3t,
                            uint8_t *first_cmd, size_t max,
                            uint32_t timeout_ms) {
    // Mode 0x01: passive Type A card only. Body layout: cmd + mode +
    // MIFARE params(6) + FeliCa params(18) + NFCID3t(10) + general-bytes
    // length + historical-bytes length = 38 bytes.
    uint8_t body[1 + 1 + 6 + 18 + 10 + 1 + 1];
    body[0] = PN532_CMD_TGINITASTARGET;
    body[1] = 0x01;
    memcpy(&body[2], nfca, 6);
    memset(&body[8], 0, 18);        // FeliCa params unused for Type A.
    memcpy(&body[26], nfcid3t, 10);
    body[36] = 0;                   // No general bytes.
    body[37] = 0;                   // No historical bytes.
    uint8_t rsp[64];
    int n = pn532_command(body, sizeof(body), rsp, sizeof(rsp), timeout_ms);
    if (n < 1) {
        return -1;
    }
    // rsp[0] is the bit-rate/status byte; the initiator's first command
    // (if any) follows it.
    size_t cmd = (size_t)(n - 1);
    if (first_cmd != NULL && cmd > 0) {
        if (cmd > max) {
            cmd = max;
        }
        memcpy(first_cmd, &rsp[1], cmd);
    }
    return (int)cmd;
}

// Fetch the next command the initiator sent in PICC (card) mode via
// TgGetInitiatorCommand (0x88). Returns the number of command bytes
// (0 = no data yet), or -1 on error.
int pn532_tg_get_data(uint8_t *data, size_t max, uint32_t timeout_ms) {
    uint8_t body[] = {PN532_CMD_TGGETINITIATORCMD};
    uint8_t rsp[64];
    bool old = pn532_skip_wake;
    pn532_skip_wake = true;
    int n = pn532_command(body, sizeof(body), rsp, sizeof(rsp), timeout_ms);
    pn532_skip_wake = old;
    if (n < 1 || rsp[0] != 0) {
        ESP_LOGD(TAG, "tg get: status=%02x n=%d",
                 n >= 1 ? rsp[0] : 0xff, n);
        return -1;
    }
    size_t cmd = (size_t)(n - 1);
    if (data != NULL && cmd > 0) {
        if (cmd > max) {
            return -1;
        }
        memcpy(data, &rsp[1], cmd);
    }
    return (int)cmd;
}

// Send a response frame to the initiator in PICC (card) mode via
// TgResponseToInitiator (0x90).
int pn532_tg_set_data(const uint8_t *data, size_t len, uint32_t timeout_ms) {
    uint8_t body[1 + PN532_MAX_BODY];
    if (len > sizeof(body) - 1) {
        return -1;
    }
    body[0] = PN532_CMD_TGRESPONSETOINIT;
    memcpy(&body[1], data, len);
    uint8_t rsp[4];
    bool old = pn532_skip_wake;
    pn532_skip_wake = true;
    int n = pn532_command(body, len + 1, rsp, sizeof(rsp), timeout_ms);
    pn532_skip_wake = old;
    if (n < 1 || rsp[0] != 0) {
        ESP_LOGD(TAG, "tg set: status=%02x n=%d",
                 n >= 1 ? rsp[0] : 0xff, n);
        return -1;
    }
    return 0;
}