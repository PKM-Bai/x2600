#include "player_audio.h"
#include "player_config.h"
#include "FreeRTOS.h"
#include "task.h"
#include "x2600_hal.h"
#include <cache.h>
#include <interrupt.h>
#include <string.h>

#if defined(CORE_RISCV) && (!defined(__riscv) || defined(__riscv_32e))
#error RISC-V audio requires RV32IMC/ILP32, never RV32E
#endif
#if !defined(CORE_RISCV) && !defined(CORE_CPU)
#error Build audio only for the selected MIPS or RISC-V hardware owner
#endif
#if SYSCLK_APLL != 1152000000UL
#error Audio clock coefficients require the 1152 MHz APLL profile
#endif
_Static_assert(PLAYER_DEFAULT_VOLUME_PERCENT <= 100u, "default volume must be a percent");

static player_pcm_queue pcm_queue;
static DMA_DescriptorDef descriptor __attribute__((aligned(64)));
static DMA_InitDescConfTypeDef descriptor_config;
static DMA_InitChannelConfTypeDef channel_config;
static DMA_HandleTypeDef dma;
static AIC_HandleTypeDef aic;
static ICODEC_HandleTypeDef codec;
static TaskHandle_t owner;
static player_audio_stats stats;
static TickType_t dma_started;
static TickType_t drain_started;
static TickType_t fifo_empty_at;
static unsigned initialized;
static unsigned dma_initialized;
static unsigned channel_allocated;
static unsigned codec_powered;
static unsigned stream_started;
static unsigned pause_requested;
static unsigned primed;
static unsigned fifo_empty_seen;
static unsigned volume_percent = PLAYER_DEFAULT_VOLUME_PERCENT;
static const HAL_AIC_PowerGpioTypeDef amplifier = {
    .Port = PE_Instance, .Pin = 17, .Active = 1
};

static void memory_barrier(void)
{
#if defined(CORE_RISCV)
    __asm__ volatile("fence rw,rw" ::: "memory");
#else
    __asm__ volatile("sync" ::: "memory");
#endif
}

/* SDK ICODEC power sequencing requires millisecond delays; the owner is a
 * running FreeRTOS task. A platform may supply a strong implementation. */
__attribute__((weak)) void HAL_Delay(uint32_t milliseconds)
{
    TickType_t ticks = pdMS_TO_TICKS(milliseconds);
    if (milliseconds && !ticks)
        ticks = 1;
    vTaskDelay(ticks);
}

static unsigned volume_gain(unsigned percent)
{
    return percent ? 140u + percent * 60u / 100u : 0u;
}

static void output_state(unsigned enable)
{
    if (!enable || !volume_percent) {
        __HAL_ICODEC_DAC_MUTE_ENABLE(&codec);
        HAL_AIC_PlaybackDisable(&aic, amplifier);
    } else {
        __HAL_ICODEC_DAC_MUTE_DISABLE(&codec);
        HAL_AIC_PlaybackEnable(&aic, amplifier);
    }
}

/* IRQ and task share this two-write stop sequence. Preserve the CTE falling
 * edge before clearing terminal flags. Descriptor interrupt bits are W0C.
 * No queue/descriptor may be reused if the channel refuses to stop. */
static int dma_quiesce(void)
{
    unsigned channel = channel_config.Channel;
    uint32_t value;
    unsigned attempt;
    if (!channel_allocated)
        return 0;
    value = READ_REG(dma.Instance->Channel.CHANNEL[channel].DCSn);
    value &= ~DCSn_CTE;
    WRITE_REG(dma.Instance->Channel.CHANNEL[channel].DCSn, value);
    (void)READ_REG(dma.Instance->Channel.CHANNEL[channel].DCSn);
    for (attempt = 0; attempt < 1024u; ++attempt) {
        if (!(READ_REG(dma.Instance->Channel.CHANNEL[channel].DCSn) & DCSn_CTE))
            break;
    }
    if (attempt == 1024u)
        return PLAYER_AUDIO_TIMEOUT;
    value &= ~(DCSn_TT | DCSn_HLT | DCSn_AR | DCSn_STOP);
    WRITE_REG(dma.Instance->Channel.CHANNEL[channel].DCSn, value);
    WRITE_REG(dma.Instance->GCR.DIC, ~(1UL << channel));
    memory_barrier();
    return 0;
}

