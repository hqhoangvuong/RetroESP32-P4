/*
 * SPDX-FileCopyrightText: 2022-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* cmd_i2ctools.c

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "argtable3/argtable3.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_console.h"
#include "esp_log.h"

static const char *TAG = "cmd_i2ctools";

#define I2C_TOOL_TIMEOUT_VALUE_MS (50)
#define I2C_TOOL_MAX_DATA_LEN      (256)
#define I2C_TOOL_MAX_FREQUENCY_HZ  (1000 * 1000)
static uint32_t i2c_frequency = 100 * 1000;
i2c_master_bus_handle_t tool_bus_handle;
static i2c_master_bus_config_t tool_bus_config;

void i2ctools_set_bus_config(const i2c_master_bus_config_t *config)
{
    if (config != NULL) {
        tool_bus_config = *config;
    }
}

static bool i2c_bus_is_ready(void)
{
    if (tool_bus_handle == NULL) {
        ESP_LOGE(TAG, "I2C bus is not initialized; run i2cconfig with valid settings");
        return false;
    }
    return true;
}

static bool i2c_address_is_valid(int address)
{
    if (address < 0 || address > 0x7f) {
        ESP_LOGE(TAG, "I2C address must be between 0x00 and 0x7f");
        return false;
    }
    return true;
}

static esp_err_t i2c_get_port(int port, i2c_port_t *i2c_port)
{
    if (port < 0 || port >= I2C_NUM_MAX) {
        ESP_LOGE(TAG, "Wrong port number: %d", port);
        return ESP_FAIL;
    }
    *i2c_port = port;
    return ESP_OK;
}

static struct {
    struct arg_int *port;
    struct arg_int *freq;
    struct arg_int *sda;
    struct arg_int *scl;
    struct arg_end *end;
} i2cconfig_args;

static int do_i2cconfig_cmd(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&i2cconfig_args);
    i2c_port_t i2c_port = I2C_NUM_0;
    int i2c_gpio_sda = 0;
    int i2c_gpio_scl = 0;
    if (nerrors != 0) {
        arg_print_errors(stderr, i2cconfig_args.end, argv[0]);
        return 0;
    }

    /* Check "--port" option */
    if (i2cconfig_args.port->count) {
        if (i2c_get_port(i2cconfig_args.port->ival[0], &i2c_port) != ESP_OK) {
            return 1;
        }
    }
    /* Check "--freq" option */
    uint32_t requested_frequency = i2c_frequency;
    if (i2cconfig_args.freq->count) {
        int frequency = i2cconfig_args.freq->ival[0];
        if (frequency <= 0 || frequency > I2C_TOOL_MAX_FREQUENCY_HZ) {
            ESP_LOGE(TAG, "Frequency must be between 1 and %d Hz", I2C_TOOL_MAX_FREQUENCY_HZ);
            return 1;
        }
        requested_frequency = (uint32_t)frequency;
    }
    /* Check "--sda" option */
    i2c_gpio_sda = i2cconfig_args.sda->ival[0];
    /* Check "--scl" option */
    i2c_gpio_scl = i2cconfig_args.scl->ival[0];

    if (!GPIO_IS_VALID_GPIO(i2c_gpio_sda) || !GPIO_IS_VALID_GPIO(i2c_gpio_scl) ||
            i2c_gpio_sda == i2c_gpio_scl) {
        ESP_LOGE(TAG, "SDA and SCL must be different valid GPIOs");
        return 1;
    }

    i2c_master_bus_config_t requested_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = i2c_port,
        .scl_io_num = i2c_gpio_scl,
        .sda_io_num = i2c_gpio_sda,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    bool had_previous_bus = tool_bus_handle != NULL;
    if (had_previous_bus) {
        // Reconfigure transactionally while keeping the previous settings for rollback.
        if (i2c_del_master_bus(tool_bus_handle) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to release the current I2C bus");
            return 1;
        }
        tool_bus_handle = NULL;
    }

    if (i2c_new_master_bus(&requested_config, &tool_bus_handle) != ESP_OK) {
        ESP_LOGE(TAG, "New I2C configuration failed");
        if (had_previous_bus) {
            ESP_LOGW(TAG, "Restoring the previous I2C bus configuration");
            if (i2c_new_master_bus(&tool_bus_config, &tool_bus_handle) != ESP_OK) {
                tool_bus_handle = NULL;
                ESP_LOGE(TAG, "Previous I2C bus could not be restored; retry i2cconfig");
            }
        }
        return 1;
    }

    tool_bus_config = requested_config;
    i2c_frequency = requested_frequency;

    return 0;
}

