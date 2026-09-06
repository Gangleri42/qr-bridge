// pn532.c -- PN532 HSU/UART driver, target (card) mode only.
//
// Framing: 00 00 FF LEN LCS TFI BODY DCS 00, LCS=(256-LEN)&0xFF,
// DCS=(256-sum(TFI,BODY))&0xFF. ACK = 00 00 FF 00 FF 00. Errors carry
// a 0x7F error code.
//
// The bare module auto-sleeps between commands, so every command is
// prefixed with the datasheet wake pattern (55 55 + 14 zero bytes),
// except inside a target session where the 3 ms wake delay would
// overrun the initiator's transceive timeout.
//
// A response the host stops waiting for leaves the chip busy with the
// command, so giving up means aborting it: the host abort of the
// datasheet is the ACK frame. TgInitAsTarget is the command the host
// waits on as long as it likes, since it completes only once an
// initiator activates the tag; only a new payload makes it abort.

#include "pn532.h"
#include "board.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pn532";

#define UART_BUF_SIZE 1024

// 00 00 FF LEN LCS TFI ... DCS 00: the frame around the body.
#define FRAME_OVERHEAD 8
// LEN counts TFI plus body and is one byte, so the body is at most 254.
#define MAX_BODY 254
// Once a frame has begun its bytes follow back to back; the longest
// takes 23 ms at 115200 baud.
#define FRAME_MS 100

#define PREAMBLE 0x00
#define STARTCODE 0xff
#define POSTAMBLE 0x00
#define HOST_TO_PN532 0xd4
#define PN532_TO_HOST 0xd5

#define CMD_GETFIRMWAREVERSION 0x02
#define CMD_SAMCONFIGURATION 0x14
// PICC (card) mode. TgGetData/TgSetData (0x86/0x8E) are DEP-only; a
// Type 2 READ from an initiator comes through these instead.
#define CMD_TGINITASTARGET 0x8c
#define CMD_TGGETINITIATORCOMMAND 0x88
#define CMD_TGRESPONSETOINITIATOR 0x90

static const uint8_t ack_frame[6] = {0x00, 0x00, 0xff, 0x00, 0xff, 0x00};

// Read exactly len bytes, or fail once timeout_ms have passed.
static bool rx(uint8_t *buf, size_t len, uint32_t timeout_ms) {
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    size_t got = 0;
    while (got < len) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) {
            return false;
        }
        int n = uart_read_bytes(PN532_UART_PORT, buf + got, len - got, deadline - now);
        if (n < 0) {
            return false;
        }
        got += (size_t)n;
    }
    return true;
}

// Read one response frame into buf. Returns the body length without
// the TFI, PN532_TIMEOUT if no frame began within timeout_ms (nothing
// consumed, so the wait can go on), or PN532_ERROR for a malformed
// frame or the chip's own error frame. A frame that began is consumed
// whole, even one that exceeds max or is rejected, or the leftover
// bytes would corrupt the next read. Stray ACK frames (the chip can
// emit a second one after a wake during a target session) are skipped;
// they carry LEN/LCS 00 FF, which no other frame does, and are told
// apart before the checksum rule they do not follow.
static int read_response(uint8_t *buf, size_t max, uint32_t timeout_ms) {
    uint8_t hdr[5];
    for (;;) {
        if (!rx(hdr, 1, timeout_ms)) {
            return PN532_TIMEOUT;
        }
        if (!rx(&hdr[1], sizeof(hdr) - 1, FRAME_MS)) {
            return PN532_ERROR;
        }
        if (hdr[0] != PREAMBLE || hdr[1] != PREAMBLE || hdr[2] != STARTCODE) {
            ESP_LOGD(TAG, "bad preamble %02x %02x %02x", hdr[0], hdr[1], hdr[2]);
            return PN532_ERROR;
        }
        if (hdr[3] != ack_frame[3] || hdr[4] != ack_frame[4]) {
            break;
        }
        uint8_t post;  // An ACK: consume its postamble and read on.
        if (!rx(&post, 1, FRAME_MS) || post != POSTAMBLE) {
            return PN532_ERROR;
        }
        ESP_LOGD(TAG, "stray ACK skipped");
    }
    if (hdr[3] == 0 || (uint8_t)(hdr[3] + hdr[4]) != 0) {
        ESP_LOGD(TAG, "bad LEN/LCS %02x %02x", hdr[3], hdr[4]);
        return PN532_ERROR;
    }
    size_t body_len = hdr[3] - 1;
    size_t copy = body_len < max ? body_len : max;
    uint8_t scratch[MAX_BODY];
    uint8_t tfi, dcs, post;
    if (!rx(&tfi, 1, FRAME_MS) || !rx(buf, copy, FRAME_MS) ||
        !rx(scratch, body_len - copy, FRAME_MS) || !rx(&dcs, 1, FRAME_MS) ||
        !rx(&post, 1, FRAME_MS)) {
        return PN532_ERROR;
    }
    if (tfi != PN532_TO_HOST) {
        // 0x7f is the chip's error frame: it rejected the command.
        ESP_LOGD(TAG, "no response, TFI %02x", tfi);
        return PN532_ERROR;
    }
    uint8_t sum = tfi;
    for (size_t i = 0; i < copy; i++) {
        sum += buf[i];
    }
    for (size_t i = 0; i < body_len - copy; i++) {
        sum += scratch[i];
    }
    if ((uint8_t)(sum + dcs) != 0 || post != POSTAMBLE) {
        ESP_LOGD(TAG, "bad checksum or postamble");
        return PN532_ERROR;
    }
    return (int)body_len;
}

