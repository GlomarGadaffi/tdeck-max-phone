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

// Full XL9555 pin map, matching LilyGO's own BOARD_XL9555_* numbering
// (their pins 0-7 are P0_0..P0_7, pins 8-15 are P1_0..P1_7).
//
// IMPORTANT: LilyGO's factory example drives EVERY one of these HIGH at boot,
// as outputs, with a 1 ms settle between each, BEFORE touching any peripheral.
// These are power-rail enables, not per-feature toggles -- leaving any of them
// low can leave a device half-powered: responsive on I2C while its analog
// section is dead. Do not "optimise" unused ones away without measuring.
//
// Port 0
#define XL9555_P0_6609_EN       (1 << 0) // A7682E module power rail
#define XL9555_P0_LORA_EN       (1 << 1) // SX1262 power
#define XL9555_P0_GPS_EN        (1 << 2) // GPS power
#define XL9555_P0_1V8_EN        (1 << 3) // 1.8V rail (labelled BHI260AP)
#define XL9555_P0_LORA_SEL      (1 << 4) // HIGH = internal antenna
#define XL9555_P0_DRV2605_EN    (1 << 5) // haptics power
#define XL9555_P0_SPK_AMP_EN    (1 << 6) // speaker power amplifier
#define XL9555_P0_TOUCH_RST     (1 << 7) // LOW = touch held in reset

// Port 1
#define XL9555_P1_4G_PWR        (1 << 0) // A7682E POWERKEY
#define XL9555_P1_KEYBOARD_RST  (1 << 1) // LOW = TCA8418 held in reset
#define XL9555_P1_AUDIO_ROUTE   (1 << 2) // 0=ES8311, 1=A7682E
#define XL9555_P1_ANT_SWITCH    (1 << 4) // 0=External, 1=Internal

// Everything LilyGO's factory bring-up asserts HIGH.
#define XL9555_P0_BRINGUP_ALL   (XL9555_P0_6609_EN | XL9555_P0_LORA_EN | \
                                 XL9555_P0_GPS_EN | XL9555_P0_1V8_EN | \
                                 XL9555_P0_LORA_SEL | XL9555_P0_DRV2605_EN | \
                                 XL9555_P0_SPK_AMP_EN | XL9555_P0_TOUCH_RST)
#define XL9555_P1_BRINGUP_ALL   (XL9555_P1_4G_PWR | XL9555_P1_KEYBOARD_RST | \
                                 XL9555_P1_AUDIO_ROUTE)

#ifdef __cplusplus
}
#endif

#endif // BOARD_TDECK_MAX_H
