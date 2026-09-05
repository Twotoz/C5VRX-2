#include "diagnostics.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/parlio_tx.h"
#include "esp_async_memcpy.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "continuous_iq.h"
#include "rf_dump.h"
#include "startup_trace.h"

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define DUMP_CTRL     0x600a9004u
#define DUMP_PTR_MODE 0x600a9008u
#define PTR_MASK      0x00003fffu

#define RF_WRAP_SOAK_TARGET 10000u
#define RF_PHASE_WINDOWS      256u
#define RF_DMA_PROBE_BYTES   2048u
#define RF_DMA_TIMEOUT_US    2000u

#define PAL_RATE_HZ          20000000u
#define PAL_HALF_SAMPLES     640u
#define PAL_FRAME_HALVES     1250u
#define PAL_FIELD_HALVES     625u
#define PAL_CHUNK_HALVES     40u
#define PAL_CHUNK_SAMPLES    (PAL_HALF_SAMPLES * PAL_CHUNK_HALVES)
#define PAL_HSYNC            94u
#define PAL_EQ               47u
#define PAL_BROAD            546u
#define PAL_ACTIVE_START     210u
#define PAL_ACTIVE_END       1250u
#define PAL_SYNC_CODE        0u
#define PAL_BLACK_CODE       18u
#define PAL_WHITE_CODE       62u

extern void adctrig(int32_t smp_num_aft_trig, int32_t trigmode,
                    int32_t trigcase, int32_t sample_80m,
                    int32_t dump_trig, int32_t rx_gain_mode,
                    int32_t rx_gain, int32_t rx_gain0,
                    int32_t rx_gain0_wait_us);
extern void set_dump_mode(int mode);

static const char *TAG = "c5vrx2_diag";
static parlio_tx_unit_handle_t s_diag_tx;
static uint8_t *s_pal_buffers[2];
static TaskHandle_t s_pal_task;
static uint16_t s_pal_next_half;
static bool s_pal_colour;
static int8_t s_sine[256];

static esp_err_t configure_dac_tx(uint32_t rate, size_t max_transfer,
                                  unsigned queue_depth)
{
    const int pins[6] = {23, 24, 11, 12, 8, 9};
    for (unsigned i = 0; i < 6u; ++i) {
        esp_err_t err = gpio_set_drive_capability((gpio_num_t)pins[i],
                                                  GPIO_DRIVE_CAP_3);
        if (err != ESP_OK) return err;
    }
    const parlio_tx_unit_config_t cfg = {
        .clk_src = PARLIO_CLK_SRC_DEFAULT,
        .clk_in_gpio_num = -1,
        .input_clk_src_freq_hz = 0,
        .output_clk_freq_hz = rate,
        .data_width = 8,
        .data_gpio_nums = {23, 24, 11, 12, 8, 9, -1, -1},
        .clk_out_gpio_num = -1,
        .valid_gpio_num = -1,
        .trans_queue_depth = queue_depth,
        .max_transfer_size = max_transfer,
        .dma_burst_size = 32,
        .shift_edge = PARLIO_SHIFT_EDGE_NEG,
        .bit_pack_order = PARLIO_BIT_PACK_ORDER_LSB,
    };
    esp_err_t err = parlio_new_tx_unit(&cfg, &s_diag_tx);
    if (err == ESP_OK) err = parlio_tx_unit_enable(s_diag_tx);
    return err;
}

static esp_err_t transmit_loop(const void *buffer, size_t bytes,
                               unsigned idle)
{
    const parlio_transmit_config_t cfg = {
        .idle_value = idle,
        .bitscrambler_program = NULL,
        .flags.loop_transmission = true,
    };
    return parlio_tx_unit_transmit(s_diag_tx, buffer, bytes * 8u, &cfg);
}

esp_err_t c5vrx2_av_static_diagnostic_start(unsigned code)
{
    if (code > 63u) return ESP_ERR_INVALID_ARG;
    static uint8_t codes[256] __attribute__((aligned(64)));
    memset(codes, (int)code, sizeof(codes));
    esp_err_t err = configure_dac_tx(1000000u, sizeof(codes), 1u);
    if (err == ESP_OK) err = transmit_loop(codes, sizeof(codes), code);
    if (err == ESP_OK)
        ESP_LOGW(TAG, "AV STATIC code=%u (test 0,18,31,32,62,63)", code);
    return err;
}

static uint8_t clamp_code(int value)
{
    if (value < 0) return 0u;
    if (value > 63) return 63u;
    return (uint8_t)value;
}

