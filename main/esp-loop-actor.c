#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "cJSON.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "class/hid/hid_device.h"

#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wformat-truncation"

// Forward declarations
static void ascii_sanitize_copy(char *dst, size_t dst_size, const char *src);
extern void start_web_server(void);

// 三击检测相关状态
static bool waiting_triple = false;
static TickType_t second_short_tick = 0;
static TaskHandle_t ui_task_handle = NULL;   // 用于暂停 UI 任务显示网络信息

// 新增执行命令
//#define CMD_SHOW_INFO   5

// 导出给 web_server 使用的函数声明
extern void web_set_wifi_password(const char *pass);
extern const char* web_get_wifi_password(void);

//循环函数
#define TAG "LOOP_ACTOR"

// -------------------- Hardware --------------------
#define BOOT_BUTTON_GPIO   0
#define LED_GPIO           48
#define I2C_MASTER_SCL_IO  4
#define I2C_MASTER_SDA_IO  5
#define I2C_MASTER_NUM     I2C_NUM_0
#define I2C_MASTER_FREQ_HZ 100000
#define SCREEN_ADDR        0x3C

// -------------------- NVS --------------------
#define NVS_NAMESPACE "storage"
#define NVS_KEY_LINE  "line_idx"

// -------------------- HID absolute coordinates --------------------
#define HID_ABS_MAX 65535   // 16-bit absolute range (0..65535)

// -------------------- UI --------------------
typedef enum { UI_IDLE, UI_STEP, UI_AUTO, UI_RESET, UI_ERROR } ui_mode_t;

typedef struct {
    ui_mode_t mode;
    int cur;
    int total;
    char line_content[21];
    char hint[21];
} ui_msg_t;

static QueueHandle_t ui_queue;

// -------------------- Execution commands --------------------
typedef enum {
    CMD_NONE = 0,
    CMD_STEP_ONCE,
    CMD_AUTO_START,
    CMD_RESET_ALL,
    CMD_ABORT,
    CMD_SHOW_INFO
} exec_cmd_t;

static QueueHandle_t exec_queue;

// -------------------- Global state --------------------
static volatile ui_mode_t g_mode = UI_IDLE;
static volatile bool g_running = false;
static volatile bool g_abort = false;

static int g_current_line = 1;          // 1‑based
static int g_total_lines = 0;
static char **g_txt_lines = NULL;

static int g_last_mouse_x = 0;          // logical coordinates
static int g_last_mouse_y = 0;

static int g_base_width  = 1920;
static int g_base_height = 1080;
static int g_disp_width  = 1920;
static int g_disp_height = 1080;

static int g_default_pre_delay_ms  = 20;
static int g_default_post_delay_ms = 80;

static int g_reset_log_x = -10;
static int g_reset_log_y = -10;

static nvs_handle_t g_nvs_handle;

// -------------------- JSON compiled steps --------------------
typedef enum {
    ACT_MOUSE_MOVE,
    ACT_MOUSE_CLICK,
    ACT_MOUSE_DRAG,
    ACT_KEYBOARD_INPUT,
    ACT_KEYBOARD_HOTKEY,
    ACT_WAIT,
    ACT_LOOP_RESET
} action_type_t;

typedef struct {
    action_type_t type;
    uint16_t pre_delay_ms;
    uint16_t post_delay_ms;
    char ui_hint[21];
    union {
        struct {
            int16_t x; int16_t y;
            bool jitter_enabled;
            uint8_t jitter_strength;
            int16_t h_min, h_max;
            int16_t v_min, v_max;
        } move;
        struct { uint8_t button; uint8_t click_type; } click;
        struct { int16_t start_x, start_y, end_x, end_y; uint8_t button; } drag;
        struct {
            uint8_t mode;
            uint16_t line_index;             // 0xFFFF = AUTO_INCREMENT
            char literal_text[128];
            bool random_delay;
            uint16_t rand_min_ms, rand_max_ms;
        } keyboard;
        struct { uint8_t modifier_mask; uint8_t key_code; } hotkey;
        uint32_t wait_duration;
    } params;
} step_t;

static step_t *g_steps = NULL;
static int g_step_count = 0;

// -------------------- CH1116 OLED driver (ASCII only) --------------------
static uint8_t screen_buffer[128 * 8];

static void ch1116_write_cmd(uint8_t cmd) {
    uint8_t buf[2] = {0x00, cmd};
    i2c_master_write_to_device(I2C_MASTER_NUM, SCREEN_ADDR, buf, 2, pdMS_TO_TICKS(100));
}
static void ch1116_write_data(uint8_t data) {
    uint8_t buf[2] = {0x40, data};
    i2c_master_write_to_device(I2C_MASTER_NUM, SCREEN_ADDR, buf, 2, pdMS_TO_TICKS(100));
}

static void ch1116_init(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    i2c_param_config(I2C_MASTER_NUM, &conf);
    i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
    ch1116_write_cmd(0xAE);
    ch1116_write_cmd(0xD5); ch1116_write_cmd(0x80);
    ch1116_write_cmd(0xA8); ch1116_write_cmd(0x3F);
    ch1116_write_cmd(0xD3); ch1116_write_cmd(0x00);
    ch1116_write_cmd(0x40);
    ch1116_write_cmd(0x8D); ch1116_write_cmd(0x14);
    ch1116_write_cmd(0x20); ch1116_write_cmd(0x00);
    ch1116_write_cmd(0xA1);
    ch1116_write_cmd(0xC8);
    ch1116_write_cmd(0xDA); ch1116_write_cmd(0x12);
    ch1116_write_cmd(0x81); ch1116_write_cmd(0xCF);
    ch1116_write_cmd(0xD9); ch1116_write_cmd(0xF1);
    ch1116_write_cmd(0xDB); ch1116_write_cmd(0x40);
    ch1116_write_cmd(0xA4);
    ch1116_write_cmd(0xA6);
    ch1116_write_cmd(0xAF);
}

