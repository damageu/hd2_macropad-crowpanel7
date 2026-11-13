/*
 * Board support for the Elecrow CrowPanel Advance 7.0 (DIS02170A, ESP32-S3-WROOM-1-N16R8)
 *
 * CrowPanel port is AI-assisted, rebased onto Erik Lembke's HD2 Macropad
 * (https://github.com/unic8s/hd2_macropad). Evan tests it on real hardware.
 * On-device About credits stay with the original authors.
 * Licence: CC BY-NC-SA 4.0.
 *
 * Replaces esp_bsp.c (JC3248W535: AXS15231B QSPI panel + touch, LEDC backlight).
 *
 * - 800x480 16-bit RGB panel (SC7277), pins/timings per Elecrow LovyanGFX_Driver.h
 * - GT911 touch on I2C0 (SDA 15 / SCL 16), polled
 * - Backlight, touch reset and amplifier owned by the board's companion MCU:
 *     V1.0  : PCA9557/TCA9534 expander @0x18 (IO1 backlight, IO2 touch reset, IO3/IO4 amp)
 *     V1.2  : STC8H1K28 @0x30, backlight 0x05..0x10, touch 0x19, speaker 0x17
 *     V1.3+ : STC8H1K28 @0x30, backlight 0..245 (0 brightest, 245 off), touch 250, amp 248
 *   The revision is detected at boot.
 *
 * The HD2 Macropad UI is designed for 480x320. LVGL renders at 480x320 and the flush
 * callback scales it 1.5x (nearest neighbour) into a centred 720x480 window, with
 * 40 px black borders left and right. Touch coordinates are mapped back the same way.
 * "Flip screen 180" (LV_DISP_ROT_270 in the app config) is honoured in both paths.
 */

#ifdef BOARD_CROWPANEL_ADV7

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "lvgl.h"
#include "esp_bsp.h"
#include "display.h"
#include "lv_port.h"

static const char *TAG = "CrowPanelAdv7";

/* ---------------------------------------------------------------- pins / timings */

#define CP_I2C_NUM          I2C_NUM_0
#define CP_I2C_SDA          GPIO_NUM_15
#define CP_I2C_SCL          GPIO_NUM_16
#define CP_I2C_HZ           400000
#define CP_TOUCH_INT        GPIO_NUM_1

#define CP_LCD_W            800
#define CP_LCD_H            480
#define CP_PCLK_HZ          (16 * 1000 * 1000)   /* V1.3+ value; also fine for V1.0/V1.2 (21 MHz there) */

/* Logical UI resolution and scaling window */
#define UI_W                480
#define UI_H                320
#define WIN_X               40                   /* (800 - 720) / 2 */
#define WIN_Y               0
#define WIN_W               720
#define WIN_H               480

#define ADDR_STC8           0x30
#define ADDR_EXPANDER       0x18
#define ADDR_GT911_A        0x5D
#define ADDR_GT911_B        0x14

typedef enum {
    REV_UNKNOWN = 0,
    REV_V10,        /* expander */
    REV_V12,        /* STC8, old command set */
    REV_V13,        /* STC8, new command set (V1.3, V1.4, V1.5) */
} cp_rev_t;

static cp_rev_t board_rev = REV_UNKNOWN;
static bool i2c_ready = false;
static bool flip180 = false;
static int  brightness_pct = 100;
static uint8_t expander_out = 0x00;

static esp_lcd_panel_handle_t panel = NULL;
static lv_disp_t *disp = NULL;
static lv_indev_t *indev = NULL;
static lv_disp_drv_t disp_drv;
static lv_disp_draw_buf_t draw_buf;
static lv_indev_drv_t indev_drv;
static uint16_t *scale_buf = NULL;
static size_t scale_buf_px = 0;

static uint8_t gt911_addr = 0;

/* ---------------------------------------------------------------- I2C helpers */

