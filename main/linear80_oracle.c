/* Bounded, RF-off hardware validation. Never part of live sample pacing. */
#include "sdkconfig.h"
#if CONFIG_C5VRX2_MODE_LINEAR80_ORACLE
#include <stdlib.h>
#include <string.h>
#include "driver/bitscrambler_loopback.h"
#include "driver/parlio_bitscrambler.h"
#include "driver/parlio_tx.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"
#include "soc/parl_io_struct.h"
#include "calibration.h"
#include "wbfm_q4.h"

#define INPUT_BYTES 16384u
#define OUTPUT_BYTES (INPUT_BYTES * 2u)
#define OUTPUT_CAPACITY (OUTPUT_BYTES + 64u)

static uint32_t fnv(const void *data, size_t n)
{
    const uint8_t *p=data;
    uint32_t h=2166136261u;
    while (n--) h=(h ^ *p++)*16777619u;
    return h;
}

static esp_err_t timed_tx(uint8_t *raw, uint32_t rate, uint32_t *rows)
{
    parlio_tx_unit_handle_t tx=NULL;
    bool decorated=false, enabled=false;
    const parlio_tx_unit_config_t cfg={
        .clk_src=PARLIO_CLK_SRC_DEFAULT, .clk_in_gpio_num=-1,
        .output_clk_freq_hz=rate, .data_width=8,
        .data_gpio_nums={23,24,11,12,8,9,-1,-1},
        .clk_out_gpio_num=-1, .valid_gpio_num=-1,
        .trans_queue_depth=1, .max_transfer_size=INPUT_BYTES,
        .dma_burst_size=32, .shift_edge=PARLIO_SHIFT_EDGE_NEG,
        .bit_pack_order=PARLIO_BIT_PACK_ORDER_LSB,
    };
    esp_err_t err=parlio_new_tx_unit(&cfg,&tx);
    if (err!=ESP_OK) return err;
    err=parlio_tx_unit_decorate_bitscrambler(tx);
    if (err!=ESP_OK) goto cleanup;
    decorated=true;
    err=parlio_tx_unit_enable(tx);
    if (err!=ESP_OK) goto cleanup;
    enabled=true;
    const parlio_transmit_config_t tr={
        .idle_value=20, .bitscrambler_program=c5vrx2_wbfm_linear80_program(),
    };
    for (unsigned run=0;run<4;++run) {
        uint32_t bytes=(run&1)?INPUT_BYTES:INPUT_BYTES/4;
        uint32_t *r=rows+run*5;
        r[0]=rate; r[1]=bytes;
        PARL_IO.int_clr.tx_fifo_rempty_int_clr=1;
        int64_t start=esp_timer_get_time();
        err=parlio_tx_unit_transmit(tx,raw,bytes*8u,&tr);
        if (err==ESP_OK) {
            /* Observe the sticky FIFO flag during, not after, the stream.
             * No USB/logging or buffer inspection during transmission. */
            esp_rom_delay_us((uint64_t)bytes*1000000u/rate);
            r[3]=PARL_IO.int_raw.val;
            err=parlio_tx_unit_wait_all_done(tx,1000);
        }
        r[2]=(uint32_t)(esp_timer_get_time()-start); r[4]=err;
        if (err!=ESP_OK) break;
    }
cleanup:
    if (enabled) (void)parlio_tx_unit_disable(tx);
    if (decorated) (void)parlio_tx_unit_undecorate_bitscrambler(tx);
    (void)parlio_del_tx_unit(tx);
    return err;
}

