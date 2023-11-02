#include <stdio.h>

#include "hardware/i2c.h"

#include "src/mcp9808.h"

//The bus address is determined by the state of pins A0, A1 and A2 on the MCP9808 board
const uint8_t MCP9808_ADDRESS[4] = {0x18, 0x19, 0x1A, 0x1B};
//hardware registers
const uint8_t REG_POINTER = 0x00;
const uint8_t REG_CONFIG = 0x01;
const uint8_t REG_TEMP_UPPER = 0x02;
const uint8_t REG_TEMP_LOWER = 0x03;
const uint8_t REG_TEMP_CRIT = 0x04;
const uint8_t REG_TEMP_AMB = 0x05;
const uint8_t REG_RESOLUTION = 0x08;

static repeating_timer_t timer;

void mcp9808_init(gpio_irq_callback_t irq_callback, repeating_timer_callback_t timer_callback) {

    float temp = 0.0;
    int16_t whole = 0;
    int16_t decimal = 0;
    uint16_t result = 0;

    temp = 19.00; // LT
    whole = (int16_t)temp << 4;
    decimal = (int16_t)((temp - (int16_t)temp) / .25) << 2;
    result = whole | decimal;
    printf("LT-%2.2f whole (%d) (%x) ", temp, whole, whole);
    printf("decimal (%d) (%x) ", decimal, decimal);
    printf("result (%d) (%x) ", result, result);
    result = (int16_t)temp << 4 | (int16_t)((temp - (int16_t)temp) / .25) << 2;
    printf("full (%2.2f) (%d) (%x) \n", temp, result, result);

    temp = 21.75; // LT+hysteresis(1.5)+1.25
    whole = (int16_t)temp << 4;
    decimal = (int16_t)((temp - (int16_t)temp) / .25) << 2;
    result = whole | decimal;
    printf("UT-%2.2f whole (%d) (%x) ", temp, whole, whole);
    printf("decimal (%d) (%x) ", decimal, decimal);
    printf("result (%d) (%x) ", result, result);
    result = (int16_t)temp << 4 | (int16_t)((temp - (int16_t)temp) / .25) << 2;
    printf("full (%2.2f) (%d) (%x) \n", temp, result, result);

    temp = 24.00; //UT+hysteresis(1.5)+.75
    whole = (int16_t)temp << 4;
    decimal = (int16_t)((temp - (int16_t)temp) / .25) << 2;
    result = whole | decimal;
    printf("CT-%2.2f whole (%d) (%x) ", temp, whole, whole);
    printf("decimal (%d) (%x) ", decimal, decimal);
    printf("result (%d) (%x) ", result, result);
    result = (int16_t)temp << 4 | (int16_t)((temp - (int16_t)temp) / .25) << 2;
    printf("full (%2.2f) (%d) (%x) \n", temp, result, result);

    //
    for (uint8_t i = 0; i < 4; i++) {
        mcp9808_set_limits(i);
    }

    gpio_set_irq_enabled(MCP9808_IRQ, GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_callback(irq_callback);
    irq_set_enabled(IO_IRQ_BANK0, true);
    gpio_pull_up(MCP9808_IRQ);

    add_repeating_timer_ms(15000, timer_callback, NULL, &timer);

}

void mcp9808_set_limits(uint8_t i) {

    //Set a lower limit of 19.00°C for the temperature
    uint8_t lower_temp_msb = 0x01;
    uint8_t lower_temp_lsb = 0x30;

    //Set an upper limit of 21.75°C for the temperature
    uint8_t upper_temp_msb = 0x01;
    uint8_t upper_temp_lsb = 0x5C;

    //Set a critical limit of 24.00°C for the temperature
    uint8_t crit_temp_msb = 0x01;
    uint8_t crit_temp_lsb = 0x80;

    uint8_t buf[3];

    printf("Init (%d", MCP9808_ADDRESS[i]);

    buf[0] = REG_TEMP_LOWER;
    buf[1] = lower_temp_msb;
    buf[2] = lower_temp_lsb;
    if (i2c_write_blocking(i2c0, MCP9808_ADDRESS[i], buf, 3, false) <0) {
        printf("*WE*");
    } else {
        printf(":LT-%02x%02x", buf[1], buf[2]);
    }

    buf[0] = REG_TEMP_UPPER;
    buf[1] = upper_temp_msb;
    buf[2] = upper_temp_lsb;
    if (i2c_write_blocking(i2c0, MCP9808_ADDRESS[i], buf, 3, false) <0) {
        printf("*WE*");
    } else {
        printf(":UT-%02x%02x", buf[1], buf[2]);
    }

    buf[0] = REG_TEMP_CRIT;
    buf[1] = crit_temp_msb;
    buf[2] = crit_temp_lsb;;
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
    for (uint8_t i = 0; i < 4; i++) {
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

}

void mcp9808_check_limits(uint8_t upper_byte) {

    // Check flags and raise alerts accordingly
    if ((upper_byte & 0x40) == 0x40) { //TA > T-UPPER
        printf("\033[0;32m*UL*\033[0m");
    }
    if ((upper_byte & 0x20) == 0x20) { //TA < T-LOWER
        printf("\033[0;32m*LL*\033[0m");
    }
    if ((upper_byte & 0x80) == 0x80) { //TA >= T-CRIT
        printf("\033[0;32m*CT*\033[0m");
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
    }

    uint8_t buf[2];
    uint16_t upper_byte;
    uint16_t lower_byte;

    float temperature;

    printf("Temp: ");

    for (uint8_t i = 0; i < 4; i++) {
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
