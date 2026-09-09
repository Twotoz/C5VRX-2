#include "rf_dump.h"

#include <stdint.h>
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "heap_memory_layout.h"

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))

#define DUMP_CTRL      0x600a9004u
#define DUMP_PTR_MODE  0x600a9008u
#define DUMP_FORMAT    0x600a9018u
#define FE_PATH        0x600a20b4u
#define FE_ENABLE      0x600a0800u
#define SOURCE_CTRL    0x600a08ccu
#define SOURCE_MUX     0x600a70b8u
#define MODEM_CLOCK    0x600a9c04u
#define CTRL_ENABLE    0x80000000u

#define PRE_GUARD_ADDR  0x4082ffc0u
#define POST_GUARD_ADDR 0x40840000u
#define POST_GUARD_END  0x40840040u

/* The C5 RF-test writer targets this fixed internal SRAM window. Never let the
 * normal heap place a task stack/DMA buffer there. */
SOC_RESERVE_MEMORY_REGION(PRE_GUARD_ADDR, POST_GUARD_END, c5vrx2_rf_dump_ram);
extern char _bss_end;

bool c5vrx2_rf_dump_memory_reserved(void)
{
    const volatile soc_reserved_region_t *r = &reserved_region_c5vrx2_rf_dump_ram;
    return r->start == PRE_GUARD_ADDR && r->end == POST_GUARD_END &&
           (uintptr_t)&_bss_end <= PRE_GUARD_ADDR &&
           esp_ptr_internal((const void *)(uintptr_t)C5VRX2_RF_DUMP_BASE) &&
           esp_ptr_dma_capable((const void *)(uintptr_t)C5VRX2_RF_DUMP_BASE) &&
           !heap_caps_check_integrity_addr(C5VRX2_RF_DUMP_BASE, false);
}

static void set_format_mode0(void)
{
    uint32_t v = REG32(DUMP_FORMAT);
    v = (v & 0xff03ffffu) | 0x006c0000u;
    REG32(DUMP_FORMAT) = v;
    v = REG32(DUMP_FORMAT);
    v = (v & 0xfffc0fffu) | 0x0001a000u;
    REG32(DUMP_FORMAT) = v;
    v = REG32(DUMP_FORMAT);
    v = (v & 0xfffff03fu) | 0x00000640u;
    REG32(DUMP_FORMAT) = v;
    v = REG32(DUMP_FORMAT);
    v = (v & 0xffffffc0u) | 0x18u;
    REG32(DUMP_FORMAT) = v | 0x01000000u;
}

esp_err_t c5vrx2_rf_dump_prepare_mode0(void)
{
    if (!c5vrx2_rf_dump_memory_reserved()) return ESP_ERR_INVALID_STATE;
    if (REG32(DUMP_CTRL) & CTRL_ENABLE) return ESP_ERR_INVALID_STATE;

    /* Exact ordinary-RX source/format subset recovered from the pinned C5
     * v6.0.2 RF-test path. Ownership and START are intentionally NOT done
     * here: the LP core owns that timing-critical interval. */
    REG32(SOURCE_CTRL) &= 0xff87ffffu;
    REG32(SOURCE_MUX) = (REG32(SOURCE_MUX) & 0xfffffff8u) | 1u;
    REG32(MODEM_CLOCK) = UINT32_MAX;
    REG32(FE_ENABLE) |= 4u;

    uint32_t control = REG32(DUMP_CTRL);
    control = (control & ~0x0001ffffu) | C5VRX2_RF_WORDS;
    control &= ~0x00320000u; /* mode bit17 + recovered control bits20..21 */
    REG32(DUMP_CTRL) = control;
    set_format_mode0();

    REG32(FE_PATH) &= ~1u;
    REG32(DUMP_PTR_MODE) |= 0x01e00000u;
    __asm__ __volatile__("fence iorw, iorw" ::: "memory");
    return ESP_OK;
}
