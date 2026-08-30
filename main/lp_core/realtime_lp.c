#include <stdint.h>
#include "riscv/rvruntime-frames.h"
#include "ulp_lp_core_cpu_freq_shared.h"

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define DUMP_CTRL       0x600a9004u
#define DUMP_PTR        0x600a9008u
#define HP_SRAM_USAGE   0x60095004u
#define PARLIO_TX_CLOCK 0x600960b4u
#define CTRL_ENABLE     0x80000000u
#define CTRL_START      0x00080000u
#define CTRL_DONE       0x00040000u
#define PTR_MASK        0x00003fffu
#define PARLIO_CLK_EN   0x00040000u

#define CMD_NONE       0u
#define CMD_CONTINUOUS 1u
#define CMD_STOP       2u

#define STATE_BOOT     0u
#define STATE_READY    1u
#define STATE_RUNNING  2u
#define STATE_ERROR    3u
#define STATE_STOPPED  4u

volatile uint32_t c5vrx2_command;
volatile uint32_t c5vrx2_state;
volatile uint32_t c5vrx2_runs;
volatile uint32_t c5vrx2_rearms;
volatile uint32_t c5vrx2_rearm_failures;
volatile uint32_t c5vrx2_blocks;
volatile uint32_t c5vrx2_gap_cycles_last;
volatile uint32_t c5vrx2_gap_cycles_max;
volatile uint32_t c5vrx2_gap_cycles_total;
volatile uint32_t c5vrx2_last_pointer;
volatile uint32_t c5vrx2_saved_ownership;
volatile uint32_t c5vrx2_fault_cause;
volatile uint32_t c5vrx2_fault_address;
volatile uint32_t c5vrx2_fault_pc;
volatile uint32_t c5vrx2_stage;

static inline void fence_io(void)
{
    __asm__ __volatile__("fence iorw, iorw" ::: "memory");
}

static inline uint32_t cycles(void)
{
    uint32_t v;
    __asm__ __volatile__("csrr %0, mcycle" : "=r"(v));
    return v;
}

static inline uint32_t cycles_for_us(uint32_t us)
{
    return us * LP_CORE_CYCLES_PER_US_NUM / LP_CORE_CYCLES_PER_US_DENOM;
}

static inline uint32_t pointer(void)
{
    return REG32(DUMP_PTR) & PTR_MASK;
}

/* Timing-critical one-shot restart. No stats, logging, heap, USB, DSP or
 * PARLIO work is permitted before these four writes are complete. */
static inline void rearm_now(void)
{
    uint32_t c = REG32(DUMP_CTRL) & ~CTRL_ENABLE;
    REG32(DUMP_CTRL) = c;
    fence_io();
    c |= CTRL_ENABLE;
    REG32(DUMP_CTRL) = c;
    REG32(DUMP_CTRL) = c | CTRL_START;
    REG32(DUMP_CTRL) = c & ~CTRL_START;
    fence_io();
}

void __attribute__((noreturn)) ulp_lp_core_panic_handler(RvExcFrame *frame,
                                                         int cause)
{
    c5vrx2_fault_cause = (uint32_t)cause;
    c5vrx2_fault_address = (uint32_t)frame->mtval;
    c5vrx2_fault_pc = (uint32_t)frame->mepc;
    REG32(PARLIO_TX_CLOCK) &= ~PARLIO_CLK_EN;
    REG32(DUMP_CTRL) &= ~CTRL_ENABLE;
    fence_io();
    if (c5vrx2_stage >= 2u) {
        REG32(HP_SRAM_USAGE) = c5vrx2_saved_ownership;
        fence_io();
    }
    c5vrx2_state = STATE_ERROR;
    c5vrx2_command = CMD_NONE;
    for (;;) __asm__ __volatile__("nop");
}