static void ch1116_clear(void) { memset(screen_buffer, 0, sizeof(screen_buffer)); }
static void ch1116_update(void) {
    for (int page = 0; page < 8; page++) {
        ch1116_write_cmd(0xB0 + page);
        ch1116_write_cmd(0x02);
        ch1116_write_cmd(0x10);
        for (int col = 0; col < 128; col++)
            ch1116_write_data(screen_buffer[page * 128 + col]);
    }
}
static void ch1116_draw_pixel(int x, int y, int color) {
    if (x < 0 || x >= 128 || y < 0 || y >= 64) return;
    int page = y / 8, bit = y % 8;
    if (color) screen_buffer[page * 128 + x] |= (1 << bit);
    else       screen_buffer[page * 128 + x] &= ~(1 << bit);
}

static const uint8_t font8x8[][8] = {
    {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00},{0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00},
    {0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00},{0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00},
    {0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00},{0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00},
    {0x3C,0x66,0x60,0x7C,0x66,0x66,0x3C,0x00},{0x7E,0x66,0x06,0x0C,0x18,0x18,0x18,0x00},
    {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00},{0x3C,0x66,0x66,0x3E,0x06,0x66,0x3C,0x00},
    {0x18,0x3C,0x66,0x7E,0x66,0x66,0x66,0x00},{0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00},
    {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00},{0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00},
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00},{0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00},
    {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3C,0x00},{0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00},
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x7E,0x00},{0x1E,0x06,0x06,0x06,0x06,0x66,0x3C,0x00},
    {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00},{0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00},
    {0x66,0x7E,0x7E,0x66,0x66,0x66,0x66,0x00},{0x66,0x76,0x7E,0x6E,0x66,0x66,0x66,0x00},
    {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},{0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00},
    {0x3C,0x66,0x66,0x66,0x6E,0x6C,0x3E,0x00},{0x7C,0x66,0x66,0x7C,0x78,0x6C,0x66,0x00},
    {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00},{0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00},
    {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},{0x66,0x66,0x66,0x66,0x3C,0x18,0x18,0x00},
    {0x66,0x66,0x66,0x66,0x7E,0x7E,0x66,0x00},{0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00},
    {0x66,0x66,0x3C,0x18,0x18,0x18,0x18,0x00},{0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},{0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00},
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},{0x00,0x00,0x00,0x00,0x00,0x30,0x30,0x00},
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00},{0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}
};
static int get_char_index(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return 10 + (c - 'A');
    if (c >= 'a' && c <= 'z') return 10 + (c - 'a');
    switch (c) { case ' ': return 36; case '+': return 37; case '-': return 38; case '.': return 39; case ':': return 40; case ',': return 41; default: return 36; }
}
static void ch1116_draw_char(int x, int y, char c) {
    int idx = get_char_index(c);
    const uint8_t *glyph = font8x8[idx];
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            if (glyph[i] & (1 << (7 - j))) ch1116_draw_pixel(x + j, y + i, 1);
}
static void ch1116_draw_string(int x, int y, const char *str) {
    while (*str) { ch1116_draw_char(x, y, *str); x += 8; if (x > 120) break; str++; }
}

// -------------------- HID report descriptor (abs mouse + 6KRO keyboard) --------------------
static const uint8_t hid_report_desc[] = {
    // Mouse (absolute, 3 buttons)
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x01,
    0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03, 0x81, 0x02,
    0x75, 0x05, 0x95, 0x01, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00,
    0x35, 0x00, 0x47, 0xFF, 0xFF, 0x00, 0x00,
    0x75, 0x10, 0x95, 0x02, 0x81, 0x02,
    0xC0, 0xC0,

    // Keyboard (6KRO)
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x02,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x01,
    0x15, 0x00, 0x25, 0x65, 0x75, 0x08, 0x95, 0x06, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0
};

static const tusb_desc_device_t device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00, .bDeviceSubClass = 0x00, .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A, .idProduct = 0x4002, .bcdDevice = 0x0100,
    .iManufacturer = 0x01, .iProduct = 0x02, .iSerialNumber = 0x03,
    .bNumConfigurations = 1
};

static const uint8_t hid_configuration[] = {
    0x09, 0x02, 0x22, 0x00, 0x01, 0x01, 0x00, 0x80, 0x32,
    0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22,
    (uint8_t)(sizeof(hid_report_desc) & 0xFF), (uint8_t)((sizeof(hid_report_desc) >> 8) & 0xFF),
    0x07, 0x05, 0x81, 0x03, 0x10, 0x00, 0x0A
};

static const char *string_desc[] = {
    (const char[]){0x09, 0x04},
    "ESP-Loop-Actor",
    "HID Mouse+Keyboard",
    "000001",
};

uint8_t const * tud_hid_descriptor_report_cb(uint8_t instance) { (void)instance; return hid_report_desc; }
uint16_t tud_hid_descriptor_report_len_cb(uint8_t instance) { (void)instance; return sizeof(hid_report_desc); }
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen; return 0;
}
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
}

// -------------------- HID helpers --------------------
static inline void hid_wait_ready(void) {
    while (!tud_hid_ready()) { vTaskDelay(pdMS_TO_TICKS(1)); }
}

static void mouse_report_abs(uint8_t buttons, uint16_t x, uint16_t y) {
    uint8_t report[5] = { buttons, (uint8_t)(x & 0xFF), (uint8_t)((x >> 8) & 0xFF), (uint8_t)(y & 0xFF), (uint8_t)((y >> 8) & 0xFF) };
    hid_wait_ready();
    tud_hid_report(1, report, sizeof(report));
}

typedef struct { uint8_t mod; uint8_t key; } hid_key_t;
#define MOD_LCTRL  0x01
#define MOD_LSHIFT 0x02
#define MOD_LALT   0x04
#define MOD_LGUI   0x08