esp_err_t bsp_i2c_init(void)
{
    if (i2c_ready) {
        return ESP_OK;
    }
    const i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = CP_I2C_SDA,
        .scl_io_num = CP_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = CP_I2C_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(CP_I2C_NUM, &conf), TAG, "i2c config");
    ESP_RETURN_ON_ERROR(i2c_driver_install(CP_I2C_NUM, conf.mode, 0, 0, 0), TAG, "i2c install");
    i2c_ready = true;
    return ESP_OK;
}

esp_err_t bsp_i2c_deinit(void)
{
    if (!i2c_ready) {
        return ESP_OK;
    }
    i2c_ready = false;
    return i2c_driver_delete(CP_I2C_NUM);
}

/* Address-only probe: START, addr+W, STOP. Sends no data byte, so it cannot trigger an STC8 command. */
static bool i2c_probe(uint8_t addr)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(CP_I2C_NUM, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return ret == ESP_OK;
}

static esp_err_t stc8_cmd(uint8_t value)
{
    esp_err_t ret = ESP_FAIL;
    for (int i = 0; i < 3 && ret != ESP_OK; i++) {
        ret = i2c_master_write_to_device(CP_I2C_NUM, ADDR_STC8, &value, 1, pdMS_TO_TICKS(50));
        if (ret != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "STC8 cmd %u failed: %s", value, esp_err_to_name(ret));
    }
    return ret;
}

/* PCA9557/TCA9534: reg 1 = output, reg 3 = config (0 = output) */
static esp_err_t expander_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_write_to_device(CP_I2C_NUM, ADDR_EXPANDER, buf, 2, pdMS_TO_TICKS(50));
}

static void expander_set(uint8_t bit, bool level)
{
    if (level) {
        expander_out |= (1 << bit);
    } else {
        expander_out &= ~(1 << bit);
    }
    expander_write(0x01, expander_out);
}

/* ---------------------------------------------------------------- touch (GT911) */

static esp_err_t gt911_read(uint16_t reg, uint8_t *data, size_t len)
{
    uint8_t r[2] = {reg >> 8, reg & 0xFF};
    return i2c_master_write_read_device(CP_I2C_NUM, gt911_addr, r, 2, data, len, pdMS_TO_TICKS(20));
}

static esp_err_t gt911_write_u8(uint16_t reg, uint8_t val)
{
    uint8_t b[3] = {reg >> 8, reg & 0xFF, val};
    return i2c_master_write_to_device(CP_I2C_NUM, gt911_addr, b, 3, pdMS_TO_TICKS(20));
}

static uint8_t gt911_find(void)
{
    if (i2c_probe(ADDR_GT911_A)) {
        return ADDR_GT911_A;
    }
    if (i2c_probe(ADDR_GT911_B)) {
        return ADDR_GT911_B;
    }
    return 0;
}

/* Hold INT low across the reset so the GT911 latches 0x5D, then release INT to input. */
static void touch_reset_with(void (*do_reset)(void))
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CP_TOUCH_INT,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(CP_TOUCH_INT, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    do_reset();
    vTaskDelay(pdMS_TO_TICKS(120));
    io.mode = GPIO_MODE_INPUT;
    gpio_config(&io);
    vTaskDelay(pdMS_TO_TICKS(50));
}

static void reset_v13(void) { stc8_cmd(250); }
static void reset_v12(void) { stc8_cmd(0x19); }
static void reset_v10(void)
{
    expander_set(2, false);
    vTaskDelay(pdMS_TO_TICKS(20));
    expander_set(2, true);
}

/* ---------------------------------------------------------------- board detection */

/* Send the touch-activate command, then poll for the GT911 for up to ~500 ms; retry up to 3x.
 * Mirrors Elecrow's loop (resend 250 until both 0x30 and 0x5D answer). */
