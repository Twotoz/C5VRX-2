#include "continuous_iq.h"

#include <string.h>

#include "esp_timer.h"

#include "wifi5.h"
#include "startup_trace.h"

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))

#define DUMP_CTRL       0x600a9004u
#define DUMP_PTR_MODE   0x600a9008u
#define HP_SRAM_USAGE   0x60095004u

#define CTRL_ENABLE     0x80000000u
#define CTRL_START      0x00080000u
#define CTRL_DONE       0x00040000u
#define CTRL_DUMP_FIRST 0x00020000u
#define PTR_MASK        0x00003fffu
#define SELECTOR_MASK   0x01fe0000u
#define TX_START_SELECT 0x00060000u

#define READER_GUARD_WORDS 256u
#define RATE_MEASURE_US    4000u
#define OBSERVER_PERIOD_US 50u

typedef struct {
    bool running;
    bool span_out;
    bool next_span_continuous;
    uint32_t saved_sram_usage;
    uint32_t last_pointer;
    uint32_t physical_wraps;
    uint32_t overruns;
    uint32_t ambiguous_wraps;
    uint32_t discontinuity_epoch;
    uint32_t producer_start_count;
    uint32_t trigger_count;
    uint32_t rf_sample_rate_hz;
    bool done_latched;
    esp_timer_handle_t observer_timer;
    int64_t last_poll_us;
    uint64_t producer_words;
    uint64_t consumer_words;
    iq_span_t outstanding;
} continuous_iq_state_t;

static continuous_iq_state_t s_iq;

static inline void fence_io(void)
{
    __asm__ __volatile__("fence iorw, iorw" ::: "memory");
}

static inline uint32_t writer_pointer(void)
{
    return REG32(DUMP_PTR_MODE) & PTR_MASK;
}

static void note_discontinuity(void)
{
    s_iq.discontinuity_epoch++;
    s_iq.next_span_continuous = false;
}

/* This tracker is intentionally for a hot polling consumer.  A slow caller
 * cannot distinguish identical pointer values separated by one or more full
 * physical wraps, so elapsed time is used to reject that ambiguity rather
 * than fabricating continuity. */
static bool update_producer(void)
{
    const int64_t now = esp_timer_get_time();
    const uint32_t current = writer_pointer();
    const uint32_t delta = (current - s_iq.last_pointer) & PTR_MASK;
    const int64_t elapsed = now - s_iq.last_poll_us;

    if (s_iq.rf_sample_rate_hz != 0u && elapsed > 0) {
        const uint64_t expected =
            (uint64_t)s_iq.rf_sample_rate_hz * (uint64_t)elapsed / 1000000u;
        if (expected >= C5VRX2_RF_WORDS) {
            s_iq.ambiguous_wraps++;
            s_iq.last_pointer = current;
            s_iq.last_poll_us = now;
            note_discontinuity();
            return false;
        }
    }

    if (current < s_iq.last_pointer && delta != 0u)
        s_iq.physical_wraps++;
    s_iq.producer_words += delta;
    s_iq.last_pointer = current;
    s_iq.last_poll_us = now;
    return true;
}

static void observe_producer(void *argument)
{
    (void)argument;
    if (!s_iq.running) return;
    (void)update_producer();
    const bool done = (REG32(DUMP_CTRL) & CTRL_DONE) != 0u;
    if (done && !s_iq.done_latched) s_iq.trigger_count++;
    s_iq.done_latched = done;
}

static esp_err_t measure_rate(void)
{
    const int64_t begin = esp_timer_get_time();
    int64_t now = begin;
    uint32_t last = writer_pointer();
    uint64_t words = 0u;
    uint32_t wraps = 0u;

    do {
        const uint32_t current = writer_pointer();
        const uint32_t delta = (current - last) & PTR_MASK;
        if (current < last && delta != 0u) wraps++;
        words += delta;
        last = current;
        if ((REG32(DUMP_CTRL) & CTRL_DONE) != 0u)
            return ESP_ERR_INVALID_STATE;
        now = esp_timer_get_time();
    } while ((uint32_t)(now - begin) < RATE_MEASURE_US);

    const uint32_t elapsed = (uint32_t)(now - begin);
    if (elapsed == 0u || words < C5VRX2_RF_WORDS)
        return ESP_ERR_TIMEOUT;

    s_iq.rf_sample_rate_hz =
        (uint32_t)(words * 1000000ull / (uint64_t)elapsed);
    s_iq.last_pointer = last;
    s_iq.last_poll_us = now;
    /* Keep the logical low bits aligned with the physical SRAM address.  The
     * measured word count is a cadence observation, not an absolute ring
     * position. */
    s_iq.producer_words = last;
    s_iq.physical_wraps = wraps;
    s_iq.consumer_words = last;
    return ESP_OK;
}