static hid_key_t ascii_to_hid(char c) {
    hid_key_t r = {0,0};
    if (c >= 'a' && c <= 'z') { r.key = 0x04 + (c - 'a'); return r; }
    if (c >= 'A' && c <= 'Z') { r.mod = MOD_LSHIFT; r.key = 0x04 + (c - 'A'); return r; }
    if (c >= '1' && c <= '9') { r.key = 0x1E + (c - '1'); return r; }
    if (c == '0') { r.key = 0x27; return r; }
    switch (c) {
        case ' ': r.key = 0x2C; break;
        case '\n': r.key = 0x28; break;
        case '\t': r.key = 0x2B; break;
        case '-': r.key = 0x2D; break;
        case '=': r.key = 0x2E; break;
        case '[': r.key = 0x2F; break;
        case ']': r.key = 0x30; break;
        case '\\': r.key = 0x31; break;
        case ';': r.key = 0x33; break;
        case '\'': r.key = 0x34; break;
        case '`': r.key = 0x35; break;
        case ',': r.key = 0x36; break;
        case '.': r.key = 0x37; break;
        case '/': r.key = 0x38; break;
        default: r.key = 0; break;
    }
    return r;
}

static void keyboard_send_raw(uint8_t mod, uint8_t keycode) {
    uint8_t report[8] = { mod, 0x00, keycode, 0,0,0,0,0 };
    hid_wait_ready();
    tud_hid_report(2, report, sizeof(report));
    vTaskDelay(pdMS_TO_TICKS(12));
    uint8_t release[8] = {0};
    hid_wait_ready();
    tud_hid_report(2, release, sizeof(release));
    vTaskDelay(pdMS_TO_TICKS(12));
}

static void keyboard_send_text(const char *str, bool random_delay, uint16_t min_ms, uint16_t max_ms) {
    if (!str) return;
    for (size_t i = 0; str[i]; i++) {
        if ((unsigned char)str[i] >= 0x80) continue;
        hid_key_t k = ascii_to_hid(str[i]);
        if (k.key == 0) continue;
        keyboard_send_raw(k.mod, k.key);
        if (random_delay) {
            if (max_ms < min_ms) { uint16_t t = min_ms; min_ms = max_ms; max_ms = t; }
            uint16_t span = (max_ms >= min_ms) ? (max_ms - min_ms + 1) : 1;
            vTaskDelay(pdMS_TO_TICKS(min_ms + (esp_random() % span)));
        }
    }
}

static uint8_t modifier_name_to_mask(const char *name) {
    if (!name) return 0;
    if (strcmp(name, "CTRL") == 0)  return MOD_LCTRL;
    if (strcmp(name, "SHIFT") == 0) return MOD_LSHIFT;
    if (strcmp(name, "ALT") == 0)   return MOD_LALT;
    if (strcmp(name, "GUI") == 0)   return MOD_LGUI;
    return 0;
}

// -------------------- Mapping (logical -> absolute) --------------------
static uint16_t map_x(int log_x) {
    if (log_x < 0) log_x = 0;
    if (log_x > g_disp_width) log_x = g_disp_width;
    return (uint16_t)(((uint64_t)log_x * HID_ABS_MAX) / (uint64_t)g_disp_width);
}
static uint16_t map_y(int log_y) {
    if (log_y < 0) log_y = 0;
    if (log_y > g_disp_height) log_y = g_disp_height;
    return (uint16_t)(((uint64_t)log_y * HID_ABS_MAX) / (uint64_t)g_disp_height);
}

// -------------------- Smooth mouse movement --------------------
static void mouse_move_smooth(int16_t target_x, int16_t target_y,
                              bool jitter, uint8_t strength,
                              int16_t h_min, int16_t h_max,
                              int16_t v_min, int16_t v_max) {
    int start_x = g_last_mouse_x;
    int start_y = g_last_mouse_y;
    int steps = jitter && strength > 0 ? strength : 32;
    if (steps < 2) steps = 2;

    for (int i = 1; i <= steps; i++) {
        if (g_abort) return;
        float t = (float)i / steps;
        int cur_x = start_x + (int)((target_x - start_x) * t);
        int cur_y = start_y + (int)((target_y - start_y) * t);
        if (jitter && i < steps) {
            int h_range = h_max - h_min + 1;
            int v_range = v_max - v_min + 1;
            if (h_range <= 0) h_range = 1;
            if (v_range <= 0) v_range = 1;
            cur_x += (esp_random() % h_range) + h_min;
            cur_y += (esp_random() % v_range) + v_min;
        }
        mouse_report_abs(0, map_x(cur_x), map_y(cur_y));
        vTaskDelay(pdMS_TO_TICKS(8));
    }
    g_last_mouse_x = target_x;
    g_last_mouse_y = target_y;
}

// -------------------- NVS helpers --------------------
static void nvs_save_line(int line) {
    nvs_set_u16(g_nvs_handle, NVS_KEY_LINE, (uint16_t)line);
    nvs_commit(g_nvs_handle);
}
static int nvs_load_line(void) {
    uint16_t line = 1;
    if (nvs_get_u16(g_nvs_handle, NVS_KEY_LINE, &line) == ESP_OK) return (int)line;
    return 1;
}
static void nvs_clear(void) {
    nvs_erase_key(g_nvs_handle, NVS_KEY_LINE);
    nvs_commit(g_nvs_handle);
}

// -------------------- File loading & JSON compilation --------------------
static void free_txt_lines(void) {
    if (!g_txt_lines) return;
    for (int i = 0; i < g_total_lines; i++) free(g_txt_lines[i]);
    free(g_txt_lines);
    g_txt_lines = NULL;
    g_total_lines = 0;
}

static char *read_line_dynamic(FILE *f) {
    if (!f) return NULL;
    size_t cap = 128, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    int ch;
    bool got_any = false;
    while ((ch = fgetc(f)) != EOF) {
        got_any = true;
        if (ch == '\r') continue;
        if (ch == '\n') break;
        if (len + 1 >= cap) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) { free(buf); return NULL; }
            buf = tmp;
        }
        buf[len++] = (char)ch;
    }
    if (!got_any && len == 0) { free(buf); return NULL; }
    buf[len] = '\0';
    return buf;
}

