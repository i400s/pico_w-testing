#ifndef _MCP9808_H
#define _MCP9808_H

#include "pico/stdlib.h"

#define MCP9808_IRQ 4

void mcp9808_init(gpio_irq_callback_t irq_callback, repeating_timer_callback_t timer_callback);
void mcp9808_reset_irq(void);
void mcp9808_process(repeating_timer_t *rt);
void static mcp9808_set_limits(uint8_t i);
void static mcp9808_check_limits(uint8_t upper_byte);
float static mcp9808_convert_temp(uint8_t upper_byte, uint8_t lower_byte);

#endif