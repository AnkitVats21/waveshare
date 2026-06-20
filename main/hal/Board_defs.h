#pragma once

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