static bool load_txt_file(void) {
    FILE *f = fopen("/spiffs/data.txt", "r");
    if (!f) return false;
    free_txt_lines();

    size_t cap = 16;
    g_txt_lines = calloc(cap, sizeof(char *));
    if (!g_txt_lines) { fclose(f); return false; }
    g_total_lines = 0;

    while (1) {
        char *line = read_line_dynamic(f);
        if (!line) break;
        if (g_total_lines >= (int)cap) {
            size_t new_cap = cap * 2;
            char **tmp = realloc(g_txt_lines, new_cap * sizeof(char *));
            if (!tmp) { free(line); fclose(f); free_txt_lines(); return false; }
            memset(tmp + cap, 0, (new_cap - cap) * sizeof(char *));
            g_txt_lines = tmp;
            cap = new_cap;
        }
        size_t len = strlen(line);
        while (len > 0 && (line[len-1]==' ' || line[len-1]=='\t')) line[--len] = '\0';
        if (len == 0) { free(line); continue; }
        g_txt_lines[g_total_lines++] = line;
    }
    fclose(f);
    if (g_total_lines <= 0) { free_txt_lines(); ESP_LOGW(TAG, "data.txt empty"); return false; }
    ESP_LOGI(TAG, "Loaded %d lines from data.txt", g_total_lines);
    return true;
}

static bool json_get_string(cJSON *obj, const char *key, const char **out) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsString(item) && item->valuestring) { *out = item->valuestring; return true; }
    return false;
}
static bool json_get_int(cJSON *obj, const char *key, int *out) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsNumber(item)) { *out = item->valueint; return true; }
    return false;
}