static void send_wake(void) {
    static const uint8_t wake[16] = {0x55, 0x55};
    uart_write_bytes(PN532_UART_PORT, wake, sizeof(wake));
    uart_wait_tx_done(PN532_UART_PORT, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(3));
}

static void flush_rx(void) {
    uart_flush_input(PN532_UART_PORT);
}

bool pn532_rx_pending(void) {
    size_t n = 0;
    return uart_get_buffered_data_len(PN532_UART_PORT, &n) == ESP_OK && n > 0;
}

void pn532_abort(void) {
    uart_write_bytes(PN532_UART_PORT, ack_frame, sizeof(ack_frame));
    uart_wait_tx_done(PN532_UART_PORT, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(2));
    flush_rx();
}

// Send a command body (without TFI) and wait up to timeout_ms for its
// ACK. A second attempt follows a missing or malformed ACK, because a
// stale error frame left by the wake can take the ACK's slot. Returns
// false, with the chip aborted in case it took the command after all,
// when both attempts fail.
static bool send(const uint8_t *body, size_t body_len, bool wake, uint32_t timeout_ms) {
    if (body_len > MAX_BODY) {
        return false;
    }
    uint8_t frame[FRAME_OVERHEAD + MAX_BODY];
    size_t len = body_len + 1;  // TFI + body.
    frame[0] = PREAMBLE;
    frame[1] = PREAMBLE;
    frame[2] = STARTCODE;
    frame[3] = (uint8_t)len;
    frame[4] = (uint8_t)(256 - len);
    frame[5] = HOST_TO_PN532;
    memcpy(&frame[6], body, body_len);
    uint8_t sum = HOST_TO_PN532;
    for (size_t i = 0; i < body_len; i++) {
        sum += body[i];
    }
    frame[6 + body_len] = (uint8_t)(256 - sum);
    frame[7 + body_len] = POSTAMBLE;

    for (int attempt = 0; attempt < 2; attempt++) {
        flush_rx();
        if (wake) {
            send_wake();
        }
        uart_write_bytes(PN532_UART_PORT, frame, FRAME_OVERHEAD + body_len);
        uart_wait_tx_done(PN532_UART_PORT, pdMS_TO_TICKS(100));

        uint8_t ack[sizeof(ack_frame)];
        if (!rx(ack, sizeof(ack), timeout_ms)) {
            ESP_LOGW(TAG, "no ACK for command %02x", body[0]);
            continue;
        }
        if (memcmp(ack, ack_frame, sizeof(ack)) != 0) {
            ESP_LOGW(TAG, "bad ACK for command %02x: %02x %02x %02x %02x %02x %02x",
                     body[0], ack[0], ack[1], ack[2], ack[3], ack[4], ack[5]);
            continue;
        }
        return true;
    }
    pn532_abort();
    return false;
}

// Read the response to the command last sent: its body without the
// echoed command code. Up to max bytes are copied to response; the
// return value is the full length, or PN532_TIMEOUT or PN532_ERROR,
// which is also what the chip's own error frame becomes.
static int receive(uint8_t *response, size_t max, uint32_t timeout_ms) {
    uint8_t staged[MAX_BODY];
    int n = read_response(staged, sizeof(staged), timeout_ms);
    if (n < 0) {
        return n;
    }
    if (n < 1) {
        return PN532_ERROR;
    }
    size_t data = (size_t)(n - 1);
    if (response != NULL) {
        memcpy(response, staged + 1, data < max ? data : max);
    }
    return (int)data;
}

// Send a command and return its response, as send and receive do. A
// timeout or a malformed answer aborts the command, since the chip may
// still be running it, so it is idle for the next one.
static int command(const uint8_t *body, size_t body_len, bool wake,
                   uint8_t *response, size_t max, uint32_t timeout_ms) {
    if (!send(body, body_len, wake, timeout_ms)) {
        return PN532_ERROR;
    }
    int n = receive(response, max, timeout_ms);
    if (n < 0) {
        pn532_abort();
    }
    return n;
}