static uint8_t touch_bringup(void (*do_reset)(void))
{
    for (int attempt = 0; attempt < 3; attempt++) {
        touch_reset_with(do_reset);
        for (int t = 0; t < 25; t++) {
            uint8_t a = gt911_find();
            if (a) {
                return a;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    return 0;
}

static void board_detect_and_init(void)
{
    bool has_stc8 = false, has_exp = false;

    /* The STC8 can take a moment after power-up; Elecrow retry until it answers */
    for (int i = 0; i < 20; i++) {
        has_stc8 = i2c_probe(ADDR_STC8);
        has_exp = i2c_probe(ADDR_EXPANDER);
        if (has_stc8 || has_exp) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

#if defined(CP_FORCE_REV) && CP_FORCE_REV == 13
    /* Build pinned to V1.3+ (V1.3 / V1.4 / V1.5 boards): never send V1.2 bytes */
    board_rev = REV_V13;
    gt911_addr = touch_bringup(reset_v13);
#else
    if (has_stc8) {
        /* Current stock is V1.3+; only call it V1.2 if the old touch command is the one
         * that brings the GT911 up after the new one has clearly failed. */
        board_rev = REV_V13;
        gt911_addr = touch_bringup(reset_v13);
        if (!gt911_addr) {
            touch_reset_with(reset_v12);
            gt911_addr = gt911_find();
            if (gt911_addr) {
                board_rev = REV_V12;
            }
        }
    } else if (has_exp) {
        board_rev = REV_V10;
        expander_out = 0x00;
        expander_write(0x03, 0x00);         /* all outputs */
        expander_write(0x01, expander_out);
        gt911_addr = touch_bringup(reset_v10);
    } else {
        ESP_LOGE(TAG, "Neither STC8 (0x30) nor expander (0x18) found");
        board_rev = REV_V13;
        gt911_addr = gt911_find();
    }
#endif

    ESP_LOGI(TAG, "Board revision: %s (stc8=%d exp=%d), GT911 at 0x%02X",
             board_rev == REV_V10 ? "V1.0" : board_rev == REV_V12 ? "V1.2" : "V1.3+",
             has_stc8, has_exp, gt911_addr);
}

/* Speaker amplifier gate, called around each sound (only audible with DIP S1=0, S0=0) */
void bsp_audio_amp(bool on)
{
    if (!i2c_ready) {
        return;
    }
    switch (board_rev) {
    case REV_V13: stc8_cmd(on ? 248 : 249); break;
    case REV_V12: if (on) stc8_cmd(0x17); break;
    case REV_V10: expander_set(3, on); expander_set(4, on); break;
    default: break;
    }
}

/* ---------------------------------------------------------------- backlight */

static esp_err_t backlight_apply(int pct, bool on)
{
    if (!i2c_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    switch (board_rev) {
    case REV_V13: {
        /* 0 brightest ... 244 dimmest, 245 off */
        uint8_t v = on ? (uint8_t)(244 - (pct * 244) / 100) : 245;
        return stc8_cmd(v);
    }
    case REV_V12: {
        /* 0x05 off ... 0x10 brightest; send 0x10 first, then the level */
        if (!on) {
            return stc8_cmd(0x05);
        }
        uint8_t v = 0x06 + (uint8_t)((pct * (0x10 - 0x06)) / 100);
        stc8_cmd(0x10);
        return v == 0x10 ? ESP_OK : stc8_cmd(v);
    }
    case REV_V10:
        expander_set(1, on && pct > 0);    /* on/off only */
        return ESP_OK;
    default:
        return ESP_ERR_INVALID_STATE;
    }
}

esp_err_t bsp_display_brightness_set(int brightness_percent)
{
    brightness_pct = brightness_percent;
    return backlight_apply(brightness_pct, true);
}

esp_err_t bsp_display_backlight_off(void)
{
    return backlight_apply(brightness_pct, false);
}

esp_err_t bsp_display_backlight_on(void)
{
    return backlight_apply(brightness_pct, true);
}

/* ---------------------------------------------------------------- RGB panel */

static esp_err_t panel_init(void)
{
    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = CP_PCLK_HZ,
            .h_res = CP_LCD_W,
            .v_res = CP_LCD_H,
            .hsync_front_porch = 8,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .vsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .flags.pclk_active_neg = 1,
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 1,
        .bounce_buffer_size_px = CP_LCD_W * 10,
        .sram_trans_align = 4,
        .psram_trans_align = 64,
        .hsync_gpio_num = 40,
        .vsync_gpio_num = 41,
        .de_gpio_num = 42,
        .pclk_gpio_num = 39,
        .disp_gpio_num = GPIO_NUM_NC,
        .data_gpio_nums = {
            21, 47, 48, 45, 38,         /* B0..B4 */
            9, 10, 11, 12, 13, 14,      /* G0..G5 */
            7, 17, 18, 3, 46,           /* R0..R4 */
        },
        .flags.fb_in_psram = 1,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&cfg, &panel), TAG, "new rgb panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "panel init");

    /* Clear the whole frame buffer to black (borders stay black for good) */
    const int rows = 40;
    uint16_t *black = heap_caps_calloc(CP_LCD_W * rows, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (black) {
        for (int y = 0; y < CP_LCD_H; y += rows) {
            esp_lcd_panel_draw_bitmap(panel, 0, y, CP_LCD_W, y + rows, black);
        }
        free(black);
    }
    return ESP_OK;
}

/* ---------------------------------------------------------------- LVGL glue */

/* First destination pixel covering source index s, for the 2:3 nearest-neighbour map d -> (2d)/3 */
static inline int dst_lo(int s) { return (3 * s + 1) / 2; }

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    const int sw = area->x2 - area->x1 + 1;

    /* Source area in "unflipped" UI coordinates */
    int vx1 = area->x1, vx2 = area->x2, vy1 = area->y1, vy2 = area->y2;
    if (flip180) {
        vx1 = UI_W - 1 - area->x2;
        vx2 = UI_W - 1 - area->x1;
        vy1 = UI_H - 1 - area->y2;
        vy2 = UI_H - 1 - area->y1;
    }

    const int dx1 = dst_lo(vx1), dx2 = dst_lo(vx2 + 1) - 1;
    const int dy1 = dst_lo(vy1), dy2 = dst_lo(vy2 + 1) - 1;
    const int dw = dx2 - dx1 + 1, dh = dy2 - dy1 + 1;

    if ((size_t)(dw * dh) > scale_buf_px) {
        ESP_LOGE(TAG, "flush area too large (%dx%d)", dw, dh);
        lv_disp_flush_ready(drv);
        return;
    }

    const uint16_t *src = (const uint16_t *)color_map;
    uint16_t *out = scale_buf;

    for (int dy = dy1; dy <= dy2; dy++) {
        int vy = (2 * dy) / 3;
        int ly = flip180 ? (UI_H - 1 - vy) : vy;
        const uint16_t *row = src + (ly - area->y1) * sw;
        for (int dx = dx1; dx <= dx2; dx++) {
            int vx = (2 * dx) / 3;
            int lx = flip180 ? (UI_W - 1 - vx) : vx;
            uint16_t c = row[lx - area->x1];
#if LV_COLOR_16_SWAP
            c = (uint16_t)((c >> 8) | (c << 8));   /* LVGL renders byte-swapped; RGB bus wants native */
#endif
            *out++ = c;
        }
    }

    esp_lcd_panel_draw_bitmap(panel, WIN_X + dx1, WIN_Y + dy1, WIN_X + dx2 + 1, WIN_Y + dy2 + 1, scale_buf);
    lv_disp_flush_ready(drv);
}

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    static lv_coord_t last_x = 0, last_y = 0;
    static lv_indev_state_t last_state = LV_INDEV_STATE_RELEASED;
    static uint32_t last_retry = 0;
    static int retries = 0;

    if (!gt911_addr) {
        /* Touch never came up: retry the bring-up about once a second, a few times */
        uint32_t now = lv_tick_get();
        if (retries < 5 && now - last_retry > 1000) {
            last_retry = now;
            retries++;
            touch_reset_with(board_rev == REV_V10 ? reset_v10 : board_rev == REV_V12 ? reset_v12 : reset_v13);
            gt911_addr = gt911_find();
            ESP_LOGW(TAG, "Touch retry %d: GT911 %s", retries, gt911_addr ? "found" : "missing");
        }
        last_state = LV_INDEV_STATE_RELEASED;
    } else {
        uint8_t status = 0;
        if (gt911_read(0x814E, &status, 1) != ESP_OK) {
            last_state = LV_INDEV_STATE_RELEASED;      /* never stick pressed on a bus error */
        } else if (status & 0x80) {
            /* New frame from the controller */
            uint8_t n = status & 0x0F;
            last_state = LV_INDEV_STATE_RELEASED;
            if (n > 0 && n <= 5) {
                uint8_t p[4];
                if (gt911_read(0x8150, p, 4) == ESP_OK) {
                    int px = p[0] | (p[1] << 8);
                    int py = p[2] | (p[3] << 8);
                    /* Touches on the black side borders are ignored */
                    if (px >= WIN_X && px < WIN_X + WIN_W && py >= WIN_Y && py < WIN_Y + WIN_H) {
                        int vx = ((px - WIN_X) * 2) / 3;
                        int vy = ((py - WIN_Y) * 2) / 3;
                        if (vx > UI_W - 1) vx = UI_W - 1;
                        if (vy > UI_H - 1) vy = UI_H - 1;
                        if (flip180) {
                            vx = UI_W - 1 - vx;
                            vy = UI_H - 1 - vy;
                        }
                        last_x = vx;
                        last_y = vy;
                        last_state = LV_INDEV_STATE_PRESSED;
                    }
                }
            }
            gt911_write_u8(0x814E, 0);
        }
        /* No new frame: keep reporting the previous state */
    }
    data->point.x = last_x;
    data->point.y = last_y;
    data->state = last_state;
}

/* Backlight off on restart (flip-screen and other restarts), so the panel doesn't show garbage */
static void on_shutdown(void)
{
    backlight_apply(brightness_pct, false);
}

lv_disp_t *bsp_display_start_with_config(const bsp_display_cfg_t *cfg)
{
    flip180 = (cfg->rotate == LV_DISP_ROT_270);

    if (bsp_i2c_init() != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed");
    } else {
        board_detect_and_init();
        backlight_apply(brightness_pct, false);
        esp_register_shutdown_handler(on_shutdown);
    }

    if (panel_init() != ESP_OK) {
        ESP_LOGE(TAG, "RGB panel init failed");
        return NULL;
    }

    if (lvgl_port_init(&cfg->lvgl_port_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "LVGL port init failed");
        return NULL;
    }

    /* Draw buffer: 40 full-width UI lines, internal RAM preferred */
    const size_t buf_px = UI_W * 40;
    lv_color_t *buf = heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    }
    /* Worst case scaled block: (1.5w+1)(1.5h+1) with w*h <= buf_px and w <= 480 */
    scale_buf_px = (buf_px * 9) / 4 + (3 * (UI_W + UI_H)) / 2 + 4;
    scale_buf = heap_caps_malloc(scale_buf_px * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!buf || !scale_buf) {
        ESP_LOGE(TAG, "Out of memory for display buffers");
        return NULL;
    }

    bsp_display_lock(0);
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, buf_px);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = UI_W;
    disp_drv.ver_res = UI_H;
    disp_drv.flush_cb = flush_cb;
    disp_drv.draw_buf = &draw_buf;
    disp = lv_disp_drv_register(&disp_drv);

    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.disp = disp;
    indev_drv.read_cb = touch_read_cb;
    indev = lv_indev_drv_register(&indev_drv);
    bsp_display_unlock();

    ESP_LOGI(TAG, "Display ready: UI %dx%d scaled to %dx%d, flip=%d", UI_W, UI_H, WIN_W, WIN_H, flip180);
    return disp;
}

lv_indev_t *bsp_display_get_input_dev(void)
{
    return indev;
}

bool bsp_display_lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void bsp_display_unlock(void)
{
    lvgl_port_unlock();
}

#endif /* BOARD_CROWPANEL_ADV7 */