static bool load_json_compile(void) {
    FILE *f = fopen("/spiffs/script.json", "r");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return false; }
    char *json_str = malloc((size_t)size + 1);
    if (!json_str) { fclose(f); return false; }
    size_t rd = fread(json_str, 1, (size_t)size, f);
    fclose(f);
    json_str[rd] = '\0';

    cJSON *root = cJSON_Parse(json_str);
    free(json_str);
    if (!root) return false;

    g_base_width = 1920; g_base_height = 1080;
    g_disp_width = g_base_width; g_disp_height = g_base_height;
    g_default_pre_delay_ms = 20; g_default_post_delay_ms = 80;
    g_reset_log_x = -10; g_reset_log_y = -10;
    int reset_x = g_reset_log_x, reset_y = g_reset_log_y;

    cJSON *meta = cJSON_GetObjectItem(root, "meta");
    if (meta && cJSON_IsObject(meta)) {
        cJSON *bres = cJSON_GetObjectItem(meta, "base_resolution");
        if (bres && cJSON_IsObject(bres)) {
            int w, h;
            if (json_get_int(bres, "width", &w))  g_base_width = (w > 0) ? w : g_base_width;
            if (json_get_int(bres, "height", &h)) g_base_height = (h > 0) ? h : g_base_height;
            g_disp_width = g_base_width; g_disp_height = g_base_height;
        }
        int v;
        if (json_get_int(meta, "default_pre_delay_ms", &v) && v >= 0) g_default_pre_delay_ms = v;
        if (json_get_int(meta, "default_post_delay_ms", &v) && v >= 0) g_default_post_delay_ms = v;

        cJSON *loop = cJSON_GetObjectItem(meta, "loop");
        if (loop && cJSON_IsObject(loop)) {
            cJSON *rp = cJSON_GetObjectItem(loop, "reset_position");
            if (rp && cJSON_IsObject(rp)) {
                if (json_get_int(rp, "x", &v)) { g_reset_log_x = v; reset_x = v; }
                if (json_get_int(rp, "y", &v)) { g_reset_log_y = v; reset_y = v; }
            }
        }
    }

    cJSON *steps = cJSON_GetObjectItem(root, "steps");
    if (!steps || !cJSON_IsArray(steps)) { cJSON_Delete(root); return false; }
    int num = cJSON_GetArraySize(steps);
    free(g_steps);
    g_steps = calloc((size_t)num + 2, sizeof(step_t));
    if (!g_steps) { cJSON_Delete(root); return false; }

    int cnt = 0;
    for (int i = 0; i < num; i++) {
        cJSON *s = cJSON_GetArrayItem(steps, i);
        if (!s || !cJSON_IsObject(s)) continue;
        cJSON *act = cJSON_GetObjectItem(s, "action");
        if (!act || !cJSON_IsObject(act)) continue;
        const char *type = NULL;
        if (!json_get_string(act, "type", &type)) continue;

        step_t *st = &g_steps[cnt];
        memset(st, 0, sizeof(*st));
        st->pre_delay_ms = 20; st->post_delay_ms = 80;
        const char *ui = NULL;
        if (json_get_string(s, "ui_hint", &ui)) ascii_sanitize_copy(st->ui_hint, sizeof(st->ui_hint), ui);
        int tmp = 0;
        if (json_get_int(s, "pre_delay_ms", &tmp) && tmp >= 0) st->pre_delay_ms = (uint16_t)tmp;
        if (json_get_int(s, "post_delay_ms", &tmp) && tmp >= 0) st->post_delay_ms = (uint16_t)tmp;

        if (strcmp(type, "mouse_move") == 0) {
            st->type = ACT_MOUSE_MOVE;
            json_get_int(act, "x", (int*)&st->params.move.x);
            json_get_int(act, "y", (int*)&st->params.move.y);
            cJSON *j = cJSON_GetObjectItem(act, "jitter_enabled");
            st->params.move.jitter_enabled = (j && cJSON_IsTrue(j));
            if (st->params.move.jitter_enabled) {
                st->params.move.jitter_strength = 10;
                json_get_int(act, "jitter_strength", (int*)&st->params.move.jitter_strength);
                st->params.move.h_min = -5; st->params.move.h_max = 5;
                st->params.move.v_min = -5; st->params.move.v_max = 5;
                int v;
                if (json_get_int(act, "h_min", &v)) st->params.move.h_min = (int16_t)v;
                if (json_get_int(act, "h_max", &v)) st->params.move.h_max = (int16_t)v;
                if (json_get_int(act, "v_min", &v)) st->params.move.v_min = (int16_t)v;
                if (json_get_int(act, "v_max", &v)) st->params.move.v_max = (int16_t)v;
            }
        } else if (strcmp(type, "mouse_click") == 0) {
            st->type = ACT_MOUSE_CLICK;
            const char *btn = NULL, *ct = NULL;
            if (json_get_string(act, "button", &btn)) {
                if (strcmp(btn, "RIGHT") == 0) st->params.click.button = 1;
                else if (strcmp(btn, "MIDDLE") == 0) st->params.click.button = 2;
                else st->params.click.button = 0;
            }
            if (json_get_string(act, "click_type", &ct)) {
                if (strcmp(ct, "DOUBLE_CLICK") == 0) st->params.click.click_type = 1;
                else if (strcmp(ct, "PRESS") == 0) st->params.click.click_type = 2;
                else if (strcmp(ct, "RELEASE") == 0) st->params.click.click_type = 3;
                else st->params.click.click_type = 0;
            }
        } else if (strcmp(type, "mouse_drag") == 0) {
            st->type = ACT_MOUSE_DRAG;
            cJSON *start = cJSON_GetObjectItem(act, "start");
            cJSON *end = cJSON_GetObjectItem(act, "end");
            if (start && cJSON_IsObject(start)) {
                int v;
                if (json_get_int(start, "x", &v)) st->params.drag.start_x = (int16_t)v;
                if (json_get_int(start, "y", &v)) st->params.drag.start_y = (int16_t)v;
            }
            if (end && cJSON_IsObject(end)) {
                int v;
                if (json_get_int(end, "x", &v)) st->params.drag.end_x = (int16_t)v;
                if (json_get_int(end, "y", &v)) st->params.drag.end_y = (int16_t)v;
            }
            const char *btn = NULL;
            if (json_get_string(act, "button", &btn))
                st->params.drag.button = (strcmp(btn, "RIGHT") == 0) ? 1 : 0;
        } else if (strcmp(type, "keyboard_input") == 0) {
            st->type = ACT_KEYBOARD_INPUT;
            const char *mode = NULL;
            if (json_get_string(act, "input_mode", &mode) && strcmp(mode, "file") == 0) {
                st->params.keyboard.mode = 0;
                cJSON *li = cJSON_GetObjectItem(act, "line_index");
                if (cJSON_IsString(li) && li->valuestring && strcmp(li->valuestring, "AUTO_INCREMENT") == 0) {
                    st->params.keyboard.line_index = 0xFFFF;
                } else {
                    int v = 1;
                    if (cJSON_IsNumber(li)) v = li->valueint;
                    st->params.keyboard.line_index = (uint16_t)((v < 1) ? 1 : v);
                }
            } else {
                st->params.keyboard.mode = 1;
                const char *lit = NULL;
                if (json_get_string(act, "literal_text", &lit)) {
                    strncpy(st->params.keyboard.literal_text, lit, sizeof(st->params.keyboard.literal_text)-1);
                    st->params.keyboard.literal_text[sizeof(st->params.keyboard.literal_text)-1] = '\0';
                }
            }
            // random delay
            cJSON *rmin = cJSON_GetObjectItem(act, "random_delay_min_ms");
            cJSON *rmax = cJSON_GetObjectItem(act, "random_delay_max_ms");
            if (rmin && rmax && cJSON_IsNumber(rmin) && cJSON_IsNumber(rmax)) {
                st->params.keyboard.random_delay = true;
                st->params.keyboard.rand_min_ms = (uint16_t)rmin->valueint;
                st->params.keyboard.rand_max_ms = (uint16_t)rmax->valueint;
            } else {
                st->params.keyboard.random_delay = false;
            }
        } else if (strcmp(type, "keyboard_hotkey") == 0) {
            st->type = ACT_KEYBOARD_HOTKEY;
            cJSON *mods = cJSON_GetObjectItem(act, "modifiers");
            if (mods && cJSON_IsArray(mods)) {
                uint8_t mask = 0;
                for (int j = 0; j < cJSON_GetArraySize(mods); j++) {
                    cJSON *m = cJSON_GetArrayItem(mods, j);
                    if (m && cJSON_IsString(m)) mask |= modifier_name_to_mask(m->valuestring);
                }
                st->params.hotkey.modifier_mask = mask;
            }
            const char *key = NULL;
            if (json_get_string(act, "key", &key) && key[0]) {
                hid_key_t hk = ascii_to_hid(key[0]);
                st->params.hotkey.key_code = hk.key;
            }
        } else if (strcmp(type, "wait") == 0) {
            st->type = ACT_WAIT;
            int v = 0;
            if (json_get_int(act, "duration_ms", &v)) st->params.wait_duration = (uint32_t)(v < 0 ? 0 : v);
        } else {
            continue;
        }
        cnt++;
    }

    // append LOOP_RESET if enabled
    bool loop_enabled = false;
    bool reset_after_each = true;
    cJSON *meta_loop = meta ? cJSON_GetObjectItem(meta, "loop") : NULL;
    if (meta_loop && cJSON_IsObject(meta_loop)) {
        cJSON *en = cJSON_GetObjectItem(meta_loop, "enabled");
        if (en && cJSON_IsBool(en)) loop_enabled = cJSON_IsTrue(en);
        cJSON *ra = cJSON_GetObjectItem(meta_loop, "reset_position_after_each_loop");
        if (ra && cJSON_IsBool(ra)) reset_after_each = cJSON_IsTrue(ra);
    }
    if (loop_enabled && reset_after_each) {
        step_t *reset = &g_steps[cnt++];
        memset(reset, 0, sizeof(*reset));
        reset->type = ACT_LOOP_RESET;
        strcpy(reset->ui_hint, "Loop Reset");
        reset->params.move.x = (int16_t)reset_x;
        reset->params.move.y = (int16_t)reset_y;
    }

    g_step_count = cnt;
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Compiled %d steps", g_step_count);
    return g_step_count > 0;
}

static void ascii_sanitize_copy(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t di = 0;
    for (size_t si = 0; src[si] && di+1 < dst_size; si++) {
        unsigned char ch = (unsigned char)src[si];
        if (ch >= 0x20 && ch <= 0x7E) dst[di++] = (char)ch;
        else if (ch == '\t' || ch == '\n' || ch == '\r') dst[di++] = ' ';
        else dst[di++] = '?';
    }
    dst[di] = '\0';
}

static const char *safe_line_text(int line_1based) {
    if (!g_txt_lines || g_total_lines <= 0) return "";
    if (line_1based < 1 || line_1based > g_total_lines) return "";
    return g_txt_lines[line_1based - 1] ? g_txt_lines[line_1based - 1] : "";
}