static int fault(int error)
{
    stats.last_error = error;
    stats.state = PLAYER_AUDIO_FAULT;
    __HAL_AIC_DISABLE_TRANSMIT_DMA(&aic);
    __HAL_AIC_DISABLE_REPLAY(&aic);
    output_state(0);
    stream_started = 0;
    /* Active storage remains owned until STOP verifies hardware quiescence. */
    return error;
}

static void count_underrun(void)
{
    if (__HAL_AIC_READ_TUR(&aic)) {
        ++stats.fifo_underruns;
        __HAL_AIC_CLEAR_TUR(&aic);
    }
}

/* Called with task critical section held or in the DMA ISR. Successors are
 * rearmed in the ISR; task notification latency cannot empty the small FIFO.
 */
static int start_period(TickType_t now)
{
    player_pcm_period *period = player_pcm_take(&pcm_queue);
    unsigned channel = channel_config.Channel;
    unsigned prefill = stream_started ? 0u : PLAYER_PCM_DMA_SAMPLES;
    unsigned i;
    if (!period)
        return fault(PLAYER_AUDIO_STATE);
    if (prefill) {
        __HAL_AIC_DISABLE_REPLAY(&aic);
        __HAL_AIC_DISABLE_TRANSMIT_DMA(&aic);
        __HAL_AIC_FLUSH_TXFIFO(&aic);
        __HAL_AIC_CLEAR_TUR(&aic);
        for (i = 0; i < prefill; ++i)
            __HAL_AIC_WRITE_TX_FIFO(&aic, (uint16_t)period->samples[i]);
    }
    descriptor_config.DescSrcAddress = period->samples + prefill;
    descriptor_config.DataLength = (period->dma_samples - prefill) * sizeof(int16_t);
    if (HAL_DMA_Initdescriptor(&dma, (int32_t)channel) != HAL_OK)
        return fault(PLAYER_AUDIO_HARDWARE);
    CleanDCache_by_Addr((unsigned long *)period->samples, sizeof(period->samples));
    CleanDCache_by_Addr((unsigned long *)&descriptor, sizeof(descriptor));
    memory_barrier();
    if (dma_quiesce())
        return fault(PLAYER_AUDIO_TIMEOUT);
    WRITE_REG(dma.Instance->Channel.CHANNEL[channel].DDAn, VIRT_TO_PHYS(&descriptor));
    WRITE_REG(dma.Instance->GCR.DDS, 1UL << channel);
    WRITE_REG(dma.Instance->Channel.CHANNEL[channel].DCSn, DCSn_DES8 | DCSn_CTE);
    memory_barrier();
    dma_started = now;
    stats.state = PLAYER_AUDIO_PLAYING;
    primed = 1;
    if (!stream_started) {
        __HAL_AIC_ENABLE_TRANSMIT_DMA(&aic);
        __HAL_AIC_ENABLE_REPLAY(&aic);
        __HAL_AIC_ENABLE(&aic);
        stream_started = 1;
        output_state(1);
    }
    return 0;
}

static void begin_drain(TickType_t now)
{
    __HAL_AIC_DISABLE_TRANSMIT_DMA(&aic);
    drain_started = now;
    fifo_empty_seen = 0;
    stats.state = pause_requested ? PLAYER_AUDIO_PAUSING : PLAYER_AUDIO_DRAINING;
    /* Leave replay enabled until the FIFO's remaining samples have left. */
}

