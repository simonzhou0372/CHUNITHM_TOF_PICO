/*
 * Chuni245Tof Main Program
 * NKRO Keyboard with MPR121 Slider + VL53L0X Air
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "tusb.h"
#include "class/cdc/cdc_device.h"

#include "board_defs.h"
#include "config.h"
#include "save.h"
#include "mpr121.h"
#include "slider.h"
#include "vl53l0x.h"
#include "tof_reader.h"
#include "air.h"
#include "button.h"

using namespace Chuni245Tof;

// Physical button pins
#define BUTTON_ENTER_PIN  18
#define BUTTON_2_PIN      19

// Onboard LED
#define LED_PIN 25

// NKRO Keyboard report
struct __attribute__((packed)) {
    uint8_t modifier;
    uint8_t keymap[15];
} hid_nkro, sent_hid_nkro;

static char cdc_rx_buf[256];
static uint8_t cdc_rx_pos = 0;

// HID Key codes (HID Usage Table)
#define HID_KEY_A       0x04
#define HID_KEY_B       0x05
#define HID_KEY_C       0x06
#define HID_KEY_D       0x07
#define HID_KEY_E       0x08
#define HID_KEY_F       0x09
#define HID_KEY_G       0x0A
#define HID_KEY_H       0x0B
#define HID_KEY_I       0x0C
#define HID_KEY_J       0x0D
#define HID_KEY_K       0x0E
#define HID_KEY_L       0x0F
#define HID_KEY_M       0x10
#define HID_KEY_N       0x11
#define HID_KEY_O       0x12
#define HID_KEY_P       0x13
#define HID_KEY_Q       0x14
#define HID_KEY_R       0x15
#define HID_KEY_S       0x16
#define HID_KEY_T       0x17
#define HID_KEY_U       0x18
#define HID_KEY_V       0x19
#define HID_KEY_W       0x1A
#define HID_KEY_X       0x1B
#define HID_KEY_Y       0x1C
#define HID_KEY_Z       0x1D
#define HID_KEY_1       0x1E
#define HID_KEY_2       0x1F
#define HID_KEY_3       0x20
#define HID_KEY_4       0x21
#define HID_KEY_5       0x22
#define HID_KEY_6       0x23
#define HID_KEY_7       0x24
#define HID_KEY_8       0x25
#define HID_KEY_9       0x26
#define HID_KEY_0       0x27
#define HID_KEY_ENTER   0x28
#define HID_KEY_LBRACKET  0x2F  // [
#define HID_KEY_RBRACKET  0x30  // ]
#define HID_KEY_BACKSLASH 0x31  // \
#define HID_KEY_SEMICOLON 0x33  // ;
#define HID_KEY_APOSTROPHE 0x34 // '
#define HID_KEY_COMMA   0x36
#define HID_KEY_PERIOD  0x37
#define HID_KEY_SLASH   0x38

// Slider keyboard mapping (Key 1-32)
// Hardware partition: 8/12/12 (matching MPR121 pin counts: 0x5C=8, 0x5B=12, 0x5A=12)
// Row 1 (cell 1-8):   Q W E R T Y U I
// Row 2 (cell 9-20):  O P [ ] a s d f g h j k
// Row 3 (cell 21-32): l ; ' \ z x c v b n m ,
static const uint8_t slider_keymap[32] = {
    HID_KEY_Q, HID_KEY_W, HID_KEY_E, HID_KEY_R,
    HID_KEY_T, HID_KEY_Y, HID_KEY_U, HID_KEY_I,

    HID_KEY_O, HID_KEY_P, HID_KEY_LBRACKET, HID_KEY_RBRACKET,
    HID_KEY_A, HID_KEY_S, HID_KEY_D, HID_KEY_F,
    HID_KEY_G, HID_KEY_H, HID_KEY_J, HID_KEY_K,

    HID_KEY_L, HID_KEY_SEMICOLON, HID_KEY_APOSTROPHE, HID_KEY_BACKSLASH,
    HID_KEY_Z, HID_KEY_X, HID_KEY_C, HID_KEY_V,
    HID_KEY_B, HID_KEY_N, HID_KEY_M, HID_KEY_COMMA,
};

// Air keyboard mapping (IR1-6: 4 5 6 7 8 9)
static const uint8_t air_keymap[6] = {
    HID_KEY_4,  // IR1
    HID_KEY_5,  // IR2
    HID_KEY_6,  // IR3
    HID_KEY_7,  // IR4
    HID_KEY_8,  // IR5
    HID_KEY_9,  // IR6
};

// Set key in NKRO bitmap
static void nkro_set_key(uint8_t keycode, bool pressed) {
    if (keycode >= 120) return;
    uint8_t byte = keycode / 8;
    uint8_t bit = keycode % 8;
    if (pressed) {
        hid_nkro.keymap[byte] |= (1 << bit);
    } else {
        hid_nkro.keymap[byte] &= ~(1 << bit);
    }
}

// Generate NKRO report
static void gen_nkro_report() {
    memset(&hid_nkro, 0, sizeof(hid_nkro));

    // Physical buttons (GP18=ENTER, GP19=2)
    if (!gpio_get(BUTTON_ENTER_PIN)) {
        nkro_set_key(HID_KEY_ENTER, true);
    }
    if (!gpio_get(BUTTON_2_PIN)) {
        nkro_set_key(HID_KEY_2, true);
    }

    // Slider keys (32 keys)
    uint32_t slider_state = slider_get_state();
    for (int i = 0; i < 32; i++) {
        if (slider_state & (1 << i)) {
            nkro_set_key(slider_keymap[i], true);
        }
    }

    // Air keys (6 height levels)
    uint8_t air_state = air_get_bitmap();
    for (int i = 0; i < 6; i++) {
        if (air_state & (1 << i)) {
            nkro_set_key(air_keymap[i], true);
        }
    }
}

// Send HID report
static void report_usb_hid() {
    static uint64_t next_nkro_time = 0;

    if (!tud_hid_n_ready(0)) {
        return;
    }

    if ((memcmp(&hid_nkro, &sent_hid_nkro, sizeof(hid_nkro)) != 0) ||
        (time_us_64() > next_nkro_time)) {
        if (tud_hid_n_report(0, 0, &hid_nkro, sizeof(hid_nkro))) {
            sent_hid_nkro = hid_nkro;
            next_nkro_time = time_us_64() + 4000;
        }
    }
}

// CDC command handler
static void cdc_process_command(const char* cmd) {
    // Parse CONFIG command: CONFIG touch release offset pitch air6_range
    if (strncmp(cmd, "CONFIG ", 7) == 0) {
        int touch, release, offset, pitch, air6_range, min_hold;
        int num_args = sscanf(cmd + 7, "%d %d %d %d %d %d", &touch, &release, &offset, &pitch, &air6_range);

        if (num_args >= 4) {  // 至少 4 �?参数
            // Validate ranges
            if (touch >= 5 && touch <= 30 && release >= 1 && release <= 25) {
                cfg->touch_threshold = touch;
                cfg->release_threshold = release;
                mpr121_set_thresholds(touch, release);
            }
            if (offset >= 40 && offset <= 200) {
                cfg->tof_offset = offset;
            }
            if (pitch >= 4 && pitch <= 100) {
                cfg->tof_pitch = pitch;
            }
            // Air6 range (�?选，默�?? 80)
            if (num_args >= 5 && air6_range >= pitch && air6_range <= 200) {
                cfg->air6_range = air6_range;
            } else if (num_args < 5) {
                // 如果�?提供，使用默认�?
                air6_range = cfg->air6_range;
            }

            printf("OK touch=%d release=%d offset=%d pitch=%d air6=%d\r\n",
                   cfg->touch_threshold, cfg->release_threshold,
                   cfg->tof_offset, cfg->tof_pitch, cfg->air6_range);
        } else {
            printf("ERROR Usage: CONFIG touch release offset pitch [air6_range]\r\n");
        }
    }
    else if (strcmp(cmd, "CONFIG?") == 0) {
        printf("CONFIG touch=%d release=%d offset=%d pitch=%d air6=%d\r\n",
               cfg->touch_threshold, cfg->release_threshold,
               cfg->tof_offset, cfg->tof_pitch, cfg->air6_range);
    }
    else if (strcmp(cmd, "SAVE") == 0) {
        config_save();
        printf("OK Config saved\r\n");
    }
    else if (strcmp(cmd, "DEFAULT") == 0) {
        // Reset to default values
        cfg->touch_threshold = 20;
        cfg->release_threshold = 18;
        cfg->tof_offset = 120;
        cfg->tof_pitch = 40;
        cfg->air6_range = 150;
        cfg->air_min_hold_ms = 50;
        mpr121_set_thresholds(20, 18);
        printf("OK Defaults restored (touch=20 release=18 offset=120 pitch=40 air6=150 hold=50)\r\n");
    }
    else if (strcmp(cmd, "STATUS") == 0) {
        printf("{\"slider\":0x%08lX,\"air\":0x%02X,\"i2c_err\":{\"mpr\":[%lu,%lu,%lu],\"tof\":[%lu,%lu,%lu,%lu,%lu]}}\r\n",
               slider_get_state(), air_get_bitmap(),
               mpr121_get_error_count(0), mpr121_get_error_count(1), mpr121_get_error_count(2),
               vl53l0x_get_error_count(0), vl53l0x_get_error_count(1), vl53l0x_get_error_count(2),
               vl53l0x_get_error_count(3), vl53l0x_get_error_count(4));
    }
    else if (strcmp(cmd, "SLIDER") == 0) {
        printf("Raw slider state: 0x%08lX\r\n", slider_get_state());
        printf("Cells pressed: ");
        uint32_t state = slider_get_state();
        for (int i = 0; i < 32; i++) {
            if (state & (1 << i)) {
                printf("%d ", i + 1);
            }
        }
        printf("\r\n");
    }
    else if (strcmp(cmd, "MPR") == 0) {
        printf("MPR121 raw data:\r\n");
        printf("  0x5A (cell 21-32): 0x%08lX\r\n", mpr121_get_touch_state(0));
        printf("  0x5B (cell 9-20):  0x%08lX\r\n", mpr121_get_touch_state(1));
        printf("  0x5C (cell 1-8):   0x%08lX\r\n", mpr121_get_touch_state(2));
    }
    else if (strcmp(cmd, "DIST") == 0) {
        printf("TOF distances (mm):");
        for (int i = 0; i < 5; i++) {
            printf(" %d", air_get_distance(i));
        }
        printf("\r\n");
    }
    else if (strcmp(cmd, "AIR") == 0) {
        printf("Air bitmap: 0x%02X (IR1-6: ", air_get_bitmap());
        for (int i = 0; i < 6; i++) {
            printf("%d", (air_get_bitmap() >> i) & 1);
        }
        printf(")\r\n");
    }
    else if (strcmp(cmd, "I2CSCAN") == 0) {
        printf("MPR121: ");
        for (int i = 0; i < 3; i++) {
            printf("0x%02X ", 0x5A + i);
        }
        printf("\r\nVL53L0X: ");
        for (int i = 0; i < 5; i++) {
            if (vl53l0x_is_ready(i)) {
                printf("OK ");
            } else {
                printf("X ");
            }
        }
        printf("\r\n");
    }
    else if (strcmp(cmd, "BOOTLOADER") == 0) {
        printf("OK\r\n");
        sleep_ms(100);
        reset_usb_boot(0, 0);
    }
    else if (strcmp(cmd, "DEBUG") == 0) {
        // 打印 MPR121 调试信息: Baseline / FilteredData / Delta
        mpr121_debug_print();
    }
    else if (strcmp(cmd, "RESET") == 0) {
        // 重置 MPR121 Baseline
        mpr121_reset_baseline();
    }
    else if (strcmp(cmd, "HELP") == 0) {
        printf("Commands: CONFIG, CONFIG?, SAVE, DEFAULT, STATUS, SLIDER, MPR, DEBUG, RESET, DIST, AIR, I2CSCAN, BOOTLOADER, HELP\r\n");
    }
}

// CDC task
static void cdc_task() {
    while (tud_cdc_available()) {
        char c = tud_cdc_read_char();
        if (c == '\r' || c == '\n') {
            if (cdc_rx_pos > 0) {
                cdc_rx_buf[cdc_rx_pos] = '\0';
                cdc_process_command(cdc_rx_buf);
                cdc_rx_pos = 0;
            }
        } else if (cdc_rx_pos < sizeof(cdc_rx_buf) - 1) {
            cdc_rx_buf[cdc_rx_pos++] = c;
        }
    }
}

int main(void) {
    set_sys_clock_khz(150000, true);
    stdio_init_all();

    // Initialize LED
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 1);

    // Initialize physical buttons
    gpio_init(BUTTON_ENTER_PIN);
    gpio_set_dir(BUTTON_ENTER_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_ENTER_PIN);

    gpio_init(BUTTON_2_PIN);
    gpio_set_dir(BUTTON_2_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_2_PIN);

    printf("\n========================================\n");
    printf("   Chuni245Tof Controller\n");
    printf("========================================\n\n");

    tusb_init();

    static mutex_t lock;
    mutex_init(&lock);

    config_init();
    save_init(0xCA34CAFE, &lock);

    printf("Initializing sensors...\n");
    button_init();
    slider_init();
    vl53l0x_init();  // 先初始化 VL53L0X
    air_init();      // 再启动 Core 1 读取任务
    printf("Ready.\n\n");

    printf("Commands: STATUS, DIST, AIR, I2CSCAN, BOOTLOADER, HELP\n");
    printf("Send DIST to see TOF readings\n\n");

    uint32_t last_log = 0;
    uint32_t last_debug = 0;

    while (1) {
        tud_task();
        cdc_task();

        slider_update();
        air_update();
        button_update();

        gen_nkro_report();
        report_usb_hid();

        save_loop();

        // LED shows activity
        uint32_t now = to_ms_since_boot(get_absolute_time());
        gpio_put(LED_PIN, (now / 500) % 2);

        // 调试输出：每 100ms 打印一次（减少阻塞）
        static uint32_t last_debug = 0;
        if (now - last_debug >= 100) {
            last_debug = now;
            printf("Air=0x%02X Dist=%d | Core1: %lu reads, %lu err | I2C: MPR[%lu,%lu,%lu] TOF[%lu,%lu,%lu,%lu,%lu]\n",
                   air_get_bitmap(), air_get_distance(0),
                   tof_reader_get_count(), tof_reader_get_error_count(),
                   mpr121_get_error_count(0), mpr121_get_error_count(1), mpr121_get_error_count(2),
                   vl53l0x_get_error_count(0), vl53l0x_get_error_count(1), vl53l0x_get_error_count(2),
                   vl53l0x_get_error_count(3), vl53l0x_get_error_count(4));
        }
    }

    return 0;
}

// HID callbacks
extern "C" {
uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id,
                                hid_report_type_t report_type,
                                uint8_t* buffer, uint16_t reqlen) {
    return 0;
}

void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const* buffer, uint16_t bufsize) {
}
}