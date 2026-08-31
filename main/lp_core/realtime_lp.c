#include <stdint.h>
#include "riscv/rvruntime-frames.h"
#include "ulp_lp_core_cpu_freq_shared.h"

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define DUMP_CTRL        0x600a9004u
#define DUMP_PTR         0x600a9008u
#define HP_SRAM_USAGE    0x60095004u
#define PARLIO_TX_CLOCK  0x600960b4u

#define CTRL_ENABLE      0x80000000u
#define CTRL_START       0x00080000u
#define CTRL_DONE        0x00040000u
#define PTR_MASK         0x00003fffu
#define PARLIO_CLK_EN    0x00040000u

#define CMD_NONE       0u
#define CMD_CONTINUOUS 1u
#define CMD_STOP       2u

#define STATE_BOOT     0u
#define STATE_READY    1u
#define STATE_RUNNING  2u
#define STATE_ERROR    3u
#define STATE_STOPPED  4u

#define TELEMETRY_MAGIC 0x43355232u /* "C5R2" */
#define DIAGNOSTIC_BLOCK_LIMIT 256u
#define WRITER_STALL_US 250000u
#define REARM_ACK_US 5000u

#define FAIL_NONE         0u
#define FAIL_INITIAL_LEAD 1u
#define FAIL_WRITER_STALL 2u
#define FAIL_DISABLE_ACK  3u
#define FAIL_RESTART_ACK  4u

volatile uint32_t c5vrx2_command;
volatile uint32_t c5vrx2_state;
volatile uint32_t c5vrx2_runs;
volatile uint32_t c5vrx2_rearms;
volatile uint32_t c5vrx2_rearm_failures;
volatile uint32_t c5vrx2_blocks;
volatile uint32_t c5vrx2_gap_cycles_last;
volatile uint32_t c5vrx2_gap_cycles_min;
volatile uint32_t c5vrx2_gap_cycles_max;
volatile uint32_t c5vrx2_gap_cycles_total;
volatile uint32_t c5vrx2_fill_cycles_last;
volatile uint32_t c5vrx2_fill_cycles_min;
volatile uint32_t c5vrx2_fill_cycles_max;
volatile uint32_t c5vrx2_fill_cycles_total;
volatile uint32_t c5vrx2_telemetry_magic;
volatile uint32_t c5vrx2_last_pointer;
volatile uint32_t c5vrx2_saved_ownership;
volatile uint32_t c5vrx2_fault_cause;
volatile uint32_t c5vrx2_fault_address;
volatile uint32_t c5vrx2_fault_pc;
volatile uint32_t c5vrx2_stage;
volatile uint32_t c5vrx2_observed_words;
volatile uint32_t c5vrx2_observed_wraps;
volatile uint32_t c5vrx2_observed_cycles;
volatile uint32_t c5vrx2_pointer_changes;
volatile uint32_t c5vrx2_done_seen;
volatile uint32_t c5vrx2_fail_reason;
volatile uint32_t c5vrx2_fail_control;
volatile uint32_t c5vrx2_fail_pointer_mode;
volatile uint32_t c5vrx2_start_control;

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

static inline uint32_t pointer_mode(void)
{
    return REG32(DUMP_PTR);
}

static inline uint32_t pointer(void)
{
    return pointer_mode() & PTR_MASK;
}

/* Disable from a clean control image.  DONE is an observed status bit, but
 * carrying its sampled 1 back into every RMW write made the physical writer
 * intermittently remain terminal at 0x80044000. */