static void update_ui(ui_mode_t mode, int cur, int total, const char *line, const char *hint) {
    ui_msg_t msg = { .mode = mode, .cur = cur, .total = total };
    ascii_sanitize_copy(msg.line_content, sizeof(msg.line_content), line);
    ascii_sanitize_copy(msg.hint, sizeof(msg.hint), hint);
    if (ui_queue) xQueueSend(ui_queue, &msg, 0);
}

static void ui_task(void *arg) {
    ui_msg_t msg;
    char l1[24], l2[24], l3[24], l4[24];
    while (1) {
        if (xQueueReceive(ui_queue, &msg, portMAX_DELAY) == pdTRUE) {
            ch1116_clear();
            snprintf(l1, sizeof(l1), "Prog:%03d/%03d", msg.cur, msg.total);
            snprintf(l2, sizeof(l2), "Line:%.16s", msg.line_content);
            snprintf(l3, sizeof(l3), "Act: %.14s", msg.hint);
            const char *m = "IDLE";
            if (msg.mode == UI_STEP) m = "STEP";
            else if (msg.mode == UI_AUTO) m = "AUTO";
            else if (msg.mode == UI_RESET) m = "RESET";
            else if (msg.mode == UI_ERROR) m = "ERROR";
            snprintf(l4, sizeof(l4), "Mode:%s", m);
            ch1116_draw_string(0, 0, l1);
            ch1116_draw_string(0, 16, l2);
            ch1116_draw_string(0, 32, l3);
            ch1116_draw_string(0, 48, l4);
            ch1116_update();
        }
    }
}

// -------------------- Step execution --------------------
static void execute_step(step_t *st) {
    if (!st) return;
    if (st->pre_delay_ms) vTaskDelay(pdMS_TO_TICKS(st->pre_delay_ms));
    const char *line_text = safe_line_text(g_current_line);
    update_ui(g_mode, g_current_line, g_total_lines, line_text, st->ui_hint);

    switch (st->type) {
        case ACT_MOUSE_MOVE:
            mouse_move_smooth(st->params.move.x, st->params.move.y,
                              st->params.move.jitter_enabled, st->params.move.jitter_strength,
                              st->params.move.h_min, st->params.move.h_max,
                              st->params.move.v_min, st->params.move.v_max);
            break;
        case ACT_MOUSE_CLICK: {
            uint8_t btn = (st->params.click.button == 0) ? 0x01 : (st->params.click.button == 1) ? 0x02 : 0x04;
            uint16_t cx = map_x(g_last_mouse_x), cy = map_y(g_last_mouse_y);
            if (st->params.click.click_type == 0) {
                mouse_report_abs(btn, cx, cy); vTaskDelay(pdMS_TO_TICKS(12));
                mouse_report_abs(0, cx, cy);
            } else if (st->params.click.click_type == 1) {
                mouse_report_abs(btn, cx, cy); vTaskDelay(pdMS_TO_TICKS(12));
                mouse_report_abs(0, cx, cy);
                vTaskDelay(pdMS_TO_TICKS(70));
                mouse_report_abs(btn, cx, cy); vTaskDelay(pdMS_TO_TICKS(12));
                mouse_report_abs(0, cx, cy);
            } else if (st->params.click.click_type == 2) {
                mouse_report_abs(btn, cx, cy);
            } else {
                mouse_report_abs(0, cx, cy);
            }
            break;
        }
        case ACT_MOUSE_DRAG: {
            uint8_t btn = (st->params.drag.button == 0) ? 0x01 : 0x02;
            int steps = 30;
            for (int i = 0; i <= steps; i++) {
                if (g_abort) return;
                int curx = st->params.drag.start_x + (st->params.drag.end_x - st->params.drag.start_x) * i / steps;
                int cury = st->params.drag.start_y + (st->params.drag.end_y - st->params.drag.start_y) * i / steps;
                mouse_report_abs(btn, map_x(curx), map_y(cury));
                vTaskDelay(pdMS_TO_TICKS(8));
            }
            mouse_report_abs(0, map_x(st->params.drag.end_x), map_y(st->params.drag.end_y));
            g_last_mouse_x = st->params.drag.end_x;
            g_last_mouse_y = st->params.drag.end_y;
            break;
        }
        case ACT_KEYBOARD_INPUT:
            if (st->params.keyboard.mode == 0) {
                int line_idx = (st->params.keyboard.line_index == 0xFFFF) ? (g_current_line - 1) : (int)(st->params.keyboard.line_index - 1);
                if (line_idx >= 0 && line_idx < g_total_lines) {
                    keyboard_send_text(g_txt_lines[line_idx],
                                       st->params.keyboard.random_delay,
                                       st->params.keyboard.rand_min_ms,
                                       st->params.keyboard.rand_max_ms);
                    if (st->params.keyboard.line_index == 0xFFFF) {
                        g_current_line++;
                        if (g_total_lines > 0 && g_current_line > g_total_lines) {
                            g_current_line = 1;
                            nvs_clear();
                        } else {
                            nvs_save_line(g_current_line);
                        }
                    }
                }
            } else {
                keyboard_send_text(st->params.keyboard.literal_text,
                                   st->params.keyboard.random_delay,
                                   st->params.keyboard.rand_min_ms,
                                   st->params.keyboard.rand_max_ms);
            }
            break;
        case ACT_KEYBOARD_HOTKEY: {
            uint8_t report[8] = { st->params.hotkey.modifier_mask, 0x00, st->params.hotkey.key_code, 0,0,0,0,0 };
            hid_wait_ready();
            tud_hid_report(2, report, sizeof(report));
            vTaskDelay(pdMS_TO_TICKS(15));
            uint8_t rel[8] = {0};
            hid_wait_ready();
            tud_hid_report(2, rel, sizeof(rel));
            break;
        }
        case ACT_WAIT:
            vTaskDelay(pdMS_TO_TICKS(st->params.wait_duration));
            break;
        case ACT_LOOP_RESET:
            mouse_move_smooth(st->params.move.x, st->params.move.y, false, 0, 0,0,0,0);
            g_last_mouse_x = st->params.move.x;
            g_last_mouse_y = st->params.move.y;
            break;
    }
    if (st->post_delay_ms) vTaskDelay(pdMS_TO_TICKS(st->post_delay_ms));
}

