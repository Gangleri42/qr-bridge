// main.c -- scan a QR code with the USB scanner, hold it in RAM and
// present it to the SeedHammer as an NFC Forum Type 2 tag, or as a
// series of them when it does not fit one.

#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bbqr.h"
#include "pn532.h"
#include "scanner.h"
#include "tag_emu.h"
#include "text_record.h"

static const char *TAG = "main";

// Longest scan the virtual tag can carry as a text record, and so the
// most characters a BBQr part may hold, header included.
#define MAX_TEXT (TAG_EMU_MAX_NDEF - TEXT_RECORD_OVERHEAD)

// How long to wait for a reader between looks at the scanner.
#define LISTEN_SLICE_MS 200
// The reader has the whole message once the last block is read, but
// its parser may fetch one more block for the terminator; it does so
// within a few milliseconds, and answering it lets the reader end the
// record cleanly instead of timing out.
#define STRAGGLER_MS 50
// The reader takes one part per session and is back for the next
// about a second later. Once every part has been read, this much
// silence means it left with the whole series.
#define SERIES_IDLE_MS 3000
// Listen failures in a row before the chip is set up again. A PN532
// that browned out on its own has lost its SAMConfiguration and
// rejects every TgInitAsTarget until it is repeated.
#define LISTEN_FAILURES_RESTART 10

// The armed payload. A scan that fits the tag is one text record and
// series.parts stays 0; a longer one becomes a BBQr series, one part
// per tag, presented round-robin from part 0. served counts the
// sessions that delivered a part, so part served % parts is up next
// and every part has been read once served reaches parts.
static struct {
    char encoded[BBQR_BASE32_LEN(SCANNER_MAX_TEXT)];
    bbqr_series_t series;
    size_t served;
} payload;

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
    int reads = 0;
    if (!serve_command(cmd, first)) {
        ESP_LOGW(TAG, "reader left at its first command");
        return false;
    }
    for (;;) {
        int r = pn532_tg_get_command(cmd, max, tag_emu_delivered() ? STRAGGLER_MS : 500);
        if (r < 0) {
            break;  // Reader gone, or done with the tag.
        }
        reads++;
        if (!serve_command(cmd, (size_t)r)) {
            break;
        }
    }
    if (!tag_emu_delivered()) {
        ESP_LOGW(TAG, "reader left after %d reads", reads);
    }
    return tag_emu_delivered();
}

// Load text into the virtual tag as one text record.
static bool load_text(const uint8_t *text, size_t len) {
    static uint8_t ndef[TAG_EMU_MAX_NDEF];
    size_t n = text_record_build(text, len, ndef, sizeof(ndef));
    return n != 0 && tag_emu_load(ndef, n);
}

// Load the series part due next into the virtual tag.
static void present(void) {
    static char part[MAX_TEXT];
    size_t i = payload.served % payload.series.parts;
    size_t n = bbqr_part(&payload.series, i, part);
    if (!load_text((const uint8_t *)part, n)) {
        // Parts are cut to MAX_TEXT, so this does not happen.
        ESP_LOGE(TAG, "part %zu does not fit the tag", i + 1);
    }
}

// Turn a scan into the virtual tag, or into a series of them when it
// does not fit one. Returns false if it cannot be carried.
static bool arm(const scan_t *scan) {
    payload.series.parts = 0;
    payload.served = 0;
    if (scan->len <= MAX_TEXT) {
        if (!load_text(scan->text, scan->len)) {
            ESP_LOGE(TAG, "scan of %zu bytes does not fit the tag", scan->len);
            return false;
        }
        ESP_LOGI(TAG, "armed: %zu bytes", scan->len);
        return true;
    }
    size_t len = bbqr_base32(scan->text, scan->len, payload.encoded);
    if (!bbqr_split(&payload.series, payload.encoded, len, 'U', MAX_TEXT)) {
        ESP_LOGE(TAG, "scan of %zu bytes does not fit %d parts", scan->len, BBQR_MAX_PARTS);
        return false;
    }
    present();
    ESP_LOGI(TAG, "armed: %zu bytes as %zu parts", scan->len, payload.series.parts);
    return true;
}

// Every part of a series has been read at least once.
static bool series_read(void) {
    return payload.series.parts > 0 && payload.served >= payload.series.parts;
}

// The chip refused to listen, or gave up on it. Wait before the next
// attempt, and set the chip up again if this keeps happening.
static void listen_failed(int *failures) {
    (*failures)++;
    if (*failures == 1) {
        ESP_LOGW(TAG, "PN532 will not listen; retrying");
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    if (*failures % LISTEN_FAILURES_RESTART == 0) {
        ESP_LOGW(TAG, "PN532 will not listen after %d attempts; setting it up again", *failures);
        if (!pn532_start()) {
            ESP_LOGE(TAG, "PN532 does not answer");
        }
    }
}

// Waits for scans and presents the latest one as a tag until the
// reader has taken it. The chip listens for a reader in one go; the
// task looks at the scanner between slices of that wait, and a new
// scan replaces the tag on the spot. A series cycles through its parts
// for as long as the reader keeps coming back.
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
    int listen_failures = 0;
    TickType_t last_read = 0;
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
                listen_failed(&listen_failures);
                continue;
            }
        }
        int n = pn532_tg_wait_initiator(cmd, sizeof(cmd), LISTEN_SLICE_MS);
        if (n == PN532_ERROR) {
            listening = false;
            listen_failed(&listen_failures);
            continue;
        }
        listen_failures = 0;
        if (n == PN532_TIMEOUT) {
            // Nobody tapped in this slice.
            if (series_read() && xTaskGetTickCount() - last_read > pdMS_TO_TICKS(SERIES_IDLE_MS)) {
                // The reader has stayed away since taking the last
                // part: it left with the whole series.
                ESP_LOGI(TAG, "delivered: %zu parts", payload.series.parts);
                pn532_abort();
                listening = false;
                armed = false;
            } else if (!pn532_rx_pending()) {
                // An arm issued while the reader's field is mid-poll can
                // sit there and never activate, and with no byte to end
                // the slice the wait would run forever. Drop the arm and
                // arm again, so the reader's next field cycle activates
                // the tag. Not when a response has just begun to arrive:
                // that is the activation, and the next slice reads it.
                pn532_abort();
                listening = false;
            }
            continue;
        }
        listening = false;
        ESP_LOGI(TAG, "reader activated the tag");
        if (!serve_session(cmd, sizeof(cmd), (size_t)n)) {
            continue;  // Reader left early; present the tag again.
        }
        if (payload.series.parts == 0) {
            ESP_LOGI(TAG, "delivered");
            armed = false;
            continue;
        }
        ESP_LOGI(TAG, "part %zu of %zu read", payload.served % payload.series.parts + 1,
                 payload.series.parts);
        payload.served++;
        last_read = xTaskGetTickCount();
        present();
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "qr-bridge %s", esp_app_get_description()->version);
    scanner_init();
    pn532_init();
    xTaskCreate(nfc_task, "nfc", 8192, NULL, 10, NULL);
}