static void render_pal_half(uint8_t *dst, uint16_t half)
{
    const unsigned field_half = half % PAL_FIELD_HALVES;
    const bool first_half = field_half >= 15u && ((field_half - 15u) & 1u) == 0u;
    const unsigned line_number = field_half >= 15u ? (field_half - 15u) / 2u : 0u;
    memset(dst, PAL_BLACK_CODE, PAL_HALF_SAMPLES);

    unsigned pulse = 0u;
    if (field_half < 5u || (field_half >= 10u && field_half < 15u))
        pulse = PAL_EQ;
    else if (field_half < 10u)
        pulse = PAL_BROAD;
    else if (first_half)
        pulse = PAL_HSYNC;
    if (pulse != 0u) memset(dst, PAL_SYNC_CODE, pulse);
    if (field_half < 15u) return;

    const unsigned base = first_half ? 0u : PAL_HALF_SAMPLES;
    if (s_pal_colour && first_half) {
        /* NCO uses the actual 20 MHz output rate; no integer samples/cycle
         * assumption is made for the 4.43361875 MHz PAL burst. */
        uint32_t phase = (line_number & 1u) ? 0x40000000u : 0xc0000000u;
        const uint32_t step = (uint32_t)(4433618.75 * 4294967296.0 /
                                         (double)PAL_RATE_HZ);
        for (unsigned x = 115u; x < 165u; ++x) {
            dst[x] = clamp_code(PAL_BLACK_CODE +
                                (s_sine[phase >> 24u] * 5) / 127);
            phase += step;
        }
    }

    if (line_number < 24u || line_number >= 312u) return;
    for (unsigned p = 0; p < PAL_HALF_SAMPLES; ++p) {
        const unsigned line_pos = base + p;
        if (line_pos < PAL_ACTIVE_START || line_pos >= PAL_ACTIVE_END) continue;
        const unsigned x = line_pos - PAL_ACTIVE_START;
        const unsigned bar = x * 8u / (PAL_ACTIVE_END - PAL_ACTIVE_START);
        static const uint8_t luma[8] = {55, 50, 44, 39, 34, 29, 23, 18};
        int code = luma[bar];
        if (s_pal_colour) {
            static const int8_t chroma[8] = {0, 7, -7, 9, -9, 6, -6, 0};
            const uint64_t phase = (uint64_t)line_pos * 443361875ull *
                                   4294967296ull / (PAL_RATE_HZ * 100ull);
            code += (chroma[bar] * s_sine[(uint32_t)phase >> 24u]) / 127;
        }
        dst[p] = clamp_code(code);
    }
}

static void render_pal_chunk(uint8_t *buffer)
{
    for (unsigned i = 0; i < PAL_CHUNK_HALVES; ++i) {
        render_pal_half(buffer + i * PAL_HALF_SAMPLES, s_pal_next_half);
        s_pal_next_half++;
        if (s_pal_next_half == PAL_FRAME_HALVES) s_pal_next_half = 0u;
    }
}

static bool pal_buffer_switched(parlio_tx_unit_handle_t tx,
    const parlio_tx_buffer_switched_event_data_t *event, void *context)
{
    (void)tx;
    (void)context;
    BaseType_t wake = pdFALSE;
    unsigned index = event && event->old_buffer_addr == s_pal_buffers[1] ? 1u : 0u;
    if (s_pal_task)
        xTaskNotifyFromISR(s_pal_task, 1u << index, eSetBits, &wake);
    return wake == pdTRUE;
}

static void pal_refill_task(void *argument)
{
    (void)argument;
    for (;;) {
        uint32_t bits = 0u;
        (void)xTaskNotifyWait(0u, UINT32_MAX, &bits, portMAX_DELAY);
        for (unsigned i = 0; i < 2u; ++i) {
            if ((bits & (1u << i)) == 0u) continue;
            render_pal_chunk(s_pal_buffers[i]);
            if (transmit_loop(s_pal_buffers[i], PAL_CHUNK_SAMPLES,
                              PAL_BLACK_CODE) != ESP_OK) {
                ESP_LOGE(TAG, "PAL diagnostic DMA refill failed");
                vTaskDelete(NULL);
            }
        }
    }
}

