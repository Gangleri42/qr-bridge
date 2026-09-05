// scanner.c -- USB HID keyboard input from a barcode scanner.
//
// The scanner enumerates as a boot-protocol keyboard and types each
// decoded code followed by Enter. The ESP-IDF USB Host Library drives
// the port and the usb_host_hid component the HID class. Reports
// arrive on the HID driver's task and are decoded with a US keymap
// into the pending scan; Enter hands the scan to scanner_take over a
// length-one queue, so the newest scan always wins.

#include "scanner.h"

#include <assert.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/hid_host.h"
#include "usb/usb_host.h"

static const char *TAG = "scanner";

// Boot-protocol keyboard report: modifiers, reserved, six keycodes.
#define REPORT_LEN 8
#define MOD_SHIFT (0x02 | 0x20)  // Left and right shift.
#define KEY_ENTER 0x28
#define KEY_BACKSPACE 0x2a

// US keyboard layout: keycode -> ASCII (plain, shifted).
static const struct {
    uint8_t plain;
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
    [0x2b] = {'\t', '\t'},
    [0x2c] = {' ', ' '},
    [0x2d] = {'-', '_'}, [0x2e] = {'=', '+'},
    [0x2f] = {'[', '{'}, [0x30] = {']', '}'},
    [0x31] = {'\\', '|'}, [0x32] = {'#', '~'},
    [0x33] = {';', ':'}, [0x34] = {'\'', '"'},
    [0x35] = {'`', '~'}, [0x36] = {',', '<'},
    [0x37] = {'.', '>'}, [0x38] = {'/', '?'},
};

static QueueHandle_t scans;    // scan_t, length one: the latest scan.
static QueueHandle_t devices;  // hid_host_device_handle_t to open.

// Keyboard state, touched only on the HID driver's task.
static scan_t pending;         // Keystrokes since the last Enter.
static bool overflow;          // The pending scan outgrew its buffer.
static uint8_t held[6];        // Keycodes down in the previous report.

static void key_pressed(uint8_t keycode, uint8_t modifiers) {
    if (keycode == KEY_ENTER) {
        if (overflow) {
            ESP_LOGW(TAG, "scan longer than %d bytes discarded", SCANNER_MAX_TEXT);
        } else if (pending.len > 0) {
            ESP_LOGI(TAG, "scan: %zu bytes", pending.len);
            xQueueOverwrite(scans, &pending);
        }
        pending.len = 0;
        overflow = false;
        return;
    }
    if (keycode == KEY_BACKSPACE) {
        if (pending.len > 0) {
            pending.len--;
        }
        return;
    }
    if (keycode >= sizeof(keymap) / sizeof(keymap[0])) {
        return;
    }
    uint8_t ch = (modifiers & MOD_SHIFT) ? keymap[keycode].shifted : keymap[keycode].plain;
    if (ch == 0) {
        return;
    }
    if (pending.len == SCANNER_MAX_TEXT) {
        overflow = true;
        return;
    }
    pending.text[pending.len++] = ch;
}

static bool was_held(uint8_t keycode) {
    for (size_t i = 0; i < sizeof(held); i++) {
        if (held[i] == keycode) {
            return true;
        }
    }
    return false;
}

// A report lists every key currently down; a key counts as pressed
// the first report it appears in.
static void handle_report(const uint8_t *report, size_t len) {
    if (len < REPORT_LEN) {
        return;
    }
    for (int i = 2; i < REPORT_LEN; i++) {
        if (report[i] != 0 && !was_held(report[i])) {
            key_pressed(report[i], report[0]);
        }
    }
    memcpy(held, &report[2], sizeof(held));
}

static void reset_keyboard(void) {
    pending.len = 0;
    overflow = false;
    memset(held, 0, sizeof(held));
}

static void interface_event(hid_host_device_handle_t dev,
                            const hid_host_interface_event_t event, void *arg) {
    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
        uint8_t report[64];
        size_t len = 0;
        if (hid_host_device_get_raw_input_report_data(dev, report, sizeof(report), &len) == ESP_OK) {
            handle_report(report, len);
        }
        break;
    }
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "scanner disconnected");
        reset_keyboard();
        hid_host_device_close(dev);
        break;
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ESP_LOGW(TAG, "HID transfer error");
        break;
    }
}

static void driver_event(hid_host_device_handle_t dev,
                         const hid_host_driver_event_t event, void *arg) {
    if (event == HID_HOST_DRIVER_EVENT_CONNECTED) {
        xQueueSend(devices, &dev, 0);
    }
}

// Opens each keyboard the driver reports. Runs on its own task because
// the class requests wait on transfers that the driver's task
// completes, so they cannot be issued from its callback.
static void open_task(void *arg) {
    for (;;) {
        hid_host_device_handle_t dev;
        xQueueReceive(devices, &dev, portMAX_DELAY);

        hid_host_dev_params_t params;
        if (hid_host_device_get_params(dev, &params) != ESP_OK) {
            continue;
        }
        if (params.sub_class != HID_SUBCLASS_BOOT_INTERFACE ||
            params.proto != HID_PROTOCOL_KEYBOARD) {
            ESP_LOGW(TAG, "ignoring HID interface %u: subclass %u protocol %u is not a boot keyboard",
                     params.iface_num, params.sub_class, params.proto);
            continue;
        }

        const hid_host_device_config_t config = {
            .callback = interface_event,
            .callback_arg = NULL,
        };
        esp_err_t err = hid_host_device_open(dev, &config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "cannot open the keyboard: %s", esp_err_to_name(err));
            continue;
        }
        // Boot protocol gives the fixed 8-byte report decoded above;
        // idle 0 makes the device report on change only. A device that
        // refuses either keeps its power-on defaults.
        err = hid_class_request_set_protocol(dev, HID_REPORT_PROTOCOL_BOOT);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Set_Protocol refused: %s", esp_err_to_name(err));
        }
        err = hid_class_request_set_idle(dev, 0, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Set_Idle refused: %s", esp_err_to_name(err));
        }
        err = hid_host_device_start(dev);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "cannot start the keyboard: %s", esp_err_to_name(err));
            hid_host_device_close(dev);
            continue;
        }
        hid_host_dev_info_t info;
        if (hid_host_get_device_info(dev, &info) == ESP_OK) {
            ESP_LOGI(TAG, "scanner connected: VID %04x PID %04x", info.VID, info.PID);
        }
    }
}

static void usb_host_lib_task(void *arg) {
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

void scanner_init(void) {
    scans = xQueueCreate(1, sizeof(scan_t));
    devices = xQueueCreate(4, sizeof(hid_host_device_handle_t));
    assert(scans != NULL && devices != NULL);

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
        .callback = driver_event,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(hid_host_install(&hid_config));

    xTaskCreate(usb_host_lib_task, "usb_host", 4096, NULL, 10, NULL);
    xTaskCreate(open_task, "hid_open", 4096, NULL, 5, NULL);
}

bool scanner_take(scan_t *out, TickType_t ticks) {
    return xQueueReceive(scans, out, ticks) == pdTRUE;
}
