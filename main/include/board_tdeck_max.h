#ifndef BOARD_TDECK_MAX_H
#define BOARD_TDECK_MAX_H

#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

// ─────────────────────────────────────────────────────────────────────────────
//  System Shared Buses
// ─────────────────────────────────────────────────────────────────────────────
#define BOARD_I2C_SDA           GPIO_NUM_13
#define BOARD_I2C_SCL           GPIO_NUM_14
#define BOARD_I2C_PORT          I2C_NUM_0

#define BOARD_SPI_SCK           GPIO_NUM_36
#define BOARD_SPI_MOSI          GPIO_NUM_33
#define BOARD_SPI_MISO          GPIO_NUM_47

// ─────────────────────────────────────────────────────────────────────────────
//  ES8311 Audio Codec & I2S Pins
// ─────────────────────────────────────────────────────────────────────────────
#define BOARD_ES8311_I2C_ADDR   0x18
#define BOARD_I2S_NUM           I2S_NUM_0
#define BOARD_I2S_MCLK          GPIO_NUM_38
#define BOARD_I2S_SCLK          GPIO_NUM_39 // BCLK
#define BOARD_I2S_ASDOUT        GPIO_NUM_40 // Mic Data (DIN to ESP32)
#define BOARD_I2S_LRCK          GPIO_NUM_18 // WS / Word Select
#define BOARD_I2S_DSDIN         GPIO_NUM_17 // Speaker Data (DOUT from ESP32)

// ─────────────────────────────────────────────────────────────────────────────
//  3.1-inch E-Paper Display (GDEQ031T10)
// ─────────────────────────────────────────────────────────────────────────────
#define BOARD_EPD_CS            GPIO_NUM_34
#define BOARD_EPD_DC            GPIO_NUM_35
#define BOARD_EPD_RST           GPIO_NUM_9
#define BOARD_EPD_BUSY          GPIO_NUM_37
#define BOARD_EPD_BACKLIGHT     GPIO_NUM_41

// ─────────────────────────────────────────────────────────────────────────────
//  Capacitive Touch (CST328 / CST3530)
// ─────────────────────────────────────────────────────────────────────────────
#define BOARD_TOUCH_INT         GPIO_NUM_12

// ─────────────────────────────────────────────────────────────────────────────
//  TCA8418 Keyboard Controller
// ─────────────────────────────────────────────────────────────────────────────
#define BOARD_KEYBOARD_I2C_ADDR 0x34
#define BOARD_KEYBOARD_INT      GPIO_NUM_15
#define BOARD_KEYBOARD_LED      GPIO_NUM_42

// ─────────────────────────────────────────────────────────────────────────────
//  A7682E 4G LTE Cellular Modem
// ─────────────────────────────────────────────────────────────────────────────
#define BOARD_4G_UART_PORT      UART_NUM_1
#define BOARD_4G_RXD            GPIO_NUM_10 // ESP32 RX <- Modem TX
#define BOARD_4G_TXD            GPIO_NUM_11 // ESP32 TX -> Modem RX
#define BOARD_4G_RI             GPIO_NUM_7
#define BOARD_4G_ITR            GPIO_NUM_8

// ─────────────────────────────────────────────────────────────────────────────
//  Semtech SX1262 LoRa Radio
// ─────────────────────────────────────────────────────────────────────────────
#define BOARD_LORA_CS           GPIO_NUM_3
#define BOARD_LORA_BUSY         GPIO_NUM_6
#define BOARD_LORA_RST          GPIO_NUM_4
#define BOARD_LORA_INT          GPIO_NUM_5

// ─────────────────────────────────────────────────────────────────────────────
//  SD Card (SPI Mode)
// ─────────────────────────────────────────────────────────────────────────────
#define BOARD_SD_CS             GPIO_NUM_48

// ─────────────────────────────────────────────────────────────────────────────
//  u-blox MIA-M10Q GPS
// ─────────────────────────────────────────────────────────────────────────────
#define BOARD_GPS_UART_PORT     UART_NUM_2
#define BOARD_GPS_RXD           GPIO_NUM_2
#define BOARD_GPS_TXD           GPIO_NUM_16
#define BOARD_GPS_PPS           GPIO_NUM_1

// ─────────────────────────────────────────────────────────────────────────────
//  Bosch BHI260AP Gyroscope / IMU
// ─────────────────────────────────────────────────────────────────────────────
#define BOARD_GYRO_INT          GPIO_NUM_21

// ─────────────────────────────────────────────────────────────────────────────
//  XL9555 I/O Expander Pin Mapping
// ─────────────────────────────────────────────────────────────────────────────
#define XL9555_I2C_ADDR         0x20

// Port 0 Pins
#define XL9555_P0_DRV2605_EN    (1 << 5) // Port 0 Bit 5
#define XL9555_P0_SPK_AMP_EN    (1 << 6) // Port 0 Bit 6
#define XL9555_P0_TOUCH_RST     (1 << 7) // Port 0 Bit 7

// Port 1 Pins
#define XL9555_P1_4G_PWR        (1 << 0) // Port 1 Bit 0
#define XL9555_P1_KEYBOARD_RST  (1 << 1) // Port 1 Bit 1
#define XL9555_P1_AUDIO_ROUTE   (1 << 2) // Port 1 Bit 2 (0=ES8311, 1=A7682E)
#define XL9555_P1_ANT_SWITCH    (1 << 4) // Port 1 Bit 4 (0=External, 1=Internal)

#ifdef __cplusplus
}
#endif

#endif // BOARD_TDECK_MAX_H