esp_err_t c5vrx2_av_pal_diagnostic_start(bool colour)
{
    s_pal_colour = colour;
    for (unsigned i = 0; i < 256u; ++i)
        s_sine[i] = (int8_t)lrintf(127.0f * sinf((float)i *
                                                2.0f * 3.14159265f / 256.0f));
    for (unsigned i = 0; i < 2u; ++i) {
        s_pal_buffers[i] = heap_caps_malloc(PAL_CHUNK_SAMPLES,
            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_pal_buffers[i]) return ESP_ERR_NO_MEM;
        render_pal_chunk(s_pal_buffers[i]);
    }
    esp_err_t err = configure_dac_tx(PAL_RATE_HZ, PAL_CHUNK_SAMPLES, 2u);
    if (err != ESP_OK) return err;
    if (xTaskCreate(pal_refill_task, "pal_diag", 3072u, NULL, 20u,
                    &s_pal_task) != pdPASS) return ESP_ERR_NO_MEM;
    const parlio_tx_event_callbacks_t callbacks = {
        .on_buffer_switched = pal_buffer_switched,
    };
    if ((err = parlio_tx_unit_register_event_callbacks(
             s_diag_tx, &callbacks, NULL)) != ESP_OK) return err;
    if ((err = transmit_loop(s_pal_buffers[0], PAL_CHUNK_SAMPLES,
                             PAL_BLACK_CODE)) != ESP_OK) return err;
    if ((err = transmit_loop(s_pal_buffers[1], PAL_CHUNK_SAMPLES,
                             PAL_BLACK_CODE)) != ESP_OK) return err;
    ESP_LOGW(TAG, "AV synthetic PAL %s: 20 MHz, continuous double-buffer DMA",
             colour ? "colour bars + burst" : "monochrome bars");
    return ESP_OK;
}

static void oracle_task(void *argument)
{
    (void)argument;
    set_dump_mode(0);
    adctrig(16383, 5, 0, 1, 1, 0, 0, 0, 0);
    vTaskDelete(NULL);
}

