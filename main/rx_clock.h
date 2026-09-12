#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#define C5VRX2_RX_CLOCK_GPIO GPIO_NUM_2
#define C5VRX2_RX_CLOCK_HZ   40000000u

esp_err_t c5vrx2_rx_clock_start(void);
void c5vrx2_rx_clock_stop(void);
bool c5vrx2_rx_clock_is_external(void);
const char *c5vrx2_rx_clock_name(void);
uint32_t c5vrx2_rx_clock_diag_signal(void);