static void run_loop(bool single_cycle) {
    if (!g_steps || g_step_count <= 0 || g_total_lines <= 0) return;
    if (g_current_line < 1) g_current_line = 1;

    g_running = true;
    g_abort = false;
    do {
        for (int i = 0; i < g_step_count; i++) {
            // ---- 新增：非阻塞检查紧急命令 ----
            exec_cmd_t pending_cmd;
            if (xQueueReceive(exec_queue, &pending_cmd, 0) == pdTRUE) {
                if (pending_cmd == CMD_ABORT) {
                    g_abort = true;
                    g_running = false;
                    return;
                } else if (pending_cmd == CMD_RESET_ALL) {
                    // 立即执行重置流程（不重启，由外部处理重启）
                    g_abort = true;
                    g_running = false;
                    g_current_line = 1;
                    nvs_clear();
                    mouse_report_abs(0, 0, 0);
                    g_last_mouse_x = 0;
                    g_last_mouse_y = 0;
                    g_mode = UI_RESET;
                    update_ui(UI_RESET, 1, g_total_lines, "", "Reset done");
                    vTaskDelay(pdMS_TO_TICKS(150));
                    esp_restart();          // 重启设备，彻底结束当前循环
                    return;                // 不会执行到这里
                }
                // 如果是其他命令（如STEP），这里不处理，丢弃即可
            }
            // ---------------------------------

            if (g_abort) { g_running = false; return; }
            if (g_current_line > g_total_lines) {
                g_mode = UI_IDLE;
                update_ui(UI_IDLE, g_current_line, g_total_lines, "", "Out of range");
                g_running = false;
                return;
            }
            execute_step(&g_steps[i]);
            vTaskDelay(pdMS_TO_TICKS(4));
        }
    } while (!single_cycle && g_running && !g_abort);
    g_running = false;
}

//屎山，为了避免隐式声明错误，需要在 exec_task 之前添加函数原型，懒得将整个 show_network_info 函数定义移到 exec_task 之前去了
static void show_network_info(void);