static void register_i2cconfig(void)
{
    i2cconfig_args.port = arg_int0(NULL, "port", "<0|1>", "Set the I2C bus port number");
    i2cconfig_args.freq = arg_int0(NULL, "freq", "<Hz>", "Set the frequency(Hz) of I2C bus");
    i2cconfig_args.sda = arg_int1(NULL, "sda", "<gpio>", "Set the gpio for I2C SDA");
    i2cconfig_args.scl = arg_int1(NULL, "scl", "<gpio>", "Set the gpio for I2C SCL");
    i2cconfig_args.end = arg_end(2);
    const esp_console_cmd_t i2cconfig_cmd = {
        .command = "i2cconfig",
        .help = "Config I2C bus",
        .hint = NULL,
        .func = &do_i2cconfig_cmd,
        .argtable = &i2cconfig_args
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&i2cconfig_cmd));
}

static int do_i2cdetect_cmd(int argc, char **argv)
{
    if (!i2c_bus_is_ready()) {
        return 1;
    }
    uint8_t address;
    printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\r\n");
    for (int i = 0; i < 128; i += 16) {
        printf("%02x: ", i);
        for (int j = 0; j < 16; j++) {
            fflush(stdout);
            address = i + j;
            esp_err_t ret = i2c_master_probe(tool_bus_handle, address, I2C_TOOL_TIMEOUT_VALUE_MS);
            if (ret == ESP_OK) {
                printf("%02x ", address);
            } else if (ret == ESP_ERR_TIMEOUT) {
                printf("UU ");
            } else {
                printf("-- ");
            }
        }
        printf("\r\n");
    }

    return 0;
}

static void register_i2cdetect(void)
{
    const esp_console_cmd_t i2cdetect_cmd = {
        .command = "i2cdetect",
        .help = "Scan I2C bus for devices",
        .hint = NULL,
        .func = &do_i2cdetect_cmd,
        .argtable = NULL
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&i2cdetect_cmd));
}

static struct {
    struct arg_int *chip_address;
    struct arg_int *register_address;
    struct arg_int *data_length;
    struct arg_end *end;
} i2cget_args;

static int do_i2cget_cmd(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&i2cget_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, i2cget_args.end, argv[0]);
        return 0;
    }

    /* Check chip address: "-c" option */
    int chip_addr = i2cget_args.chip_address->ival[0];
    if (!i2c_bus_is_ready() || !i2c_address_is_valid(chip_addr)) {
        return 1;
    }
    /* Check register address: "-r" option */
    int data_addr = -1;
    if (i2cget_args.register_address->count) {
        data_addr = i2cget_args.register_address->ival[0];
        if (data_addr < 0 || data_addr > UINT8_MAX) {
            ESP_LOGE(TAG, "Register address must be between 0x00 and 0xff");
            return 1;
        }
    }
    /* Check data length: "-l" option */
    int len = 1;
    if (i2cget_args.data_length->count) {
        len = i2cget_args.data_length->ival[0];
    }
    if (len <= 0 || len > I2C_TOOL_MAX_DATA_LEN) {
        ESP_LOGE(TAG, "Read length must be between 1 and %d bytes", I2C_TOOL_MAX_DATA_LEN);
        return 1;
    }
    uint8_t *data = malloc((size_t)len);
    if (data == NULL) {
        ESP_LOGE(TAG, "Not enough memory for %d read bytes", len);
        return 1;
    }

    i2c_device_config_t i2c_dev_conf = {
        .scl_speed_hz = i2c_frequency,
        .device_address = chip_addr,
    };
    i2c_master_dev_handle_t dev_handle;
    if (i2c_master_bus_add_device(tool_bus_handle, &i2c_dev_conf, &dev_handle) != ESP_OK) {
        free(data);
        return 1;
    }

    esp_err_t ret;
    if (data_addr >= 0) {
        uint8_t register_address = (uint8_t)data_addr;
        ret = i2c_master_transmit_receive(dev_handle, &register_address, 1, data, len,
                                          I2C_TOOL_TIMEOUT_VALUE_MS);
    } else {
        ret = i2c_master_receive(dev_handle, data, len, I2C_TOOL_TIMEOUT_VALUE_MS);
    }
    int command_status = 0;
    if (ret == ESP_OK) {
        for (int i = 0; i < len; i++) {
            printf("0x%02x ", data[i]);
            if ((i + 1) % 16 == 0) {
                printf("\r\n");
            }
        }
        if (len % 16) {
            printf("\r\n");
        }
    } else if (ret == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "Bus is busy");
        command_status = 1;
    } else {
        ESP_LOGW(TAG, "Read failed");
        command_status = 1;
    }
    free(data);
    if (i2c_master_bus_rm_device(dev_handle) != ESP_OK) {
        return 1;
    }
    return command_status;
}

