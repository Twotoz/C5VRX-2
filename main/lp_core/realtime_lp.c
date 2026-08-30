#include <stdint.h>
#include "riscv/rvruntime-frames.h"
#include "ulp_lp_core_cpu_freq_shared.h"

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define DUMP_CTRL        0x600a9004u
#define DUMP_PTR         0x600a9008u
#define PAU_REGDMA_CONF  0x60093000u
#define PAU_INT_RAW      0x6009301cu
#define PAU_INT_CLR      0x60093020u
#define HP_SRAM_USAGE    0x60095004u
#define PARLIO_TX_CLOCK  0x600960b4u

#define CTRL_ENABLE      0x80000000u
#define CTRL_START       0x00080000u
#define CTRL_DONE        0x00040000u
#define PTR_MASK         0x00003fffu
#define PARLIO_CLK_EN    0x00040000u

#define PAU_START        0x00000008u
#define PAU_TO_MEM       0x00000010u
#define PAU_LINK_SEL_M   0x000001e0u
#define PAU_LINK3        0x00000060u
#define PAU_DONE_RAW     0x00000001u
#define PAU_ERROR_RAW    0x00000002u
#define PAU_TIMEOUT_CYCLES 8192u

#define REGDMA_WRITE_HEAD     0x40020000u
#define REGDMA_WRITE_HEAD_EOF 0xc0020000u

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
volatile uint32_t c5vrx2_regdma_link_root;
volatile uint32_t c5vrx2_regdma_nodes[4][7];

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

static void prepare_regdma_chain(void)
{
    for (uint32_t i = 0; i < 4u; ++i) {
        for (uint32_t j = 0; j < 7u; ++j) c5vrx2_regdma_nodes[i][j] = 0u;
        c5vrx2_regdma_nodes[i][2] =
            i == 3u ? REGDMA_WRITE_HEAD_EOF : REGDMA_WRITE_HEAD;
        c5vrx2_regdma_nodes[i][3] = i == 3u ? 0u :
            (uint32_t)(uintptr_t)&c5vrx2_regdma_nodes[i + 1u][2];
        c5vrx2_regdma_nodes[i][4] = DUMP_CTRL;
    }
    /* Exact proven sequence: ENABLE 0 -> ENABLE 1 -> START 1 -> START 0. */
    c5vrx2_regdma_nodes[0][5] = 0u;
    c5vrx2_regdma_nodes[0][6] = CTRL_ENABLE;
    c5vrx2_regdma_nodes[1][5] = CTRL_ENABLE;
    c5vrx2_regdma_nodes[1][6] = CTRL_ENABLE;
    c5vrx2_regdma_nodes[2][5] = CTRL_START;
    c5vrx2_regdma_nodes[2][6] = CTRL_START;
    c5vrx2_regdma_nodes[3][5] = 0u;
    c5vrx2_regdma_nodes[3][6] = CTRL_START;
    c5vrx2_regdma_link_root =
        (uint32_t)(uintptr_t)&c5vrx2_regdma_nodes[0][2];
    fence_io();
}

/* One-time startup uses direct writes. Every realtime 16K boundary below uses
 * the physically proven PAU/REGDMA chain instead of making the 48 MHz LP core
 * execute the four modem writes itself. */
static inline void start_writer_once(void)
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

static inline bool rearm_regdma_now(void)
{
    REG32(PAU_INT_CLR) = PAU_DONE_RAW | PAU_ERROR_RAW;
    uint32_t conf = REG32(PAU_REGDMA_CONF);
    conf &= ~(PAU_START | PAU_TO_MEM | PAU_LINK_SEL_M);
    conf |= PAU_LINK3;
    REG32(PAU_REGDMA_CONF) = conf;
    fence_io();
    REG32(PAU_REGDMA_CONF) = conf | PAU_START;
    fence_io();

    const uint32_t started = cycles();
    uint32_t raw;
    do {
        raw = REG32(PAU_INT_RAW);
        if (raw & PAU_ERROR_RAW) break;
    } while (!(raw & PAU_DONE_RAW) &&
             (uint32_t)(cycles() - started) < PAU_TIMEOUT_CYCLES);

    REG32(PAU_REGDMA_CONF) = conf & ~PAU_LINK_SEL_M;
    if ((raw & PAU_ERROR_RAW) == 0u) REG32(PAU_INT_CLR) = PAU_DONE_RAW;
    fence_io();
    return (raw & PAU_DONE_RAW) != 0u && (raw & PAU_ERROR_RAW) == 0u;
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

    start_writer_once();
    c5vrx2_stage = 3u;

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

            /* HOT PATH: after DONE+terminal pointer, the next operation is the
             * REGDMA restart. No block-rate math, consumer tracking or logs. */
            if (!rearm_regdma_now()) {
                c5vrx2_rearm_failures++;
                goto fail_no_increment;
            }

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
                    goto fail_no_increment;
                }
            }
        }

        /* Writer-stall watchdog only; VTX/video content is never inspected. */
        if ((uint32_t)(cycles() - last_progress) > cycles_for_us(250000u))
            goto fail;
    }

fail:
    c5vrx2_rearm_failures++;
fail_no_increment:
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
    prepare_regdma_chain();
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
