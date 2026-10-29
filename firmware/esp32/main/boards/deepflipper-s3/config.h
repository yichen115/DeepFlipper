#pragma once
// Reference wiring for a bare ESP32-S3 module. Verify your actual board before flashing.
// These are a new board identity; no existing board's pin map is changed.
#include <driver/gpio.h>
#define DF_UART_TX GPIO_NUM_17
#define DF_UART_RX GPIO_NUM_18
#define DF_MIC_BCLK GPIO_NUM_5
#define DF_MIC_WS GPIO_NUM_4
#define DF_MIC_DATA GPIO_NUM_6
#define DF_SPK_BCLK GPIO_NUM_15
#define DF_SPK_WS GPIO_NUM_16
#define DF_SPK_DATA GPIO_NUM_7
#define DF_LED_R GPIO_NUM_8
#define DF_LED_G GPIO_NUM_9
#define DF_LED_B GPIO_NUM_10
#define DF_TALK_BUTTON GPIO_NUM_0