static void dma_irq(int irq, void *data)
{
    BaseType_t wake = pdFALSE;
    unsigned channel = channel_config.Channel;
    uint32_t pending = READ_REG(dma.Instance->GCR.DIRQP) |
                       READ_REG(dma.Instance->GCR.DIP);
    uint32_t status;
    TickType_t now;
    (void)irq;
    (void)data;
    if (!channel_allocated || !(pending & (1UL << channel)))
        return;
    status = READ_REG(dma.Instance->Channel.CHANNEL[channel].DCSn);
    if (dma_quiesce()) {
        ++stats.dma_timeouts;
        (void)fault(PLAYER_AUDIO_TIMEOUT);
    } else if (pcm_queue.active_slot >= 0) {
        now = xTaskGetTickCountFromISR();
        if ((status & (DCSn_AR | DCSn_HLT)) || !(status & DCSn_TT)) {
            ++stats.dma_errors;
            (void)fault(PLAYER_AUDIO_HARDWARE);
        } else {
            (void)player_pcm_complete(&pcm_queue);
            ++stats.completed_periods;
            count_underrun();
            if (!pause_requested && pcm_queue.ready_count) {
                (void)start_period(now);
            } else {
                if (!pause_requested && !pcm_queue.eof) {
                    ++stats.queue_underruns;
                    primed = 0;
                }
                begin_drain(now);
            }
        }
    }
    if (owner)
        vTaskNotifyGiveFromISR(owner, &wake);
    portYIELD_FROM_ISR(wake);
}