void pn532_init(void) {
    // The module self-boots on power and RST is never driven. A
    // pulled-up input keeps it deasserted if the pin is wired.
    gpio_config_t rst = {
        .pin_bit_mask = 1ULL << PN532_RST_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&rst));

    uart_config_t uart = {
        .baud_rate = PN532_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(PN532_UART_PORT, UART_BUF_SIZE, UART_BUF_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(PN532_UART_PORT, &uart));
    ESP_ERROR_CHECK(uart_set_pin(PN532_UART_PORT, PN532_UART_TX_PIN, PN532_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

bool pn532_start(void) {
    // RST is undriven, so the chip keeps its state across a reset of
    // this board and may still be listening for an initiator.
    pn532_abort();

    uint8_t get_version[] = {CMD_GETFIRMWAREVERSION};
    uint8_t version[4];  // IC, Ver, Rev, Support.
    if (command(get_version, sizeof(get_version), true, version, sizeof(version), 500) != 4) {
        return false;
    }
    ESP_LOGI(TAG, "PN5%02x firmware %u.%u", version[0], version[1], version[2]);

    // SAMConfiguration: normal mode, timeout 0x14, IRQ on. This is the
    // whole target-mode setup. A timeout of 0 makes the chip reject
    // TgInitAsTarget, and so do nfcpy's extra steps (SetParameters,
    // RFConfiguration items 2/4, the CIU_Mode register write), which
    // are ACR122U-specific. RFConfiguration item 5 (finite
    // passive-activation retries) has the same effect and is only
    // needed for initiator-side polling, which the bridge never does.
    uint8_t sam[] = {CMD_SAMCONFIGURATION, 0x01, 0x14, 0x01};
    return command(sam, sizeof(sam), true, NULL, 0, 500) >= 0;
}

// Copy the initiator's command out of a target-mode response of n
// bytes, of which at most rsp_max landed in rsp: everything after the
// leading status or mode byte. Returns its length, or PN532_ERROR when
// it does not fit max.
static int take_command(const uint8_t *rsp, size_t rsp_max, int n, uint8_t *cmd, size_t max) {
    size_t got = (size_t)n < rsp_max ? (size_t)n : rsp_max;
    size_t len = got - 1;
    if (len > max) {
        return PN532_ERROR;
    }
    memcpy(cmd, &rsp[1], len);
    return (int)len;
}

bool pn532_tg_listen(const uint8_t *nfca, const uint8_t *nfcid3t) {
    // Mode 0x01: passive Type A card only. Body layout: cmd + mode +
    // MIFARE params(6) + FeliCa params(18) + NFCID3t(10) + general-bytes
    // length + historical-bytes length = 38 bytes.
    uint8_t body[38] = {CMD_TGINITASTARGET, 0x01};
    memcpy(&body[2], nfca, 6);
    memcpy(&body[26], nfcid3t, 10);
    return send(body, sizeof(body), true, 200);
}

int pn532_tg_wait_initiator(uint8_t *first_cmd, size_t max, uint32_t timeout_ms) {
    uint8_t rsp[64];
    int n = receive(rsp, sizeof(rsp), timeout_ms);
    if (n == PN532_TIMEOUT) {
        return PN532_TIMEOUT;
    }
    if (n < 1) {
        pn532_abort();
        return PN532_ERROR;
    }
    // rsp[0] is the mode byte; the initiator's first command follows.
    return take_command(rsp, sizeof(rsp), n, first_cmd, max);
}

int pn532_tg_get_command(uint8_t *cmd, size_t max, uint32_t timeout_ms) {
    uint8_t body[] = {CMD_TGGETINITIATORCOMMAND};
    uint8_t rsp[64];
    int n = command(body, sizeof(body), false, rsp, sizeof(rsp), timeout_ms);
    if (n < 1 || rsp[0] != 0) {
        ESP_LOGD(TAG, "TgGetInitiatorCommand: status %02x, n=%d", n >= 1 ? rsp[0] : 0xff, n);
        return PN532_ERROR;
    }
    return take_command(rsp, sizeof(rsp), n, cmd, max);
}

int pn532_tg_respond(const uint8_t *data, size_t len, uint32_t timeout_ms) {
    uint8_t body[1 + MAX_BODY];
    if (len > MAX_BODY - 1) {
        return PN532_ERROR;
    }
    body[0] = CMD_TGRESPONSETOINITIATOR;
    memcpy(&body[1], data, len);
    uint8_t status;
    int n = command(body, len + 1, false, &status, 1, timeout_ms);
    if (n < 1 || status != 0) {
        ESP_LOGD(TAG, "TgResponseToInitiator: status %02x, n=%d", n >= 1 ? status : 0xff, n);
        return PN532_ERROR;
    }
    return 0;
}
