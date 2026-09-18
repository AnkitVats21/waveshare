#pragma once

#include "sdkconfig.h"

/**
 * @brief Physical Pin Definitions for Waveshare S3 Audio Board
 */
#define I2C_NUM I2C_NUM_0
#define GPIO_I2C_SCL 10
#define GPIO_I2C_SDA 11

#define GPIO_I2S_LRCK 14
#define GPIO_I2S_MCLK 12
#define GPIO_I2S_SCLK 13
#define GPIO_I2S_SDIN 15
#define GPIO_I2S_DOUT 16

#define GPIO_PWR_CTRL -1 // Controlled via IO Expander

// SD Card Hardware Pins
#define FUNC_SDMMC_EN 1
#define FUNC_SDSPI_EN 0
#define SDMMC_BUS_WIDTH 1
#define GPIO_SDMMC_CLK 40
#define GPIO_SDMMC_CMD 42
#define GPIO_SDMMC_D0 41
#define GPIO_SDMMC_D1 -1
#define GPIO_SDMMC_D2 -1
#define GPIO_SDMMC_D3 -1
#define GPIO_SDMMC_DET -1

#define MAX_FILE_NAME_SIZE 128
#define MAX_PATH_SIZE 256

// Strip LED Hardware Pins
#define LED_STRIP_GPIO_PIN 38
#define LED_STRIP_LED_COUNT 7

// ESP32-WROOM Bluetooth Companion Hardware Pins
#if CONFIG_BT_COMPANION_ENABLE
#define BT_COMPANION_I2S_PORT_NUM      CONFIG_BT_COMPANION_I2S_PORT
#define BT_COMPANION_I2S_BCLK_GPIO     CONFIG_BT_COMPANION_I2S_BCLK_PIN
#define BT_COMPANION_I2S_WS_GPIO       CONFIG_BT_COMPANION_I2S_WS_PIN
#define BT_COMPANION_I2S_DOUT_GPIO     CONFIG_BT_COMPANION_I2S_DOUT_PIN
#define BT_COMPANION_UART_NUM          CONFIG_BT_COMPANION_UART_PORT
#define BT_COMPANION_UART_TX_GPIO      CONFIG_BT_COMPANION_UART_TX_PIN
#define BT_COMPANION_UART_RX_GPIO      CONFIG_BT_COMPANION_UART_RX_PIN
#define BT_COMPANION_UART_BAUD_RATE    CONFIG_BT_COMPANION_UART_BAUDRATE
#endif

// SPI Display & Touch Hardware Pins (ILI9341 + XPT2046)
#if CONFIG_DISPLAY_ENABLE
#define LCD_SPI_HOST                   static_cast<spi_host_device_t>(CONFIG_DISPLAY_SPI_HOST)
#define LCD_GPIO_SCLK                  CONFIG_DISPLAY_SCLK_PIN
#define LCD_GPIO_MOSI                  CONFIG_DISPLAY_MOSI_PIN
#define LCD_GPIO_MISO                  CONFIG_DISPLAY_MISO_PIN
#define LCD_GPIO_CS                    CONFIG_DISPLAY_CS_PIN
#define LCD_GPIO_DC                    CONFIG_DISPLAY_DC_PIN
#define LCD_GPIO_RST                   CONFIG_DISPLAY_RST_PIN
#define LCD_GPIO_BL                    CONFIG_DISPLAY_BL_PIN
#define LCD_H_RES                      CONFIG_DISPLAY_WIDTH
#define LCD_V_RES                      CONFIG_DISPLAY_HEIGHT
#if CONFIG_TOUCH_ENABLE
#define TOUCH_GPIO_CS                  CONFIG_TOUCH_CS_PIN
#define TOUCH_GPIO_IRQ                 CONFIG_TOUCH_IRQ_PIN
#endif
#endif