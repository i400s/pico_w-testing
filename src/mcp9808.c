#include <stdio.h>

#include "hardware/i2c.h"

#include "src/mcp9808.h"
#define LSB(w) ((uint8_t) ((w) & 0xFF))
#define MSB(w) ((uint8_t) ((w) >> 8))
static uint16_t mcp9808_calc_register(float temp);
static void mcp9808_set_limits(uint8_t i, u_int16_t reg_frost, uint16_t reg_heating, u_int16_t reg_conditioning);
static void mcp9808_print_temp(void);
static void mcp9808_check_limits(uint8_t upper_byte);
static float mcp9808_convert_temp(uint8_t upper_byte, uint8_t lower_byte);

//The bus address is determined by the state of pins A0, A1 and A2 on the MCP9808 board
const uint8_t MCP9808_ADDRESS[2] = {0x18, 0x19};
const uint8_t MCP9808_DEV_COUNT = 2;
const int32_t MCP9808_CALLBACK_TIME = 30000; // 30 Seconds
//hardware registers
const uint8_t REG_POINTER = 0x00;
const uint8_t REG_CONFIG = 0x01;
const uint8_t REG_TEMP_FROST = 0x03; // Lower temp.
const uint8_t REG_TEMP_HEATING = 0x02; // Upper temp.
const uint8_t REG_TEMP_CONDITIONING = 0x04; // Critical temp.
const uint8_t REG_TEMP_AMB = 0x05;
const uint8_t REG_RESOLUTION = 0x08;

static repeating_timer_t timer;

static uint16_t mcp9808_calc_register(float temp) {
    return (int16_t)temp << 4 | (int16_t)((temp - (int16_t)temp) / .25) << 2;
}

