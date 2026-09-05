// scanner.c -- ESP-IDF native USB host + HID host keyboard parser for NT-1228BL.
//
// Uses the ESP-IDF 'usb' component (native HCD + PHY setup) and the
// 'usb_host_hid' managed component for HID class support.

#include "scanner.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"

static const char *TAG = "scanner";

// US keyboard layout: keycode -> ASCII (unshifted/shifted)
static const struct {
    uint8_t unshifted;
    uint8_t shifted;
} keymap[128] = {
    [0x04] = {'a', 'A'}, [0x05] = {'b', 'B'}, [0x06] = {'c', 'C'}, [0x07] = {'d', 'D'},
    [0x08] = {'e', 'E'}, [0x09] = {'f', 'F'}, [0x0a] = {'g', 'G'}, [0x0b] = {'h', 'H'},
    [0x0c] = {'i', 'I'}, [0x0d] = {'j', 'J'}, [0x0e] = {'k', 'K'}, [0x0f] = {'l', 'L'},
    [0x10] = {'m', 'M'}, [0x11] = {'n', 'N'}, [0x12] = {'o', 'O'}, [0x13] = {'p', 'P'},
    [0x14] = {'q', 'Q'}, [0x15] = {'r', 'R'}, [0x16] = {'s', 'S'}, [0x17] = {'t', 'T'},
    [0x18] = {'u', 'U'}, [0x19] = {'v', 'V'}, [0x1a] = {'w', 'W'}, [0x1b] = {'x', 'X'},
    [0x1c] = {'y', 'Y'}, [0x1d] = {'z', 'Z'},
    [0x1e] = {'1', '!'}, [0x1f] = {'2', '@'}, [0x20] = {'3', '#'}, [0x21] = {'4', '$'},
    [0x22] = {'5', '%'}, [0x23] = {'6', '^'}, [0x24] = {'7', '&'}, [0x25] = {'8', '*'},
    [0x26] = {'9', '('}, [0x27] = {'0', ')'},
    [0x28] = {'\n', '\n'},  // ENTER
    [0x2a] = {'\b', '\b'},   // BACKSPACE
    [0x2b] = {'\t', '\t'},   // TAB
    [0x2c] = {' ', ' '},     // SPACE
    [0x2d] = {'-', '_'}, [0x2e] = {'=', '+'},
    [0x2f] = {'[', '{'}, [0x30] = {']', '}'},
    [0x31] = {'\\', '|'}, [0x32] = {'#', '~'},
    [0x33] = {';', ':'}, [0x34] = {'\'', '"'},
    [0x35] = {'`', '~'}, [0x36] = {',', '<'},
    [0x37] = {'.', '>'}, [0x38] = {'/', '?'},
};

#define MAX_PAYLOAD 4096
static uint8_t payload_buf[MAX_PAYLOAD];
static size_t payload_len = 0;

static uint8_t prev_report[8];

static uint8_t map_keycode(uint8_t keycode, bool shift) {
    if (keycode >= sizeof(keymap) / sizeof(keymap[0])) return 0;
    return shift ? keymap[keycode].shifted : keymap[keycode].unshifted;
}

static void process_keypress(uint8_t keycode, uint8_t modifiers) {
    if (keycode == 0x28) { // ENTER
        if (payload_len > 0) {
            if (payload_buf[payload_len - 1] == '\n') {
                payload_len--;
            }
            if (payload_len > 0) {
                ESP_LOGI(TAG, "Payload armed: %zu bytes", payload_len);
                scanner_on_payload(payload_buf, payload_len);
            }
        }
        payload_len = 0;
        return;
    }

    if (keycode == 0x2a) { // BACKSPACE
        if (payload_len > 0) payload_len--;
        return;
    }

    bool shift = (modifiers & (0x02 | 0x20)) != 0;
    uint8_t ch = map_keycode(keycode, shift);

    if (ch != 0 && ch != '\n' && payload_len < MAX_PAYLOAD - 1) {
        payload_buf[payload_len++] = ch;
    }
}