int player_audio_open(void)
{
    LL_CPM_CGU_I2STypeDef clock = {
        .PLLMux = I2STCDR_SCLK_A,
        .Div_M = 4UL << I2STCDR_I2SDIV_M_Pos,
        .Div_N = 375UL << I2STCDR_I2SDIV_N_Pos,
        .Div_D = 0, .N_EN = 0, .D_EN = 0
    };
    if (initialized)
        return stats.state == PLAYER_AUDIO_FAULT ? stats.last_error : 0;
    if (stats.state != PLAYER_AUDIO_CLOSED ||
        xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED)
        return PLAYER_AUDIO_STATE;
    owner = xTaskGetCurrentTaskHandle();
    aic.Instance = AIC_Instance;
    codec.Instance = ICODEC_Instance;
    LL_GPIO_setPinMode(PE_Instance, 17, GPIO_MODE_OUTPUT0);
    CPM_GATE_Enable(CPM_Instance, CPM_CLKID_AUDIO);
    CPM_GATE_Enable(CPM_Instance, CPM_CLKID_I2ST);
    CPM_GATE_Enable(CPM_Instance, CPM_CLKID_DMAC);
    if (CPM_CGU_I2S0_Config(CPM_Instance, &clock) != HAL_OK)
        return fault(PLAYER_AUDIO_HARDWARE);
    if ((READ_REG(CPM_Instance->I2STCDR) &
         (I2STCDR_I2SDIV_M | I2STCDR_I2SDIV_N | I2STCDR_I2SCS)) !=
        (clock.Div_M | clock.Div_N | clock.PLLMux) ||
        (READ_REG(CPM_Instance->I2STCDR1) & (I2STCDR1_I2S_NEN | I2STCDR1_I2S_DEN)))
        return fault(PLAYER_AUDIO_HARDWARE);
    dma.Instance = DMA_Instance;
    if (HAL_DMA_Init(&dma) != HAL_OK)
        return fault(PLAYER_AUDIO_HARDWARE);
    dma_initialized = 1;
    channel_config.DescMode = DESCRIPTOR;
    channel_config.desc = &descriptor;
    channel_config.desc_count = 1;
    channel_config.descConfig = &descriptor_config;
    if (HAL_DMA_requestChannel(&dma, &channel_config) < 0)
        return fault(PLAYER_AUDIO_HARDWARE);
    channel_allocated = 1;
    descriptor_config.Desc_Interrupt = DMA_INTERRUPT;
    descriptor_config.Desc_Link = NO_LINK;
    descriptor_config.DescDMATransferType = DMA_RQ_AIC_T;
    descriptor_config.DescSrcAddrIncrement = ENABLE;
    descriptor_config.DescSrcPortWidth = DMA_PORT_16BIT;
    descriptor_config.DescDstAddrIncrement = DISABLE;
    descriptor_config.DescDstPortWidth = DMA_PORT_16BIT;
    descriptor_config.DescDstAddress = (void *)&AIC_Instance->AICDR;
    descriptor_config.DescTransferNumByte = 32;
    aic.Stream = HAL_AIC_STREAM_ONLY_PLAYBACK;
    aic.Playback.SampleRates = HAL_AIC_SAMPLE_RATE_48000;
    aic.Playback.Format = HAL_AIC_DATA_FMT_S16LE;
    aic.Playback.Channels = 1;
    aic.Interface = HAL_AIC_OPERATION_MODE_I2S;
    aic.LrFirst = HAL_AIC_FRAME_MODE_LR;
    aic.BclkDirection = HAL_AIC_BCLK_CODEC_SLAVE;
    aic.codec = HAL_AIC_CODEC_INTERNAL;
    if (HAL_AIC_Init(&aic) != HAL_OK)
        return fault(PLAYER_AUDIO_HARDWARE);
    __HAL_AIC_DISABLE_REPLAY(&aic);
    __HAL_AIC_DISABLE_TRANSMIT_DMA(&aic);
    __HAL_AIC_PLAY_ZERO(&aic);
    /* MDIV=0 bypasses MCLK division. The SDK writes odd 1 which reads as 0;
     * DEN=0 calculates Div_D=187 automatically, so don't compare Div_D. */
    if (READ_REG(aic.Instance->I2SDIV) & I2SDIV_MDIV_Msk)
        return fault(PLAYER_AUDIO_HARDWARE);
    codec.Interface = HAL_ICODEC_OPERATION_MODE_I2S;
    codec.Stream = HAL_ICODEC_STREAM_ONLY_PLAYBACK;
    codec.Playback.SampleRates = HAL_ICODEC_SAMPLE_RATE_48000;
    codec.Playback.Format = HAL_ICODEC_DATA_FMT_S16LE;
    codec.Playback.dgain = volume_gain(volume_percent);
    if (HAL_ICODEC_PowerOn(&codec) != HAL_OK)
        return fault(PLAYER_AUDIO_HARDWARE);
    codec_powered = 1;
    if (HAL_ICODEC_Configure(&codec) != HAL_OK)
        return fault(PLAYER_AUDIO_HARDWARE);
    output_state(0);
    ll_request_irq(IRQ_INTC0_PDMA, dma_irq, NULL);
    ll_request_irq(IRQ_INTC0_PDMAD, dma_irq, NULL);
    player_pcm_reset(&pcm_queue);
    initialized = 1;
    stats.state = PLAYER_AUDIO_IDLE;
    return 0;
}

int player_audio_begin(void)
{
    if (!initialized || (stats.state != PLAYER_AUDIO_IDLE &&
                         stats.state != PLAYER_AUDIO_DRAINED))
        return PLAYER_AUDIO_STATE;
    taskENTER_CRITICAL();
    if (dma_quiesce()) {
        taskEXIT_CRITICAL();
        return fault(PLAYER_AUDIO_TIMEOUT);
    }
    output_state(0);
    __HAL_AIC_DISABLE_REPLAY(&aic);
    __HAL_AIC_DISABLE_TRANSMIT_DMA(&aic);
    __HAL_AIC_FLUSH_TXFIFO(&aic);
    player_pcm_reset(&pcm_queue);
    memset(&stats, 0, sizeof(stats));
    stats.state = PLAYER_AUDIO_BUFFERING;
    stream_started = 0;
    pause_requested = 0;
    primed = 0;
    taskEXIT_CRITICAL();
    if (HAL_ICODEC_DAC_Enable(&codec) != HAL_OK)
        return fault(PLAYER_AUDIO_HARDWARE);
    HAL_Delay(300);
    return 0;
}