void mcp9808_init(gpio_irq_callback_t irq_callback, repeating_timer_callback_t timer_callback) {

    // Frost protection calculation is 1°C higher for +1°C to -.5°C hysteresis.
    float frost = 10.00;
    uint16_t reg_frost = mcp9808_calc_register(frost + 1);
    float adj_frost = mcp9808_convert_temp(MSB(reg_frost), LSB(reg_frost));
    // Heating calculation is .75°C higher for +.75°C to -.75°C hysteresis.
    float heating = 20.25;
    uint16_t reg_heating = mcp9808_calc_register(heating + .75);
    float adj_heating = mcp9808_convert_temp(MSB(reg_heating), LSB(reg_heating));
    // Air conditioning calculation is 1.5°C higher for +1.5°C to -.00°C hysteresis.
    float conditioning = 24.00;
    uint16_t reg_conditioning = mcp9808_calc_register(conditioning + 1.5);
    float adj_conditioning = mcp9808_convert_temp(MSB(reg_conditioning), LSB(reg_conditioning));

    printf("Temps: Frost(%2.2f %04X %2.2f) Heating(%2.2f %04X %2.2f) Conditioning(%2.2f %04X %2.2f) \n"
        , frost, reg_frost, adj_frost, heating, reg_heating, adj_heating, conditioning, reg_conditioning, adj_conditioning);

    //
    for (uint8_t i = 0; i < MCP9808_DEV_COUNT; i++) {
        mcp9808_set_limits(i, reg_frost, reg_heating, reg_conditioning);
    }

    gpio_set_irq_enabled(MCP9808_IRQ, GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_callback(irq_callback);
    irq_set_enabled(IO_IRQ_BANK0, true);
    gpio_pull_up(MCP9808_IRQ);

    add_repeating_timer_ms(MCP9808_CALLBACK_TIME, timer_callback, NULL, &timer);

}

void mcp9808_set_limits(uint8_t i, u_int16_t reg_frost, uint16_t reg_heating, u_int16_t reg_conditioning) {

    uint8_t buf[3];

    printf("Init (%d", MCP9808_ADDRESS[i]);

    buf[0] = REG_TEMP_FROST;
    buf[1] = MSB(reg_frost);
    buf[2] = LSB(reg_frost);
    if (i2c_write_blocking(i2c0, MCP9808_ADDRESS[i], buf, 3, false) <0) {
        printf("*WE*");
    } else {
        printf(":LT-%02x%02x", buf[1], buf[2]);
    }

    buf[0] = REG_TEMP_HEATING;
    buf[1] = MSB(reg_heating);
    buf[2] = LSB(reg_heating);
    if (i2c_write_blocking(i2c0, MCP9808_ADDRESS[i], buf, 3, false) <0) {
        printf("*WE*");
    } else {
        printf(":UT-%02x%02x", buf[1], buf[2]);
    }

    buf[0] = REG_TEMP_CONDITIONING;
    buf[1] = MSB(reg_conditioning);
    buf[2] = LSB(reg_conditioning);
    if (i2c_write_blocking(i2c0, MCP9808_ADDRESS[i], buf, 3, false) <0) {
        printf("*WE*");
    } else {
        printf(":CT-%02x%02x", buf[1], buf[2]);
    }

    buf[0] = REG_RESOLUTION;
    buf[1] = 0x01; // .25°C resolution
    if (i2c_write_blocking(i2c0, MCP9808_ADDRESS[i], buf, 2, false) <0) {
        printf("*WE*");
    } else {
        printf(":RS-%02x", buf[1]);
    }

    buf[0] = REG_CONFIG;
    buf[1] = 0x02; // 21:Hysteresis 1.5°C
    buf[2] = 0x39; // 5:Interrupt clear 3:Alert 0:interrupt mode
    if (i2c_write_blocking(i2c0, MCP9808_ADDRESS[i], buf, 3, false) < 0) {
        printf("*WE*");
    } else {
        printf(":CN-%02x%02x", buf[1], buf[2]);
    }

    buf[1] = 0;
    buf[2] = 0;
    if (i2c_write_blocking(i2c0, MCP9808_ADDRESS[i], &REG_CONFIG, 1, true) < 0) {
        printf("*WE*");
    } else if (i2c_read_blocking(i2c0, MCP9808_ADDRESS[i], &buf[1], 2, false) < 0) {
        printf("*RE*");
    } else {
        printf(":%02x%02x*OK*) ", buf[1], buf[2]);
    }

    printf("\n");

}

void mcp9808_reset_irq(void) {
    uint8_t buf[3];
    buf[0] = REG_CONFIG;
    buf[1] = 0;
    buf[2] = 0;
    for (uint8_t i = 0; i < MCP9808_DEV_COUNT; i++) {
        printf("(%d ", MCP9808_ADDRESS[i]);
        if (i2c_write_blocking(i2c0, MCP9808_ADDRESS[i], &REG_CONFIG, 1, true) < 0) {
            printf("*WE*");
        } else if (i2c_read_blocking(i2c0, MCP9808_ADDRESS[i], &buf[1], 2, false) < 0) {
            printf("*RE*");
        } else {
            printf(":%02x%02x*OK*", buf[1], buf[2]);
            if (buf[2] & (1 << 4)) { // 4:Interrupt this device
                buf[2] |= (1 << 5); // 5:Interrupt clear
                printf("\033[0;32m:%02x%02x", buf[1], buf[2]);
                if (i2c_write_blocking(i2c0, MCP9808_ADDRESS[i], buf, 3, false) < 0) {
                    printf("*WE*");
                } else if (i2c_write_blocking(i2c0, MCP9808_ADDRESS[i], &REG_CONFIG, 1, true) < 0) {
                    printf("*WE*");
                } else if (i2c_read_blocking(i2c0, MCP9808_ADDRESS[i], &buf[1], 2, false) < 0) {
                    printf("*RE*");
                } else {
                    printf(":%02x%02x*OK*", buf[1], buf[2]);
                }
                printf("\033[0m");
            }
            printf(") ");
        }
    }
    printf(" \n");
    mcp9808_print_temp();
}

void mcp9808_check_limits(uint8_t upper_byte) {

    // Check flags and raise alerts accordingly
    if ((upper_byte & 0x20) == 0x20) { // < (frost - hysteresis)
        printf("\033[0;32m*frost*\033[0m");
    }
    if ((upper_byte & 0x40) != 0x40) { // < (heating) or < (heating - hysteresis)
        printf("\033[0;32m*heat*\033[0m");
    }
    if ((upper_byte & 0x80) == 0x80) { // > (conditioning)
        printf("\033[0;32m*conditioning*\033[0m");
    }
}

float mcp9808_convert_temp(uint8_t upper_byte, uint8_t lower_byte) {

    float temperature;


    //Check if TA <= 0°C and convert to denary accordingly
    if ((upper_byte & 0x10) == 0x10) {
        upper_byte = upper_byte & 0x0F;
        temperature = 256 - (((float) upper_byte * 16) + ((float) lower_byte / 16));
    } else {
        temperature = (((float) upper_byte * 16) + ((float) lower_byte / 16));

    }
    return temperature;
}

void mcp9808_process(repeating_timer_t *rt) {

    if (rt->alarm_id != timer.alarm_id) {
        return;
    } else {
        mcp9808_print_temp();
    }
}

void mcp9808_print_temp() {
    uint8_t buf[2];
    uint16_t upper_byte;
    uint16_t lower_byte;

    float temperature;

    printf("Temp: ");

    for (uint8_t i = 0; i < MCP9808_DEV_COUNT; i++) {
        // Start reading ambient temperature register for 2 bytes
        if (i2c_write_blocking(i2c0, MCP9808_ADDRESS[i], &REG_TEMP_AMB, 1, true) < 0) {
            printf("*WE*");
            continue;
        } else if (i2c_read_blocking(i2c0, MCP9808_ADDRESS[i], buf, 2, false) < 0) {
            printf("*RE*");
            continue;
        }

        upper_byte = buf[0];
        lower_byte = buf[1];

        // printf("UB:%x ", upper_byte);

        //clears flag bits in upper byte
        temperature = mcp9808_convert_temp(upper_byte & 0x1F, lower_byte);
        printf("(%d: %.2f°C", MCP9808_ADDRESS[i], temperature);

        //isolates limit flags in upper byte
        mcp9808_check_limits(upper_byte & 0xE0);

        printf(") ");
    }
    printf("\n");
}