esp_err_t continuous_iq_start(void)
{
    c5vrx2_trace_stage(101u, ESP_OK);
    if (s_iq.running) return ESP_ERR_INVALID_STATE;
    if (!c5vrx2_rf_dump_memory_reserved()) return ESP_ERR_INVALID_STATE;
    c5vrx2_trace_stage(102u, ESP_OK);

    esp_err_t err = c5vrx2_rf_dump_prepare_mode0();
    if (err != ESP_OK) return err;
    c5vrx2_trace_stage(103u, ESP_OK);

    memset(&s_iq, 0, sizeof(s_iq));
    s_iq.next_span_continuous = false;
    c5vrx2_rf_dump_guards_init();
    c5vrx2_trace_stage(104u, ESP_OK);

    /* C5 trigmode=TX_START (5) selects 0x00060000 in PTR_MODE.  The C5
     * v6.0.1 vendor routine ignores its historical dump_trig argument, so it
     * cannot itself expose a dump-first TX_START combination.  Preserve the
     * verified TX_START selector and explicitly request the historical
     * pre-trigger state with CTRL_DUMP_FIRST; no software START is issued. */
    uint32_t selector = REG32(DUMP_PTR_MODE);
    selector = (selector & ~SELECTOR_MASK) | TX_START_SELECT;
    REG32(DUMP_PTR_MODE) = selector;

    uint32_t control = REG32(DUMP_CTRL);
    control &= ~(CTRL_ENABLE | CTRL_START | CTRL_DONE);
    control |= CTRL_DUMP_FIRST;
    control = (control & ~0x0001ffffu) | C5VRX2_RF_WORDS;
    REG32(DUMP_CTRL) = control;

    /* Reproduce the vendor wrapper's SRAM grant once.  The linker/heap
     * reservation keeps all HP stacks and objects outside this 64 KiB bank. */
    s_iq.saved_sram_usage = REG32(HP_SRAM_USAGE);
    REG32(HP_SRAM_USAGE) =
        (s_iq.saved_sram_usage & 0xfffef0ffu) | 0x00010200u;
    fence_io();
    c5vrx2_trace_stage(105u, ESP_OK);

    /* wifi5_start_a1() keeps PHY/RX and the 5 GHz tune alive. Re-apply its
     * one-shot LMAC TX gate immediately before arming, so TX_START cannot
     * occur in the interval between RF setup and dump ENABLE. */
    err = c5vrx2_wifi5_lock_rx_only();
    if (err != ESP_OK) {
        REG32(HP_SRAM_USAGE) = s_iq.saved_sram_usage;
        return err;
    }
    c5vrx2_trace_stage(106u, ESP_OK);

    /* DUMP FIRST + never-occurring TX_START starts by ENABLE only.  A START
     * pulse here would select the finite software-trigger lifecycle again. */
    REG32(DUMP_CTRL) = control | CTRL_ENABLE;
    fence_io();
    s_iq.running = true;
    s_iq.producer_start_count = 1u;
    s_iq.last_pointer = writer_pointer();
    s_iq.last_poll_us = esp_timer_get_time();
    c5vrx2_trace_stage(107u, ESP_OK);

    c5vrx2_trace_stage(108u, ESP_OK);
    err = measure_rate();
    if (err != ESP_OK) {
        c5vrx2_trace_stage(108u, err);
        (void)continuous_iq_stop();
        return err;
    }
    c5vrx2_trace_stage(109u, ESP_OK);
    const esp_timer_create_args_t observer_args = {
        .callback = observe_producer,
        .name = "iq_ptr",
    };
    err = esp_timer_create(&observer_args, &s_iq.observer_timer);
    if (err == ESP_OK)
        err = esp_timer_start_periodic(s_iq.observer_timer,
                                       OBSERVER_PERIOD_US);
    if (err != ESP_OK) {
        (void)continuous_iq_stop();
        return err;
    }
    c5vrx2_trace_stage(110u, ESP_OK);
    return ESP_OK;
}

