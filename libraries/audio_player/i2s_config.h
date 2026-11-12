#pragma once

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"

// I2S Configuration
#define AUDIO_I2S_PORT I2S_NUM_0
#ifdef BOARD_CROWPANEL_ADV7
// CrowPanel Advance 7: NS4168 amp, routed when DIP S1=0, S0=0
#define AUDIO_I2S_MCK_IO -1 // MCK
#define AUDIO_I2S_BCK_IO 5  // BCK
#define AUDIO_I2S_LRCK_IO 6 // LCK
#define AUDIO_I2S_DO_IO 4   // DIN
#else
#define AUDIO_I2S_MCK_IO -1 // MCK
#define AUDIO_I2S_BCK_IO 42 // BCK
#define AUDIO_I2S_LRCK_IO 2   // LCK
#define AUDIO_I2S_DO_IO 41  // DIN
#endif