esp_err_t c5vrx2_linear80_oracle_run(void)
{
    uint32_t h[64]={0x4f30384cu,1,sizeof(h),INPUT_BYTES,OUTPUT_BYTES};
    h[6]=h[7]=h[8]=UINT32_MAX;
    h[12]=h[13]=UINT32_MAX;
    h[14]=CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ*1000000u;
    for (unsigned i=16;i<56;++i) h[i]=UINT32_MAX;
    const c5vrx2_calibration_t *cal=c5vrx2_calibration_get();
    if (cal->pedestal_code!=20 || cal->discriminator_gain!=2 || cal->polarity!=0)
        return ESP_ERR_INVALID_STATE;
    uint8_t *raw=heap_caps_malloc(INPUT_BYTES,MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    uint8_t *actual=heap_caps_malloc(OUTPUT_CAPACITY,MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    uint8_t *expected=malloc(OUTPUT_BYTES);
    uint8_t *base=malloc(INPUT_BYTES/2);
    if (!raw || !actual || !expected || !base) {
        free(raw); free(actual); free(expected); free(base); return ESP_ERR_NO_MEM;
    }
    for (size_t i=0;i<INPUT_BYTES;++i) raw[i]=(i*73u+(i>>3)*29u+(i>>7)*11u+17u)&255;
    c5vrx2_wbfm_q4_phase5_reference(raw,INPUT_BYTES,base,INPUT_BYTES/2);
    unsigned a=0;
    for (size_t i=0;i<INPUT_BYTES/2;++i) {
        unsigned b=base[i];
        expected[i*4]=a; expected[i*4+1]=(3*a+b)/4;
        expected[i*4+2]=(a+b)/2; expected[i*4+3]=(a+3*b)/4;
        a=b;
    }
    memset(actual,0xa5,OUTPUT_CAPACITY);
    bitscrambler_handle_t bs=NULL;
    esp_err_t err=bitscrambler_loopback_create(&bs,SOC_BITSCRAMBLER_ATTACH_I2S0,OUTPUT_CAPACITY);
    size_t written=0;
    if (err==ESP_OK) err=bitscrambler_load_program(bs,c5vrx2_wbfm_linear80_program());
    if (err==ESP_OK) err=bitscrambler_loopback_run(bs,raw,INPUT_BYTES,actual,OUTPUT_CAPACITY,&written);
    h[5]=written; h[6]=err;
    if (bs) (void)bitscrambler_free(bs);
    if (err==ESP_OK) {
        h[7]=0;
        size_t count=written<OUTPUT_BYTES?written:OUTPUT_BYTES;
        for (size_t i=0;i<count;++i) if (actual[i]!=expected[i]) {
            if (h[8]==UINT32_MAX) h[8]=i;
            h[7]++;
        }
        h[7]+=OUTPUT_BYTES-count;
        /* Timing runs also execute on a mismatch, to preserve diagnostic
         * evidence; a mismatch is never promoted to a passing live gate. */
        h[12]=timed_tx(raw,40000000,h+16);
        h[13]=timed_tx(raw,80000000,h+36);
    }
    h[9]=fnv(raw,INPUT_BYTES); h[10]=fnv(actual,OUTPUT_CAPACITY);
    h[11]=fnv(expected,OUTPUT_BYTES); h[15]=OUTPUT_CAPACITY;
    const esp_partition_t *p=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,0x42,"diagcap");
    size_t total=sizeof(h)+OUTPUT_CAPACITY+OUTPUT_BYTES;
    esp_err_t saved=p?ESP_OK:ESP_ERR_NOT_FOUND;
    size_t erase=p?((total+p->erase_size-1)/p->erase_size)*p->erase_size:0;
    if (p && erase>p->size) saved=ESP_ERR_INVALID_SIZE;
    if (saved==ESP_OK) saved=esp_partition_erase_range(p,0,erase);
    if (saved==ESP_OK) saved=esp_partition_write(p,sizeof(h),actual,OUTPUT_CAPACITY);
    if (saved==ESP_OK) saved=esp_partition_write(p,sizeof(h)+OUTPUT_CAPACITY,expected,OUTPUT_BYTES);
    if (saved==ESP_OK) saved=esp_partition_write(p,0,h,sizeof(h));
    uint8_t check[256];
    for (size_t pos=0;saved==ESP_OK && pos<total;pos+=sizeof(check)) {
        size_t n=total-pos<sizeof(check)?total-pos:sizeof(check);
        saved=esp_partition_read(p,pos,check,n);
        for (size_t j=0;saved==ESP_OK && j<n;++j) {
            size_t k=pos+j;
            uint8_t v=k<sizeof(h)?((uint8_t*)h)[k]:
                k<sizeof(h)+OUTPUT_CAPACITY?actual[k-sizeof(h)]:expected[k-sizeof(h)-OUTPUT_CAPACITY];
            if (check[j]!=v) saved=ESP_FAIL;
        }
    }
    ESP_LOGW("linear80", "loop_err=%lu written=%lu mismatches=%lu first=%lu flash=%s",
             h[6],h[5],h[7],h[8],esp_err_to_name(saved));
    for (unsigned i=0;i<8;++i) {
        const uint32_t *r=h+16+i*5;
        ESP_LOGW("linear80","rate=%lu input=%lu elapsed_us=%lu mid_irq=0x%lx error=%lu",r[0],r[1],r[2],r[3],r[4]);
    }
    free(raw);free(actual);free(expected);free(base);
    bool failed=h[6]!=0 || h[7]!=0 || h[12]!=0 || h[13]!=0;
    for (unsigned i=0;i<8;++i) {
        const uint32_t *r=h+16+i*5;
        if (r[4]!=0 || (r[3]&1u)) failed=true;
    }
    return saved!=ESP_OK?saved:failed?ESP_FAIL:ESP_OK;
}
#endif