static void register_i2cget(void)
{
    i2cget_args.chip_address = arg_int1("c", "chip", "<chip_addr>", "Specify the address of the chip on that bus");
    i2cget_args.register_address = arg_int0("r", "register", "<register_addr>", "Specify the address on that chip to read from");
    i2cget_args.data_length = arg_int0("l", "length", "<length>", "Specify the length to read from that data address");
    i2cget_args.end = arg_end(1);
    const esp_console_cmd_t i2cget_cmd = {
        .command = "i2cget",
        .help = "Read registers visible through the I2C bus",
        .hint = NULL,
        .func = &do_i2cget_cmd,
        .argtable = &i2cget_args
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&i2cget_cmd));
}

static struct {
    struct arg_int *chip_address;
    struct arg_int *register_address;
    struct arg_int *data;
    struct arg_end *end;
} i2cset_args;

static int do_i2cset_cmd(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&i2cset_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, i2cset_args.end, argv[0]);
        return 0;
    }

    /* Check chip address: "-c" option */
    int chip_addr = i2cset_args.chip_address->ival[0];
    if (!i2c_bus_is_ready() || !i2c_address_is_valid(chip_addr)) {
        return 1;
    }
    /* Check register address: "-r" option */
    int data_addr = 0;
    if (i2cset_args.register_address->count) {
        data_addr = i2cset_args.register_address->ival[0];
    }
    if (data_addr < 0 || data_addr > UINT8_MAX) {
        ESP_LOGE(TAG, "Register address must be between 0x00 and 0xff");
        return 1;
    }
    /* Check data: "-d" option */
    int len = i2cset_args.data->count;
    for (int i = 0; i < len; i++) {
        if (i2cset_args.data->ival[i] < 0 || i2cset_args.data->ival[i] > UINT8_MAX) {
            ESP_LOGE(TAG, "Data bytes must be between 0x00 and 0xff");
            return 1;
        }
    }

    uint8_t *data = malloc((size_t)len + 1);
    if (data == NULL) {
        ESP_LOGE(TAG, "Not enough memory for write buffer");
        return 1;
    }

    i2c_device_config_t i2c_dev_conf = {
        .scl_speed_hz = i2c_frequency,
        .device_address = chip_addr,
    };
    i2c_master_dev_handle_t dev_handle;
    if (i2c_master_bus_add_device(tool_bus_handle, &i2c_dev_conf, &dev_handle) != ESP_OK) {
        free(data);
        return 1;
    }

    data[0] = (uint8_t)data_addr;
    for (int i = 0; i < len; i++) {
        data[i + 1] = i2cset_args.data->ival[i];
    }
    esp_err_t ret = i2c_master_transmit(dev_handle, data, len + 1, I2C_TOOL_TIMEOUT_VALUE_MS);
    int command_status = 0;
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Write OK");
    } else if (ret == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "Bus is busy");
        command_status = 1;
    } else {
        ESP_LOGW(TAG, "Write Failed");
        command_status = 1;
    }

    free(data);
    if (i2c_master_bus_rm_device(dev_handle) != ESP_OK) {
        return 1;
    }
    return command_status;
}