static void run_continuous(void)
{
    c5vrx2_runs++;
    c5vrx2_rearms = 0u;
    c5vrx2_rearm_failures = 0u;
    c5vrx2_blocks = 0u;
    c5vrx2_gap_cycles_last = 0u;
    c5vrx2_gap_cycles_max = 0u;
    c5vrx2_gap_cycles_total = 0u;
    c5vrx2_stage = 1u;

    c5vrx2_saved_ownership = REG32(HP_SRAM_USAGE);
    REG32(HP_SRAM_USAGE) =
        (c5vrx2_saved_ownership & 0xfffef0ffu) | 0x00010200u;
    fence_io();
    c5vrx2_stage = 2u;

    rearm_now();
    c5vrx2_stage = 3u;

    /* Start PARLIO only after half a real 16K generation has been written. */
    uint32_t previous = pointer();
    uint32_t lead = 0u;
    const uint32_t lead_start = cycles();
    while (lead < 8192u) {
        const uint32_t current = pointer();
        if (current > previous) {
            lead += current - previous;
            previous = current;
        }
        if ((uint32_t)(cycles() - lead_start) > cycles_for_us(100000u))
            goto fail;
    }

    REG32(PARLIO_TX_CLOCK) |= PARLIO_CLK_EN;
    fence_io();
    c5vrx2_state = STATE_RUNNING;
    c5vrx2_stage = 4u;

    uint32_t last_progress = cycles();
    for (;;) {
        if (c5vrx2_command == CMD_STOP) goto stopped;
        const uint32_t current = pointer();
        const uint32_t control = REG32(DUMP_CTRL);

        if ((control & CTRL_DONE) != 0u && current == PTR_MASK) {
            const uint32_t completed_at = cycles();

            /* CRITICAL ORDERING: restart first. Old C5VRX did block/phase/
             * consumer bookkeeping before this point. C5VRX-2 does none. */
            rearm_now();

            /* Only after START has been pulsed do we wait/measure/publish. */
            for (;;) {
                const uint32_t restarted = pointer();
                if (restarted != PTR_MASK) {
                    const uint32_t gap = cycles() - completed_at;
                    c5vrx2_gap_cycles_last = gap;
                    c5vrx2_gap_cycles_total += gap;
                    if (gap > c5vrx2_gap_cycles_max)
                        c5vrx2_gap_cycles_max = gap;
                    c5vrx2_rearms++;
                    c5vrx2_blocks++;
                    c5vrx2_last_pointer = restarted;
                    last_progress = cycles();
                    break;
                }
                if ((uint32_t)(cycles() - completed_at) > cycles_for_us(5000u)) {
                    c5vrx2_rearm_failures++;
                    goto fail;
                }
            }
        }

        /* Hardware-stall watchdog only. This is not RF/VTX/video detection. */
        if ((uint32_t)(cycles() - last_progress) > cycles_for_us(250000u))
            goto fail;
    }

fail:
    c5vrx2_rearm_failures++;
    c5vrx2_state = STATE_ERROR;
    goto restore;
stopped:
    c5vrx2_state = STATE_STOPPED;
restore:
    REG32(PARLIO_TX_CLOCK) &= ~PARLIO_CLK_EN;
    REG32(DUMP_CTRL) &= ~CTRL_ENABLE;
    fence_io();
    REG32(HP_SRAM_USAGE) = c5vrx2_saved_ownership;
    fence_io();
    c5vrx2_stage = 0u;
    c5vrx2_command = CMD_NONE;
}

int main(void)
{
    c5vrx2_fault_cause = 0u;
    c5vrx2_fault_address = 0u;
    c5vrx2_fault_pc = 0u;
    c5vrx2_stage = 0u;
    c5vrx2_command = CMD_NONE;
    c5vrx2_state = STATE_READY;
    for (;;) {
        if (c5vrx2_command == CMD_CONTINUOUS) {
            c5vrx2_command = CMD_NONE;
            run_continuous();
        }
        __asm__ __volatile__("nop");
    }
}