bool continuous_iq_acquire(iq_span_t *span)
{
    if (!span || !s_iq.running || s_iq.span_out) return false;
    if (!update_producer()) return false;

    uint64_t available = s_iq.producer_words - s_iq.consumer_words;
    if (available >= C5VRX2_RF_WORDS - READER_GUARD_WORDS) {
        s_iq.overruns++;
        note_discontinuity();
        s_iq.consumer_words = s_iq.producer_words - C5VRX2_RF_WORDS / 2u;
        available = C5VRX2_RF_WORDS / 2u;
    }
    if (available <= READER_GUARD_WORDS) return false;

    const uint32_t physical =
        (uint32_t)(s_iq.consumer_words & (C5VRX2_RF_WORDS - 1u));
    size_t words = (size_t)(available - READER_GUARD_WORDS);
    const size_t until_wrap = C5VRX2_RF_WORDS - physical;
    if (words > until_wrap) words = until_wrap;

    *span = (iq_span_t) {
        .data = (const volatile uint32_t *)(uintptr_t)
                    (C5VRX2_RF_DUMP_BASE + physical * sizeof(uint32_t)),
        .words = words,
        .absolute_start = s_iq.consumer_words,
        .discontinuity_epoch = s_iq.discontinuity_epoch,
        .continuous_from_previous = s_iq.next_span_continuous,
    };
    s_iq.outstanding = *span;
    s_iq.span_out = true;
    return true;
}

void continuous_iq_release(const iq_span_t *span)
{
    if (!span || !s_iq.span_out) return;
    if (span->absolute_start != s_iq.outstanding.absolute_start ||
        span->words > s_iq.outstanding.words) {
        note_discontinuity();
        s_iq.span_out = false;
        return;
    }
    s_iq.consumer_words += span->words;
    s_iq.next_span_continuous = true;
    s_iq.span_out = false;
}

esp_err_t continuous_iq_stop(void)
{
    if (!s_iq.running) return ESP_ERR_INVALID_STATE;
    if (s_iq.observer_timer) {
        (void)esp_timer_stop(s_iq.observer_timer);
        (void)esp_timer_delete(s_iq.observer_timer);
        s_iq.observer_timer = NULL;
    }
    REG32(DUMP_CTRL) &= ~CTRL_ENABLE;
    fence_io();
    REG32(HP_SRAM_USAGE) = s_iq.saved_sram_usage;
    fence_io();
    s_iq.running = false;
    s_iq.span_out = false;
    note_discontinuity();
    return ESP_OK;
}

void continuous_iq_get_stats(continuous_iq_stats_t *stats)
{
    if (!stats) return;
    *stats = (continuous_iq_stats_t) {
        .rf_sample_rate_hz = s_iq.rf_sample_rate_hz,
        .writer_pointer = writer_pointer(),
        .dump_control = REG32(DUMP_CTRL),
        .producer_words = s_iq.producer_words,
        .consumer_words = s_iq.consumer_words,
        .physical_wraps = s_iq.physical_wraps,
        .overruns = s_iq.overruns,
        .ambiguous_wraps = s_iq.ambiguous_wraps,
        .discontinuity_epoch = s_iq.discontinuity_epoch,
        .producer_start_count = s_iq.producer_start_count,
        .rearm_count = 0u,
        .trigger_count = s_iq.trigger_count,
    };
}

uint32_t continuous_iq_sample_rate_hz(void)
{
    return s_iq.rf_sample_rate_hz;
}

const void *continuous_iq_ring_base(void)
{
    return (const void *)(uintptr_t)C5VRX2_RF_DUMP_BASE;
}

size_t continuous_iq_ring_bytes(void)
{
    return C5VRX2_RF_WORDS * sizeof(uint32_t);
}

esp_err_t continuous_iq_wait_base_lead(uint32_t lead_words,
                                       uint32_t tolerance_words,
                                       uint32_t timeout_us)
{
    if (!s_iq.running || lead_words >= C5VRX2_RF_WORDS ||
        tolerance_words >= C5VRX2_RF_WORDS)
        return ESP_ERR_INVALID_ARG;
    const int64_t begin = esp_timer_get_time();
    do {
        const uint32_t pointer = writer_pointer();
        const uint32_t distance =
            (pointer - lead_words) & (C5VRX2_RF_WORDS - 1u);
        if (distance <= tolerance_words) return ESP_OK;
        if ((REG32(DUMP_CTRL) & (CTRL_ENABLE | CTRL_DONE)) != CTRL_ENABLE)
            return ESP_ERR_INVALID_STATE;
    } while ((uint32_t)(esp_timer_get_time() - begin) < timeout_us);
    return ESP_ERR_TIMEOUT;
}
