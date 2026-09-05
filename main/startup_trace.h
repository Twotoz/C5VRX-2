#pragma once

#include <stdint.h>

#include "esp_err.h"

void c5vrx2_trace_begin(void);
void c5vrx2_trace_stage(uint32_t stage, esp_err_t error);