esp_err_t c5vrx2_rf_oracle_diagnostic_start(void)
{
    if (!c5vrx2_rf_dump_memory_reserved()) return ESP_ERR_INVALID_STATE;
    if (xTaskCreate(oracle_task, "rf_oracle", 4096u, NULL, 8u, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    vTaskDelay(pdMS_TO_TICKS(2u));
    const int64_t begin = esp_timer_get_time();
    uint32_t last = REG32(DUMP_PTR_MODE) & PTR_MASK;
    uint32_t changes = 0u;
    uint32_t wraps = 0u;
    uint32_t ram_changes = 0u;
    uint32_t sample = REG32(C5VRX2_RF_DUMP_BASE);
    while (esp_timer_get_time() - begin < 250000) {
        const uint32_t pointer = REG32(DUMP_PTR_MODE) & PTR_MASK;
        if (pointer != last) changes++;
        if (pointer < last) wraps++;
        last = pointer;
        const uint32_t current = REG32(C5VRX2_RF_DUMP_BASE);
        if (current != sample) {
            ram_changes++;
            sample = current;
        }
    }
    ESP_LOGW(TAG,
             "RF ORACLE dump_first=1 tx_start=5 ctrl=0x%08x selector=0x%08x "
             "ptr=%u changes=%u wraps=%u ram_changes=%u",
             (unsigned)REG32(DUMP_CTRL), (unsigned)REG32(DUMP_PTR_MODE),
             (unsigned)last, (unsigned)changes, (unsigned)wraps,
             (unsigned)ram_changes);
    return changes && ram_changes ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static int16_t sign10(uint32_t value)
{
    value &= 0x3ffu;
    return (value & 0x200u) ? (int16_t)(value - 0x400u) : (int16_t)value;
}

static float phase_step(uint32_t previous, uint32_t current)
{
    const int pi = sign10(previous >> 10u);
    const int pq = sign10(previous);
    const int ci = sign10(current >> 10u);
    const int cq = sign10(current);
    const int32_t dot = ci * pi + cq * pq;
    const int32_t cross = cq * pi - ci * pq;
    return atan2f((float)cross, (float)dot);
}

static DMA_ATTR uint8_t s_rf_dma_probe_a[RF_DMA_PROBE_BYTES];
static DMA_ATTR uint8_t s_rf_dma_probe_b[RF_DMA_PROBE_BYTES];
static volatile bool s_rf_dma_done_a;
static volatile bool s_rf_dma_done_b;

static bool rf_dma_done(async_memcpy_handle_t handle,
                        async_memcpy_event_t *event, void *argument)
{
    (void)handle;
    (void)event;
    *(volatile bool *)argument = true;
    return false;
}

static esp_err_t rf_dma_copy_wait(async_memcpy_handle_t dma, void *destination,
                                  const void *source, volatile bool *done,
                                  uint32_t *elapsed_us)
{
    *done = false;
    const int64_t begin = esp_timer_get_time();
    esp_err_t err = esp_async_memcpy(dma, destination, (void *)source,
                                     RF_DMA_PROBE_BYTES, rf_dma_done,
                                     (void *)done);
    if (err != ESP_OK) return err;
    while (!*done &&
           (uint32_t)(esp_timer_get_time() - begin) < RF_DMA_TIMEOUT_US) {
        __asm__ __volatile__("nop");
    }
    *elapsed_us = (uint32_t)(esp_timer_get_time() - begin);
    return *done ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t c5vrx2_rf_dma_diagnostic_run(void)
{
    for (unsigned second = 10u; second != 0u; --second) {
        ESP_LOGW(TAG, "RF DMA diagnostic starts in %u seconds (USB settle)",
                 second);
        vTaskDelay(pdMS_TO_TICKS(1000u));
    }

    async_memcpy_handle_t dma = NULL;
    async_memcpy_config_t dma_config = ASYNC_MEMCPY_DEFAULT_CONFIG();
    dma_config.backlog = 2u;
    dma_config.weight = 1u;
    dma_config.dma_burst_size = 32u;
    esp_err_t err = esp_async_memcpy_install_gdma_ahb(&dma_config, &dma);
    if (err != ESP_OK) return err;

    memset(s_rf_dma_probe_a, 0xa5, sizeof(s_rf_dma_probe_a));
    memset(s_rf_dma_probe_b, 0x5a, sizeof(s_rf_dma_probe_b));
    err = continuous_iq_start();
    if (err != ESP_OK) {
        (void)esp_async_memcpy_uninstall(dma);
        return err;
    }

    const uint32_t pointer = REG32(DUMP_PTR_MODE) & PTR_MASK;
    uint32_t source_word = (pointer + C5VRX2_RF_WORDS / 2u) & PTR_MASK;
    source_word &= ~((RF_DMA_PROBE_BYTES / sizeof(uint32_t)) - 1u);
    const void *source = (const void *)(uintptr_t)
        (C5VRX2_RF_DUMP_BASE + source_word * sizeof(uint32_t));

    uint32_t first_us = 0u;
    uint32_t second_us = 0u;
    const esp_err_t first_err =
        rf_dma_copy_wait(dma, s_rf_dma_probe_a, source, &s_rf_dma_done_a,
                         &first_us);
    /* At ~80 MS/s this exceeds one 16K traversal, so the same physical
     * address should contain a later RF generation. */
    const int64_t wait_begin = esp_timer_get_time();
    while ((uint32_t)(esp_timer_get_time() - wait_begin) < 300u) {
        __asm__ __volatile__("nop");
    }
    const esp_err_t second_err = first_err == ESP_OK
        ? rf_dma_copy_wait(dma, s_rf_dma_probe_b, source, &s_rf_dma_done_b,
                           &second_us)
        : ESP_ERR_INVALID_STATE;

    continuous_iq_stats_t stats;
    continuous_iq_get_stats(&stats);
    const esp_err_t stop_err = continuous_iq_stop();

    unsigned nonzero_a = 0u;
    unsigned nonzero_b = 0u;
    unsigned changed_between = 0u;
    for (unsigned i = 0; i < RF_DMA_PROBE_BYTES; ++i) {
        nonzero_a += s_rf_dma_probe_a[i] != 0u;
        nonzero_b += s_rf_dma_probe_b[i] != 0u;
        changed_between += s_rf_dma_probe_a[i] != s_rf_dma_probe_b[i];
    }
    const bool visible = first_err == ESP_OK && second_err == ESP_OK &&
                         nonzero_a != 0u && nonzero_b != 0u &&
                         changed_between != 0u;
    c5vrx2_trace_stage_detail(201u, first_err, first_us, 0u, 0u);
    c5vrx2_trace_stage_detail(202u, second_err, second_us, 0u, 0u);
    c5vrx2_trace_stage_detail(203u,
                              visible ? ESP_OK : ESP_ERR_NOT_SUPPORTED,
                              nonzero_a, nonzero_b, changed_between);
    for (unsigned report = 1u; report <= 30u; ++report) {
        ESP_LOGW(TAG,
                 "RF DMA VISIBILITY report=%u/30 src_word=%u first=%s/%uus "
                 "second=%s/%uus nonzero_a=%u nonzero_b=%u changed=%u "
                 "ptr=%u ctrl=0x%08x starts=%u rearms=%u triggers=%u "
                 "stop=%s verdict=%s",
                 report, (unsigned)source_word, esp_err_to_name(first_err),
                 (unsigned)first_us, esp_err_to_name(second_err),
                 (unsigned)second_us, nonzero_a, nonzero_b, changed_between,
                 (unsigned)stats.writer_pointer, (unsigned)stats.dump_control,
                 (unsigned)stats.producer_start_count,
                 (unsigned)stats.rearm_count, (unsigned)stats.trigger_count,
                 esp_err_to_name(stop_err),
                 visible ? "GDMA_CAN_READ_MAC_RING" : "GDMA_VIEW_BLOCKED");
        vTaskDelay(pdMS_TO_TICKS(1000u));
    }

    const esp_err_t uninstall_err = esp_async_memcpy_uninstall(dma);
    if (!visible) return ESP_ERR_NOT_SUPPORTED;
    if (stop_err != ESP_OK) return stop_err;
    return uninstall_err;
}

esp_err_t c5vrx2_rf_wrap_diagnostic_run(void)
{
    esp_err_t err = continuous_iq_start();
    if (err != ESP_OK) return err;
    const volatile uint32_t *ring = continuous_iq_ring_base();
    uint32_t previous_pointer = REG32(DUMP_PTR_MODE) & PTR_MASK;
    unsigned wraps = 0u;
    unsigned phase_windows = 0u;
    unsigned cpu_visible_words = 0u;
    double boundary_abs = 0.0;
    double neighbor_abs = 0.0;
    float worst_difference = 0.0f;
    while (wraps < RF_WRAP_SOAK_TARGET) {
        const uint32_t pointer = REG32(DUMP_PTR_MODE) & PTR_MASK;
        if (pointer < previous_pointer && pointer >= 16u && pointer < 128u) {
            if (phase_windows < RF_PHASE_WINDOWS) {
                uint32_t window[16];
                for (unsigned i = 0; i < 8u; ++i)
                    window[i] = ring[C5VRX2_RF_WORDS - 8u + i];
                for (unsigned i = 0; i < 8u; ++i) window[8u + i] = ring[i];
                for (unsigned i = 0; i < 16u; ++i)
                    cpu_visible_words += window[i] != 0u;
                const float boundary = phase_step(window[7], window[8]);
                float neighbors = 0.0f;
                for (unsigned i = 1u; i < 15u; ++i) {
                    if (i == 8u) continue;
                    neighbors += fabsf(phase_step(window[i - 1u], window[i]));
                }
                neighbors /= 13.0f;
                const float difference = fabsf(fabsf(boundary) - neighbors);
                if (difference > worst_difference) worst_difference = difference;
                boundary_abs += fabsf(boundary);
                neighbor_abs += neighbors;
                phase_windows++;
            }
            wraps++;
        }
        previous_pointer = pointer;
        if ((REG32(DUMP_CTRL) & 0x80040000u) != 0x80000000u) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
    }
    continuous_iq_stats_t stats;
    continuous_iq_get_stats(&stats);
    const bool writer_healthy =
        err == ESP_OK && wraps == RF_WRAP_SOAK_TARGET &&
        stats.producer_start_count == 1u && stats.rearm_count == 0u &&
        stats.trigger_count == 0u &&
        (stats.dump_control & 0x80040000u) == 0x80000000u;
    const esp_err_t stop_err = continuous_iq_stop();
    /* The guard banks are not reliably HP-readable while MAC_DUMP_ALLOC owns
     * them. Check their contents only after restoring HP ownership. */
    const bool guards_after_stop = c5vrx2_rf_dump_guards_valid();
    const bool healthy = writer_healthy && stop_err == ESP_OK && guards_after_stop;
    if (!healthy && err == ESP_OK) err = ESP_ERR_INVALID_STATE;
    ESP_LOGW(TAG,
             "RF WRAP SOAK observed=%u phase_windows=%u hw_wraps=%u "
             "starts=%u rearms=%u triggers=%u ctrl=0x%08x guards=%u "
             "cpu_visible_words=%u boundary_abs=%.6f neighbor_abs=%.6f "
             "worst_delta=%.6f verdict=%s",
             wraps, phase_windows, (unsigned)stats.physical_wraps,
             (unsigned)stats.producer_start_count, (unsigned)stats.rearm_count,
             (unsigned)stats.trigger_count, (unsigned)stats.dump_control,
             guards_after_stop, cpu_visible_words,
             phase_windows ? boundary_abs / phase_windows : 0.0,
             phase_windows ? neighbor_abs / phase_windows : 0.0,
             worst_difference,
             healthy ? (cpu_visible_words != 0u
                            ? "RING_SOAK_PASS_PHASE_NOT_PROVEN"
                            : "RING_SOAK_PASS_CPU_VIEW_BLOCKED")
                     : "FAIL");
    return err;
}