int player_audio_service(void)
{
    TickType_t now = xTaskGetTickCount();
    int result = 0;
    taskENTER_CRITICAL();
    if (stats.state == PLAYER_AUDIO_FAULT) {
        result = stats.last_error;
    } else if (pcm_queue.active_slot >= 0 &&
               now - dma_started > pdMS_TO_TICKS(50)) {
        ++stats.dma_timeouts;
        (void)dma_quiesce();
        result = fault(PLAYER_AUDIO_TIMEOUT);
    } else if (stats.state == PLAYER_AUDIO_PAUSING ||
               stats.state == PLAYER_AUDIO_DRAINING) {
        if (!__HAL_AIC_READ_TFL(&aic)) {
            if (!fifo_empty_seen) {
                fifo_empty_seen = 1;
                fifo_empty_at = now;
            }
            /* FIFO empty can still leave the final sample in the serializer.
             * One complete RTOS tick is longer than a 48 kHz frame. */
            if (now - fifo_empty_at < pdMS_TO_TICKS(1)) {
                taskEXIT_CRITICAL();
                return 0;
            }
            __HAL_AIC_DISABLE_REPLAY(&aic);
            output_state(0);
            stream_started = 0;
            if (pause_requested)
                stats.state = PLAYER_AUDIO_PAUSED;
            else if (player_pcm_drained(&pcm_queue))
                stats.state = PLAYER_AUDIO_DRAINED;
            else
                stats.state = PLAYER_AUDIO_BUFFERING;
        } else if (now - drain_started > pdMS_TO_TICKS(50)) {
            ++stats.dma_timeouts;
            result = fault(PLAYER_AUDIO_TIMEOUT);
        } else {
            fifo_empty_seen = 0;
        }
    }
    if (!result && stats.state == PLAYER_AUDIO_BUFFERING) {
        if (pause_requested) {
            stats.state = PLAYER_AUDIO_PAUSED;
        } else if (player_pcm_drained(&pcm_queue)) {
            stats.state = PLAYER_AUDIO_DRAINED;
        } else if (pcm_queue.ready_count && (primed || pcm_queue.eof ||
                   pcm_queue.ready_count >= PLAYER_PCM_PRIME_PERIODS)) {
            result = start_period(now);
        }
    }
    taskEXIT_CRITICAL();
    return result;
}

unsigned player_audio_write(const int16_t *pcm, unsigned samples)
{
    unsigned accepted = 0;
    if (player_audio_service())
        return 0;
    taskENTER_CRITICAL();
    if (stats.state == PLAYER_AUDIO_BUFFERING || stats.state == PLAYER_AUDIO_PLAYING ||
        stats.state == PLAYER_AUDIO_PAUSING || stats.state == PLAYER_AUDIO_PAUSED ||
        stats.state == PLAYER_AUDIO_DRAINING)
        accepted = player_pcm_write(&pcm_queue, pcm, samples);
    taskEXIT_CRITICAL();
    (void)player_audio_service();
    return accepted;
}

int player_audio_finish(void)
{
    taskENTER_CRITICAL();
    if (!initialized || stats.state == PLAYER_AUDIO_IDLE || stats.state == PLAYER_AUDIO_FAULT ||
        stats.state == PLAYER_AUDIO_CLOSED) {
        taskEXIT_CRITICAL();
        return PLAYER_AUDIO_STATE;
    }
    player_pcm_finish(&pcm_queue);
    taskEXIT_CRITICAL();
    return player_audio_service();
}

int player_audio_pause(void)
{
    taskENTER_CRITICAL();
    if (!initialized || stats.state == PLAYER_AUDIO_IDLE || stats.state == PLAYER_AUDIO_DRAINED ||
        stats.state == PLAYER_AUDIO_FAULT) {
        taskEXIT_CRITICAL();
        return PLAYER_AUDIO_STATE;
    }
    pause_requested = 1;
    /* PLAYING remains until the ISR completes its active period. */
    taskEXIT_CRITICAL();
    return player_audio_service();
}