static bool was_pressed(uint8_t keycode) {
    for (int i = 2; i < 8; i++) {
        if (prev_report[i] == keycode) return true;
    }
    return false;
}

static void handle_keyboard_report(const uint8_t *report, size_t len) {
    if (len < 8) return;
    uint8_t modifiers = report[0];
    for (int i = 2; i < 8; i++) {
        uint8_t kc = report[i];
        if (kc == 0) continue;
        if (!was_pressed(kc)) {
            process_keypress(kc, modifiers);
        }
    }
    memcpy(prev_report, report, 8);
}

static void hid_interface_event_cb(hid_host_device_handle_t hid_device_handle,
                                   const hid_host_interface_event_t event,
                                   void *arg) {
    switch (event) {
        case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
            uint8_t data[64];
            size_t data_length = 0;
            esp_err_t err = hid_host_device_get_raw_input_report_data(
                hid_device_handle, data, sizeof(data), &data_length);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "report (%zu): %02x %02x %02x %02x %02x %02x %02x %02x",
                         data_length,
                         data_length > 0 ? data[0] : 0, data_length > 1 ? data[1] : 0,
                         data_length > 2 ? data[2] : 0, data_length > 3 ? data[3] : 0,
                         data_length > 4 ? data[4] : 0, data_length > 5 ? data[5] : 0,
                         data_length > 6 ? data[6] : 0, data_length > 7 ? data[7] : 0);
                handle_keyboard_report(data, data_length);
            } else {
                ESP_LOGW(TAG, "report read err: %s", esp_err_to_name(err));
            }
            break;
        }
        case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "HID device disconnected");
            payload_len = 0;
            memset(prev_report, 0, sizeof(prev_report));
            break;
        case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
            ESP_LOGW(TAG, "HID transfer error");
            break;
        default:
            break;
    }
}

static void hid_driver_event_cb(hid_host_device_handle_t hid_device_handle,
                                const hid_host_driver_event_t event,
                                void *arg) {
    if (event != HID_HOST_DRIVER_EVENT_CONNECTED) return;

    hid_host_dev_params_t params;
    ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &params));
    ESP_LOGI(TAG, "HID device connected: addr=%d iface=%d sub_class=%d proto=%d",
             params.addr, params.iface_num, params.sub_class, params.proto);

    hid_host_dev_info_t info;
    if (hid_host_get_device_info(hid_device_handle, &info) == ESP_OK) {
        ESP_LOGI(TAG, "  VID=0x%04x PID=0x%04x", info.VID, info.PID);
    }

    const hid_host_device_config_t dev_config = {
        .callback = hid_interface_event_cb,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(hid_host_device_open(hid_device_handle, &dev_config));

    // device_open leaves the interface in READY state with no transfers
    // submitted; start() begins the interrupt-IN pipe.
    ESP_ERROR_CHECK(hid_host_device_start(hid_device_handle));
    ESP_LOGI(TAG, "HID device opened, listening for keystrokes");
}

static void usb_host_lib_task(void *arg) {
    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "USB: all devices freed");
        }
    }
}

void scanner_init(void) {
    ESP_LOGI(TAG, "Initializing USB host");

    // Verbose HID/USB logging while debugging enumeration issues
    esp_log_level_set("hid_host", ESP_LOG_DEBUG);
    esp_log_level_set("usb", ESP_LOG_DEBUG);

    // peripheral_map BIT0 is USB controller 0: on the ESP32-P4 that is
    // the OTG 2.0 (high-speed) core the MX1.25 header is wired to, and
    // the core a map of 0 would select by default. PHY setup is
    // automatic.
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        .peripheral_map = BIT0,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));

    const hid_host_driver_config_t hid_config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = hid_driver_event_cb,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(hid_host_install(&hid_config));

    xTaskCreate(usb_host_lib_task, "usb_host_lib", 4096, NULL, 10, NULL);

    ESP_LOGI(TAG, "Scanner init done");
}

void scanner_clear_payload(void) {
    payload_len = 0;
    memset(prev_report, 0, sizeof(prev_report));
}