// -------------------- Execution task --------------------
static void exec_task(void *arg) {
    exec_cmd_t cmd;
    while (1) {
        if (xQueueReceive(exec_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;
        switch (cmd) {
            case CMD_STEP_ONCE:
                g_mode = UI_STEP;
                update_ui(UI_STEP, g_current_line, g_total_lines, safe_line_text(g_current_line), "Step");
                run_loop(true);
                g_mode = UI_IDLE;
                update_ui(UI_IDLE, g_current_line, g_total_lines, safe_line_text(g_current_line), "Done");
                break;
            case CMD_AUTO_START:
                g_mode = UI_AUTO;
                update_ui(UI_AUTO, g_current_line, g_total_lines, safe_line_text(g_current_line), "Auto");
                run_loop(false);
                g_mode = UI_IDLE;
                update_ui(UI_IDLE, g_current_line, g_total_lines, safe_line_text(g_current_line), "Stopped");
                break;
            case CMD_RESET_ALL:
                g_abort = true;
                g_running = false;
                g_mode = UI_RESET;
                g_current_line = 1;
                nvs_clear();
                mouse_report_abs(0, 0, 0);
                g_last_mouse_x = 0; g_last_mouse_y = 0;
                update_ui(UI_RESET, g_current_line, g_total_lines, safe_line_text(g_current_line), "Reset done");
                vTaskDelay(pdMS_TO_TICKS(150));
                esp_restart();
                break;
            case CMD_SHOW_INFO:
                show_network_info();
                break;    
            case CMD_ABORT:
                g_abort = true;
                g_running = false;
                g_mode = UI_IDLE;
                update_ui(UI_IDLE, g_current_line, g_total_lines, safe_line_text(g_current_line), "Abort");
                break;
            default: break;
        }
    }
}

// -------------------- Button task --------------------
static void send_exec_cmd(exec_cmd_t cmd) {
    if (!exec_queue) return;
    xQueueSend(exec_queue, &cmd, 0);
}

static void button_task(void *arg) {
    bool last = true;
    TickType_t press = 0;
    bool waiting_double = false;
    TickType_t first_short_tick = 0;

    // 三击相关变量已定义在全局

    while (1) {
        bool level = gpio_get_level(BOOT_BUTTON_GPIO);
        TickType_t now = xTaskGetTickCount();

        if (last && !level) {
            press = now;
        } else if (!last && level) {
            uint32_t dur_ms = (now - press) * portTICK_PERIOD_MS;

            if (dur_ms > 2000) {
                // 长按复位（清除所有状态）
                waiting_double = false;
                waiting_triple = false;
                send_exec_cmd(CMD_RESET_ALL);
            } else if (dur_ms > 30) {
                // 短按
                if (!waiting_double && !waiting_triple) {
                    // 第一次短按
                    waiting_double = true;
                    first_short_tick = now;
                } else if (waiting_double && !waiting_triple) {
                    // 第二次短按
                    if ((now - first_short_tick) * portTICK_PERIOD_MS <= 500) {
                        waiting_double = false;
                        waiting_triple = true;
                        second_short_tick = now;
                    } else {
                        // 超时，重新开始等待第一次短按
                        waiting_double = true;
                        first_short_tick = now;
                    }
                } else if (waiting_triple) {
                    // 第三次短按
                    if ((now - second_short_tick) * portTICK_PERIOD_MS <= 300) {
                        waiting_triple = false;
                        waiting_double = false;
                        send_exec_cmd(CMD_SHOW_INFO);
                    } else {
                        // 超时，重新开始等待第一次短按
                        waiting_triple = false;
                        waiting_double = true;
                        first_short_tick = now;
                    }
                }
            }
        }

        // 超时检查：双击等待超时 → 执行单击动作
        if (waiting_double && ((now - first_short_tick) * portTICK_PERIOD_MS > 500)) {
            waiting_double = false;
            send_exec_cmd(CMD_STEP_ONCE);
        }
        // 三击等待超时 → 执行双击动作
        if (waiting_triple && ((now - second_short_tick) * portTICK_PERIOD_MS > 300)) {
            waiting_triple = false;
            send_exec_cmd(CMD_AUTO_START);
        }

        last = level;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
// ==================== 为 Web 服务器提供的导出接口 ====================
// 放在所有静态函数和变量定义之后，app_main 之前

void web_send_exec_cmd(int cmd) {
    send_exec_cmd((exec_cmd_t)cmd);
}

int web_get_current_line(void) {
    return g_current_line;
}

int web_get_total_lines(void) {
    return g_total_lines;
}

int web_get_mode(void) {
    return (int)g_mode;
}

bool web_is_running(void) {
    return g_running;
}

const char *web_safe_line_text(int line) {
    return safe_line_text(line);
}

void web_ascii_sanitize_copy(char *dst, size_t dst_size, const char *src) {
    ascii_sanitize_copy(dst, dst_size, src);
}

void web_reload_txt(void) {
    load_txt_file();
}

void web_reload_json(void) {
    load_json_compile();
}

// ==================== Wi‑Fi 密码管理（读写 NVS） ====================
const char* web_get_wifi_password(void) {
    static char pass[32] = "12345678";
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t len = sizeof(pass);
        nvs_get_str(handle, "wifi_pass", pass, &len);
        nvs_close(handle);
    }
    return pass;
}

void web_set_wifi_password(const char *pass) {
    nvs_handle_t handle;
    nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    nvs_set_str(handle, "wifi_pass", pass);
    nvs_commit(handle);
    nvs_close(handle);
}

const char* web_get_ap_ssid(void) {
    return "ESP-Loop-Actor";
}

const char* web_get_ap_ip(void) {
    return "192.168.4.1";
}


// 三击后显示网络信息（由执行任务调用）
static void show_network_info(void) {
    if (ui_task_handle) vTaskSuspend(ui_task_handle);
    ch1116_clear();
    ch1116_draw_string(0, 0, "IP:192.168.4.1");
    ch1116_draw_string(0, 16, "SSID:ESP-Loop-Actor");
    char pwd[32];
    snprintf(pwd, sizeof(pwd), "PASS:%s", web_get_wifi_password());
    ch1116_draw_string(0, 32, pwd);
    ch1116_draw_string(0, 48, "Triple-Click");
    ch1116_update();
    vTaskDelay(pdMS_TO_TICKS(4000));
    if (ui_task_handle) vTaskResume(ui_task_handle);
    update_ui(g_mode, g_current_line, g_total_lines, safe_line_text(g_current_line), "Info shown");
}

// -------------------- Main --------------------
void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &g_nvs_handle));
    g_current_line = nvs_load_line();

    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&spiffs_conf));

    ch1116_init();
    ch1116_clear();
    ch1116_draw_string(0, 0, "ESP-Loop-Actor");
    ch1116_draw_string(0, 16, "Loading...");
    ch1116_update();

    if (!load_txt_file()) {
        ESP_LOGW(TAG, "Creating default data.txt");
        FILE *f = fopen("/spiffs/data.txt", "w");
        if (f) { fputs("Hello\nWorld\nTest\n", f); fclose(f); }
        if (!load_txt_file()) {
            ch1116_clear(); ch1116_draw_string(0,0,"TXT ERR"); ch1116_update();
            while (1) vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    if (!load_json_compile()) {
        ESP_LOGW(TAG, "Creating default script.json");
        const char *default_json =
            "{"
                "\"meta\":{\"name\":\"default\",\"base_resolution\":{\"width\":1920,\"height\":1080},"
                "\"loop\":{\"enabled\":true,\"reset_position\":{\"x\":-10,\"y\":-10},\"reset_position_after_each_loop\":true}},"
                "\"steps\":["
                    "{\"id\":1,\"ui_hint\":\"move to 200,400\",\"action\":{\"type\":\"mouse_move\",\"x\":200,\"y\":400}},"
                    "{\"id\":2,\"ui_hint\":\"click\",\"action\":{\"type\":\"mouse_click\",\"button\":\"LEFT\",\"click_type\":\"CLICK\"}},"
                    "{\"id\":3,\"ui_hint\":\"type line\",\"action\":{\"type\":\"keyboard_input\",\"input_mode\":\"file\",\"line_index\":\"AUTO_INCREMENT\"}}"
                "]}";
        FILE *f = fopen("/spiffs/script.json", "w");
        if (f) { fputs(default_json, f); fclose(f); }
        if (!load_json_compile()) {
            ch1116_clear(); ch1116_draw_string(0,0,"JSON ERR"); ch1116_update();
            while (1) vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    if (g_current_line < 1) g_current_line = 1;
    if (g_current_line > g_total_lines) {
        ESP_LOGW(TAG, "Persisted line %d > total %d, resetting to 1", g_current_line, g_total_lines);
        nvs_clear();
        g_current_line = 1;
    }

    g_last_mouse_x = 0; g_last_mouse_y = 0;

    ui_queue = xQueueCreate(10, sizeof(ui_msg_t));
    exec_queue = xQueueCreate(4, sizeof(exec_cmd_t));
    // 找到原创建 UI 任务的代码，修改为：
    xTaskCreate(ui_task, "ui", 4096, NULL, 1, &ui_task_handle);
    xTaskCreate(exec_task, "exec", 6144, NULL, 2, NULL);

    update_ui(UI_IDLE, g_current_line, g_total_lines, safe_line_text(g_current_line), "Ready");

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io_conf);
    xTaskCreate(button_task, "btn", 4096, NULL, 3, NULL);

    tinyusb_config_t tcfg = TINYUSB_DEFAULT_CONFIG(NULL);
    tcfg.descriptor.device = &device_desc;
    tcfg.descriptor.string = string_desc;
    tcfg.descriptor.string_count = sizeof(string_desc) / sizeof(string_desc[0]);
    tcfg.descriptor.full_speed_config = hid_configuration;
    tinyusb_driver_install(&tcfg);
    
    //web服务
    start_web_server();
    
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    bool led = false;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));
        led = !led;
        gpio_set_level(LED_GPIO, led);
    }
}