static void register_i2cset(void)
{
    i2cset_args.chip_address = arg_int1("c", "chip", "<chip_addr>", "Specify the address of the chip on that bus");
    i2cset_args.register_address = arg_int0("r", "register", "<register_addr>", "Specify the address on that chip to read from");
    i2cset_args.data = arg_intn(NULL, NULL, "<data>", 0, 256, "Specify the data to write to that data address");
    i2cset_args.end = arg_end(2);
    const esp_console_cmd_t i2cset_cmd = {
        .command = "i2cset",
        .help = "Set registers visible through the I2C bus",
        .hint = NULL,
        .func = &do_i2cset_cmd,
        .argtable = &i2cset_args
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&i2cset_cmd));
}

static struct {
    struct arg_int *chip_address;
    struct arg_int *size;
    struct arg_end *end;
} i2cdump_args;

static int do_i2cdump_cmd(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&i2cdump_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, i2cdump_args.end, argv[0]);
        return 0;
    }

    /* Check chip address: "-c" option */
    int chip_addr = i2cdump_args.chip_address->ival[0];
    if (!i2c_bus_is_ready() || !i2c_address_is_valid(chip_addr)) {
        return 1;
    }
    /* Check read size: "-s" option */
    int size = 1;
    if (i2cdump_args.size->count) {
        size = i2cdump_args.size->ival[0];
    }
    if (size != 1 && size != 2 && size != 4) {
        ESP_LOGE(TAG, "Wrong read size. Only support 1,2,4");
        return 1;
    }

    i2c_device_config_t i2c_dev_conf = {
        .scl_speed_hz = i2c_frequency,
        .device_address = chip_addr,
    };
    i2c_master_dev_handle_t dev_handle;
    if (i2c_master_bus_add_device(tool_bus_handle, &i2c_dev_conf, &dev_handle) != ESP_OK) {
        return 1;
    }

    uint8_t data_addr;
    uint8_t data[4];
    int32_t block[16];
    printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f"
           "    0123456789abcdef\r\n");
    for (int i = 0; i < 128; i += 16) {
        printf("%02x: ", i);
        for (int j = 0; j < 16; j += size) {
            fflush(stdout);
            data_addr = i + j;
            esp_err_t ret = i2c_master_transmit_receive(dev_handle, &data_addr, 1, data, size, I2C_TOOL_TIMEOUT_VALUE_MS);
            if (ret == ESP_OK) {
                for (int k = 0; k < size; k++) {
                    printf("%02x ", data[k]);
                    block[j + k] = data[k];
                }
            } else {
                for (int k = 0; k < size; k++) {
                    printf("XX ");
                    block[j + k] = -1;
                }
            }
        }
        printf("   ");
        for (int k = 0; k < 16; k++) {
            if (block[k] < 0) {
                printf("X");
            } else if ((block[k] & 0xff) == 0x00 || (block[k] & 0xff) == 0xff) {
                printf(".");
            } else if ((block[k] & 0xff) < 32 || (block[k] & 0xff) >= 127) {
                printf("?");
            } else {
                printf("%c", (char)(block[k] & 0xff));
            }
        }
        printf("\r\n");
    }
    if (i2c_master_bus_rm_device(dev_handle) != ESP_OK) {
        return 1;
    }
    return 0;
}

static void register_i2cdump(void)
{
    i2cdump_args.chip_address = arg_int1("c", "chip", "<chip_addr>", "Specify the address of the chip on that bus");
    i2cdump_args.size = arg_int0("s", "size", "<size>", "Specify the size of each read");
    i2cdump_args.end = arg_end(1);
    const esp_console_cmd_t i2cdump_cmd = {
        .command = "i2cdump",
        .help = "Examine registers visible through the I2C bus",
        .hint = NULL,
        .func = &do_i2cdump_cmd,
        .argtable = &i2cdump_args
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&i2cdump_cmd));
}

void register_i2ctools(void)
{
    register_i2cconfig();
    register_i2cdetect();
    register_i2cget();
    register_i2cset();
    register_i2cdump();
}
