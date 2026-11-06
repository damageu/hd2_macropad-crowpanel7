/*
 * CrowPanel Advance 7: "USB" connection over the board's USB-C serial bridge (CH340K, UART0).
 *
 * The board has no native USB, so it cannot be a USB keyboard itself. Instead it sends
 * keyboard reports as small framed packets over UART0, and the "HD2 Macropad Link" app on
 * the PC turns them into real key presses. Same cable that powers the board.
 *
 * Frame: A5 5A <type> <len> <payload...> <chk>   chk = ~(type + len + sum(payload)) & 0xFF
 *   board -> PC  0x01 KEY   payload: modifier mask, key0..key5  (USB HID boot keyboard report)
 *   board -> PC  0x02 PONG  payload: 'H' 'D' '2' <protocol version>
 *   PC -> board  0x81 PING  payload: none
 * Anything else on the line (boot messages, logs) is ignored by the app.
 * While this link is active, ESP log output is switched off so it can't interleave with frames.
 *
 * Replaces usb_controller.c (TinyUSB) on this board; same four functions.
 *
 * UART0 stays at 115200. That is the Link protocol rate. Esptool flash baud
 * is a separate setting and must be 460800 or lower on this PC.
 */

#ifdef BOARD_CROWPANEL_ADV7

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "usb_controller.h"

extern void updateConnection();
extern bool bsp_display_lock(uint32_t timeout_ms);
extern void bsp_display_unlock(void);

#define LINK_UART           UART_NUM_0
#define LINK_PROTO_VERSION  1
#define LINK_TIMEOUT_US     (5000 * 1000)   /* cp7.2: was 2.5 s (only drives the UI icon now) */

#define T_KEY   0x01
#define T_PONG  0x02
#define T_PING  0x81

static bool driver_ready = false;
static volatile bool link_active = false;
static volatile int64_t last_ping_us = 0;
static bool last_connected = false;
static SemaphoreHandle_t tx_mutex = NULL;
static TaskHandle_t rx_task = NULL;

static void send_frame(uint8_t type, const uint8_t *payload, uint8_t len)
{
    uint8_t buf[4 + 16 + 1];
    if (len > 16) {
        return;
    }
    uint8_t sum = type + len;
    buf[0] = 0xA5;
    buf[1] = 0x5A;
    buf[2] = type;
    buf[3] = len;
    for (uint8_t i = 0; i < len; i++) {
        buf[4 + i] = payload[i];
        sum += payload[i];
    }
    buf[4 + len] = (uint8_t)~sum;

    if (tx_mutex) {
        xSemaphoreTake(tx_mutex, portMAX_DELAY);
    }
    uart_write_bytes(LINK_UART, (const char *)buf, 5 + len);
    if (tx_mutex) {
        xSemaphoreGive(tx_mutex);
    }
}

static void refresh_ui_if_changed(void)
{
    bool now = usb_connected();
    if (now != last_connected) {
        last_connected = now;
        if (bsp_display_lock(200)) {
            updateConnection();
            bsp_display_unlock();
        }
    }
}

/* Reads PINGs from the PC app, answers with PONG, tracks whether the app is alive */
static void link_rx_task(void *arg)
{
    uint8_t b;
    int state = 0;
    uint8_t type = 0, len = 0, idx = 0, sum = 0;

    while (1) {
        int n = uart_read_bytes(LINK_UART, &b, 1, pdMS_TO_TICKS(500));
        if (n == 1 && link_active) {
            switch (state) {
            case 0: state = (b == 0xA5) ? 1 : 0; break;
            case 1: state = (b == 0x5A) ? 2 : (b == 0xA5 ? 1 : 0); break;
            case 2: type = b; sum = b; state = 3; break;
            case 3:
                len = b; sum += b; idx = 0;
                state = (len > 16) ? 0 : (len == 0 ? 5 : 4);
                break;
            case 4:
                sum += b;
                if (++idx >= len) {
                    state = 5;
                }
                break;
            case 5:
                if ((uint8_t)~sum == b && type == T_PING) {
                    last_ping_us = esp_timer_get_time();
                    const uint8_t pong[4] = {'H', 'D', '2', LINK_PROTO_VERSION};
                    send_frame(T_PONG, pong, sizeof(pong));
                }
                state = 0;
                break;
            }
        }
        if (link_active) {
            refresh_ui_if_changed();
        }
    }
}

/* cp7.2: true whenever Connectivity = USB, regardless of PING timing */
bool usb_link_active(void)
{
    return link_active;
}

bool usb_connected()
{
    return link_active && last_ping_us != 0 &&
           (esp_timer_get_time() - last_ping_us) < LINK_TIMEOUT_US;
}

void usb_keyboard_send(uint8_t special_key_mask, uint8_t keyboard_cmd, uint8_t num_key)
{
    if (!link_active) {
        return;
    }
    uint8_t report[7] = {special_key_mask, 0, 0, 0, 0, 0, 0};
    if (num_key > 0) {
        report[1] = keyboard_cmd;
    }
    send_frame(T_KEY, report, sizeof(report));
}

esp_err_t usb_controller_init()
{
    if (!driver_ready) {
        /* UART0 is already set up at 115200 for the console; add the driver for RX + atomic TX */
        esp_err_t ret = uart_driver_install(LINK_UART, 512, 0, 0, NULL, 0);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            return ret;
        }
        uart_set_baudrate(LINK_UART, 115200);
        tx_mutex = xSemaphoreCreateMutex();
        xTaskCreatePinnedToCore(link_rx_task, "serial_link", 3072, NULL, 5, &rx_task, 0);
        driver_ready = true;
    }
    /* Logs share this UART; mute them so they can't split a frame */
    esp_log_level_set("*", ESP_LOG_NONE);
    last_ping_us = 0;
    last_connected = false;
    link_active = true;
    return ESP_OK;
}

esp_err_t usb_controller_deinit()
{
    if (link_active) {
        /* release anything that might be held on the PC side */
        const uint8_t empty[7] = {0};
        send_frame(T_KEY, empty, sizeof(empty));
        uart_wait_tx_done(LINK_UART, pdMS_TO_TICKS(50));
    }
    link_active = false;
    last_ping_us = 0;
    last_connected = false;
    esp_log_level_set("*", ESP_LOG_INFO);
    return ESP_OK;
}

#endif /* BOARD_CROWPANEL_ADV7 */