static inline bool arm_writer_direct(void)
{
    uint32_t c = REG32(DUMP_CTRL) &
                 ~(CTRL_ENABLE | CTRL_START | CTRL_DONE);
    REG32(DUMP_CTRL) = c;
    fence_io();

    const uint32_t disabled_at = cycles();
    while ((REG32(DUMP_CTRL) & (CTRL_ENABLE | CTRL_DONE)) != 0u) {
        if ((uint32_t)(cycles() - disabled_at) > cycles_for_us(REARM_ACK_US))
            return false;
    }

    c |= CTRL_ENABLE;
    REG32(DUMP_CTRL) = c;
    REG32(DUMP_CTRL) = c | CTRL_START;
    REG32(DUMP_CTRL) = c;
    fence_io();
    c5vrx2_start_control = REG32(DUMP_CTRL);
    return (c5vrx2_start_control & CTRL_ENABLE) != 0u &&
           (c5vrx2_start_control & CTRL_DONE) == 0u;
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
    uint32_t final_state = STATE_ERROR;
    c5vrx2_runs++;
    c5vrx2_rearms = 0u;
    c5vrx2_rearm_failures = 0u;
    c5vrx2_blocks = 0u;
    c5vrx2_gap_cycles_last = 0u;
    c5vrx2_gap_cycles_min = UINT32_MAX;
    c5vrx2_gap_cycles_max = 0u;
    c5vrx2_gap_cycles_total = 0u;
    c5vrx2_fill_cycles_last = 0u;
    c5vrx2_fill_cycles_min = UINT32_MAX;
    c5vrx2_fill_cycles_max = 0u;
    c5vrx2_fill_cycles_total = 0u;
    c5vrx2_telemetry_magic = TELEMETRY_MAGIC;
    c5vrx2_observed_words = 0u;
    c5vrx2_observed_wraps = 0u;
    c5vrx2_observed_cycles = 0u;
    c5vrx2_pointer_changes = 0u;
    c5vrx2_done_seen = 0u;
    c5vrx2_fail_reason = FAIL_NONE;
    c5vrx2_fail_control = 0u;
    c5vrx2_fail_pointer_mode = 0u;
    c5vrx2_start_control = 0u;
    c5vrx2_stage = 1u;

    c5vrx2_saved_ownership = REG32(HP_SRAM_USAGE);
    REG32(HP_SRAM_USAGE) =
        (c5vrx2_saved_ownership & 0xfffef0ffu) | 0x00010200u;
    fence_io();
    c5vrx2_stage = 2u;

    if (!arm_writer_direct()) {
        c5vrx2_fail_reason = FAIL_DISABLE_ACK;
        goto fail;
    }
    c5vrx2_stage = 3u;

    uint32_t previous = pointer();
    uint32_t lead = 0u;
    const uint32_t lead_start = cycles();
    while (lead < 8192u) {
        const uint32_t current = pointer();
        if (current != previous) {
            lead += (current - previous) & PTR_MASK;
            previous = current;
        }
        if ((uint32_t)(cycles() - lead_start) > cycles_for_us(WRITER_STALL_US)) {
            c5vrx2_fail_reason = FAIL_INITIAL_LEAD;
            goto fail;
        }
    }

    REG32(PARLIO_TX_CLOCK) |= PARLIO_CLK_EN;
    fence_io();
    c5vrx2_state = STATE_RUNNING;
    c5vrx2_stage = 4u;

    uint32_t fill_started = lead_start;
    uint32_t last_progress = cycles();
    for (;;) {
        if (c5vrx2_command == CMD_STOP) goto stopped;
        const uint32_t now = cycles();
        const uint32_t current = pointer();
        const uint32_t control = REG32(DUMP_CTRL);

        if ((control & CTRL_DONE) != 0u) c5vrx2_done_seen = 1u;
        if (current != previous) {
            c5vrx2_last_pointer = current;
            previous = current;
            last_progress = now;
        }

        if ((control & CTRL_DONE) != 0u && current == PTR_MASK) {
            const uint32_t completed_at = now;
            const uint32_t fill = completed_at - fill_started;
            c5vrx2_fill_cycles_last = fill;
            c5vrx2_fill_cycles_total += fill;
            if (fill < c5vrx2_fill_cycles_min)
                c5vrx2_fill_cycles_min = fill;
            if (fill > c5vrx2_fill_cycles_max)
                c5vrx2_fill_cycles_max = fill;
            c5vrx2_blocks++;

            if (!arm_writer_direct()) {
                c5vrx2_fail_reason = FAIL_DISABLE_ACK;
                goto fail;
            }

            for (;;) {
                const uint32_t restarted = pointer();
                if (restarted != PTR_MASK) {
                    const uint32_t restarted_at = cycles();
                    const uint32_t gap = restarted_at - completed_at;
                    c5vrx2_gap_cycles_last = gap;
                    c5vrx2_gap_cycles_total += gap;
                    if (gap < c5vrx2_gap_cycles_min)
                        c5vrx2_gap_cycles_min = gap;
                    if (gap > c5vrx2_gap_cycles_max)
                        c5vrx2_gap_cycles_max = gap;
                    c5vrx2_rearms++;
                    c5vrx2_last_pointer = restarted;
                    previous = restarted;
                    fill_started = restarted_at;
                    last_progress = restarted_at;
                    break;
                }
                if ((uint32_t)(cycles() - completed_at) >
                    cycles_for_us(REARM_ACK_US)) {
                    c5vrx2_fail_reason = FAIL_RESTART_ACK;
                    goto fail;
                }
            }

            if (c5vrx2_blocks >= DIAGNOSTIC_BLOCK_LIMIT) goto stopped;
        }

        /* Writer-stall watchdog only; VTX/video content is never inspected. */
        if ((uint32_t)(now - last_progress) > cycles_for_us(WRITER_STALL_US)) {
            c5vrx2_fail_reason = FAIL_WRITER_STALL;
            goto fail;
        }
    }

fail:
    c5vrx2_rearm_failures++;
    c5vrx2_fail_control = REG32(DUMP_CTRL);
    c5vrx2_fail_pointer_mode = pointer_mode();
    final_state = STATE_ERROR;
    goto restore;
stopped:
    final_state = STATE_STOPPED;
restore:
    REG32(PARLIO_TX_CLOCK) &= ~PARLIO_CLK_EN;
    REG32(DUMP_CTRL) &= ~CTRL_ENABLE;
    fence_io();
    REG32(HP_SRAM_USAGE) = c5vrx2_saved_ownership;
    fence_io();
    c5vrx2_stage = 0u;
    c5vrx2_command = CMD_NONE;
    fence_io();
    c5vrx2_state = final_state;
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
