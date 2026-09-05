// main.c -- scan a QR code with the USB scanner, hold it in RAM and
// present it to the SeedHammer as an NFC Forum Type 2 tag.

#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pn532.h"
#include "scanner.h"
#include "tag_emu.h"
#include "text_record.h"

static const char *TAG = "main";

// Longest scan the virtual tag can carry as a text record.
#define MAX_TEXT (TAG_EMU_MAX_NDEF - TEXT_RECORD_OVERHEAD)

// How long to wait for a reader between looks at the scanner.
#define LISTEN_SLICE_MS 200
// The reader has the whole message once the last block is read, but
// its parser may fetch one more block for the terminator; it does so
// within a few milliseconds, and answering it lets the reader end the
// record cleanly instead of timing out.
#define STRAGGLER_MS 50

// Serve one Type 2 command from the reader. READ (0x30) gets the four
// blocks from the requested one; anything else is ignored and the
// session continues.
static bool serve_command(const uint8_t *cmd, size_t len) {
    if (len >= 2 && cmd[0] == 0x30) {
        uint8_t blocks[16];
        tag_emu_read(cmd[1], blocks);
        return pn532_tg_respond(blocks, sizeof(blocks), 300) == 0;
    }
    return true;
}

// Serve the reader that activated the tag, from its first command
// until it has the whole message or walks away. Returns true once the
// whole message was read.
static bool serve_session(uint8_t *cmd, size_t max, size_t first) {
    if (!serve_command(cmd, first)) {
        return false;
    }
    for (;;) {
        int r = pn532_tg_get_command(cmd, max, tag_emu_delivered() ? STRAGGLER_MS : 500);
        if (r < 0 || !serve_command(cmd, (size_t)r)) {
            break;  // Reader gone, or done with the tag.
        }
    }
    return tag_emu_delivered();
}

// Turn a scan into the virtual tag. Returns false if it does not fit.
static bool arm(const scan_t *scan) {
    static uint8_t ndef[SCANNER_MAX_TEXT + TEXT_RECORD_OVERHEAD];
    size_t len = text_record_build(scan->text, scan->len, ndef, sizeof(ndef));
    if (len == 0 || !tag_emu_load(ndef, len)) {
        ESP_LOGE(TAG, "scan of %zu bytes does not fit the tag (%d at most)", scan->len, MAX_TEXT);
        return false;
    }
    ESP_LOGI(TAG, "armed: %zu bytes, NDEF %zu bytes", scan->len, len);
    return true;
}

// Waits for scans and presents the latest one as a tag until the
// reader has taken it. The chip listens for a reader in one go; the
// task looks at the scanner between slices of that wait, and a new
// scan replaces the tag on the spot.
static void nfc_task(void *arg) {
    if (!pn532_start()) {
        ESP_LOGE(TAG, "PN532 does not answer; retrying every second");
        do {
            vTaskDelay(pdMS_TO_TICKS(1000));
        } while (!pn532_start());
    }
    ESP_LOGI(TAG, "PN532 ready");

    static scan_t scan;
    static const uint8_t nfcid3t[10] = {0};
    uint8_t nfca[6];
    uint8_t cmd[64];
    bool armed = false;
    bool listening = false;
    for (;;) {
        if (scanner_take(&scan, armed ? 0 : portMAX_DELAY)) {
            if (listening) {
                pn532_abort();
                listening = false;
            }
            armed = arm(&scan);
        }
        if (!armed) {
            continue;
        }
        if (!listening) {
            tag_emu_nfca(nfca);
            listening = pn532_tg_listen(nfca, nfcid3t);
            if (!listening) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
        }
        int n = pn532_tg_wait_initiator(cmd, sizeof(cmd), LISTEN_SLICE_MS);
        if (n == PN532_TIMEOUT) {
            continue;  // Nobody tapped yet.
        }
        listening = false;
        if (n < 0) {
            continue;  // Present the tag again.
        }
        ESP_LOGI(TAG, "reader activated the tag");
        if (serve_session(cmd, sizeof(cmd), (size_t)n)) {
            ESP_LOGI(TAG, "delivered");
            armed = false;
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "qr-bridge %s", esp_app_get_description()->version);
    scanner_init();
    pn532_init();
    xTaskCreate(nfc_task, "nfc", 8192, NULL, 10, NULL);
}