int player_audio_resume(void)
{
    taskENTER_CRITICAL();
    if (stats.state != PLAYER_AUDIO_PAUSED) {
        taskEXIT_CRITICAL();
        return PLAYER_AUDIO_STATE;
    }
    pause_requested = 0;
    stats.state = PLAYER_AUDIO_BUFFERING;
    /* No new 50 ms prefill is needed for already queued paused data. */
    primed = 1;
    taskEXIT_CRITICAL();
    return player_audio_service();
}

int player_audio_stop(void)
{
    int result;
    if (!initialized && stats.state == PLAYER_AUDIO_CLOSED)
        return 0;
    taskENTER_CRITICAL();
    output_state(0);
    __HAL_AIC_DISABLE_TRANSMIT_DMA(&aic);
    __HAL_AIC_DISABLE_REPLAY(&aic);
    result = dma_quiesce();
    if (result) {
        ++stats.dma_timeouts;
        result = fault(result);
    } else {
        __HAL_AIC_FLUSH_TXFIFO(&aic);
        /* Preserve counters for the STOP ACK but release all private slots. */
        unsigned submitted = pcm_queue.submitted_samples;
        unsigned completed = pcm_queue.completed_samples;
        player_pcm_reset(&pcm_queue);
        pcm_queue.submitted_samples = submitted;
        pcm_queue.completed_samples = completed;
        stream_started = 0;
        pause_requested = 0;
        primed = 0;
        stats.state = PLAYER_AUDIO_IDLE;
        stats.last_error = 0;
        if (codec_powered)
            (void)HAL_ICODEC_DAC_Disable(&codec);
    }
    taskEXIT_CRITICAL();
    return result;
}

int player_audio_close(void)
{
    int result;
    if (stats.state == PLAYER_AUDIO_CLOSED)
        return 0;
    result = player_audio_stop();
    if (result)
        return result;
    ll_disable_irq(IRQ_INTC0_PDMA);
    ll_disable_irq(IRQ_INTC0_PDMAD);
    if (codec_powered && HAL_ICODEC_PowerOff(&codec) != HAL_OK)
        return fault(PLAYER_AUDIO_HARDWARE);
    if (dma_initialized && HAL_DMA_DeInit(&dma) != HAL_OK)
        return fault(PLAYER_AUDIO_HARDWARE);
    __HAL_AIC_DISABLE(&aic);
    CPM_GATE_Disable(CPM_Instance, CPM_CLKID_DMAC);
    CPM_GATE_Disable(CPM_Instance, CPM_CLKID_I2ST);
    CPM_GATE_Disable(CPM_Instance, CPM_CLKID_AUDIO);
    memset(&dma, 0, sizeof(dma));
    memset(&channel_config, 0, sizeof(channel_config));
    initialized = dma_initialized = channel_allocated = codec_powered = 0;
    owner = NULL;
    stats.state = PLAYER_AUDIO_CLOSED;
    return 0;
}

int player_audio_set_volume(unsigned percent)
{
    if (percent > 100u)
        return PLAYER_AUDIO_ARGUMENT;
    taskENTER_CRITICAL();
    volume_percent = percent;
    if (initialized) {
        codec.Playback.dgain = volume_gain(percent);
        if (HAL_ICODEC_DAC_SetDigGain(&codec, codec.Playback.dgain) != HAL_OK) {
            taskEXIT_CRITICAL();
            return fault(PLAYER_AUDIO_HARDWARE);
        }
        output_state(stream_started);
    }
    taskEXIT_CRITICAL();
    return 0;
}

void player_audio_get_stats(player_audio_stats *output)
{
    if (!output)
        return;
    taskENTER_CRITICAL();
    *output = stats;
    output->submitted_samples = pcm_queue.submitted_samples;
    output->completed_samples = pcm_queue.completed_samples;
    output->ready_periods = pcm_queue.ready_count;
    output->active_period = initialized ? pcm_queue.active_slot : -1;
    output->volume_percent = volume_percent;
    taskEXIT_CRITICAL();
}
