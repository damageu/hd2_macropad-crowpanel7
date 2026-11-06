#ifndef _USB_CONTROLLER_H
#define _USB_CONTROLLER_H

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t usb_controller_init();
esp_err_t usb_controller_deinit();
bool usb_connected();
#ifdef BOARD_CROWPANEL_ADV7
bool usb_link_active(void);
#endif
void usb_keyboard_send(uint8_t special_key_mask, uint8_t keyboard_cmd, uint8_t num_key);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
