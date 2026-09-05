// main.c -- scan a QR code with the USB scanner, hold it in RAM and
// present it to the SeedHammer as an NFC Forum Type 2 tag.

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "scanner.h"
#include "pn532.h"
#include "text_record.h"
#include "tag_emu.h"

static const char *TAG = "main";

// Armed payload and NDEF message buffers
static uint8_t armed_payload[4096];
static size_t armed_len = 0;
static uint8_t ndef_msg[4106]; // payload + max NDEF overhead (10 bytes)
static size_t ndef_len = 0;

// Callback from scanner when payload is received
void scanner_on_payload(const uint8_t *payload, size_t len) {
    ESP_LOGI(TAG, "Scanner callback: %zu bytes", len);

    if (len == 0 || len > sizeof(armed_payload)) {
        ESP_LOGE(TAG, "Invalid payload length: %zu", len);
        return;
    }

    // Build NDEF Text record
    size_t msg_len = text_record_build(payload, len, ndef_msg, sizeof(ndef_msg));
    if (msg_len == 0) {
        ESP_LOGE(TAG, "NDEF build failed");
        return;
    }

    memcpy(armed_payload, payload, len);
    armed_len = len;
    ndef_len = msg_len;

    ESP_LOGI(TAG, "Payload armed: %zu bytes, NDEF: %zu bytes", armed_len, ndef_len);
}

// Serve one Type 2 command from the SeedHammer's reader. READ (0x30)
// gets the four emulated blocks back; anything else is ignored and the
// session continues.
static bool serve_cmd(const uint8_t *cmd, size_t len) {
    if (len >= 2 && cmd[0] == 0x30) {
        uint8_t block = cmd[1];
        uint8_t out[16];
        tag_emu_read(block, out);
        ESP_LOGD(TAG, "T2 read block %u", block);
        return pn532_tg_set_data(out, sizeof(out), 300) == 0;
    }
    return true;
}

// Present the armed payload as a passive Type 2 tag until the
// SeedHammer's scanner has read it (or the initiator walks away).
static bool tag_emu_serve(void) {
    // NTAG-shaped anti-collision parameters: SENS_RES 44 00, the low
    // three UID bytes, SEL_RES 00 (plain Type 2, no ISO-DEP).
    static const uint8_t nfca[6] = {0x44, 0x00, 0x11, 0x22, 0x33, 0x00};
    static const uint8_t nfcid3t[10] = {0};
    uint8_t cmd[64];

    // Re-arm the passive target; blocks until an initiator activates
    // the card. A timeout just means no one tapped.
    int n = pn532_tg_init_as_target(nfca, nfcid3t, cmd, sizeof(cmd), 200);
    if (n < 0) {
        return false;
    }
    ESP_LOGI(TAG, "SeedHammer activated the emulated tag");
    if (n > 0) {
        serve_cmd(cmd, (size_t)n);
    }

    while (!tag_emu_is_delivered()) {
        int r = pn532_tg_get_data(cmd, sizeof(cmd), 500);
        if (r <= 0) {
            break;  // Reader gone; re-arm for the next tap.
        }
        if (!serve_cmd(cmd, (size_t)r)) {
            break;
        }
    }
    return tag_emu_is_delivered();
}

// NFC task: emulates a passive tag while a payload is armed.
static void nfc_task(void *arg) {
    ESP_LOGI(TAG, "NFC task started");

    if (!pn532_init()) {
        ESP_LOGE(TAG, "PN532 init failed");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "PN532 ready");

    // The armed payload this task last turned into a virtual tag.
    static uint32_t armed_checksum = 0;

    while (1) {
        // Only emulate when we have an armed payload.
        if (armed_len > 0) {
            // Rebuild the virtual tag only when the payload changed.
            uint32_t sum = 0;
            for (size_t i = 0; i < armed_len; i++) {
                sum += armed_payload[i];
            }
            if (sum != armed_checksum || ndef_len == 0) {
                armed_checksum = sum;
                size_t msg_len = text_record_build(armed_payload, armed_len,
                                                   ndef_msg, sizeof(ndef_msg));
                if (msg_len == 0 || !tag_emu_load(ndef_msg, msg_len)) {
                    ESP_LOGE(TAG, "cannot build virtual tag");
                    armed_len = 0;
                    continue;
                }
                ndef_len = msg_len;
                ESP_LOGI(TAG, "emulating tag, NDEF %zu bytes", ndef_len);
            }

            if (tag_emu_serve()) {
                ESP_LOGI(TAG, "payload delivered to SeedHammer");
                armed_len = 0;
                ndef_len = 0;
                armed_checksum = 0;
                scanner_clear_payload();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20)); // Loop period
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "qr-bridge starting");

    scanner_init();
    xTaskCreate(nfc_task, "nfc_task", 8192, NULL, 10, NULL);

    ESP_LOGI(TAG, "All tasks started");
}
