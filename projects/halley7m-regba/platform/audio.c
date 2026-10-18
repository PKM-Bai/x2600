#include "audio.h"

#include "FreeRTOS.h"
#include "task.h"
#include "platform_runtime.h"
#include "x2600_hal.h"
#include <cache.h>
#include <interrupt.h>
#include <errno.h>
#include <string.h>

#if SYSCLK_APLL != 1152000000UL
#error Audio clock coefficients require the HALLEY7-M 1152 MHz APLL profile
#endif

/* The v1 storage driver polls SSI0, so audio exclusively owns main PDMA.
 * Each DMA transfer terminates after one 10 ms period. The service task must
 * restart it before the AIC FIFO empties. FIFO underruns expose a missed
 * deadline; cyclic DMA is deliberately not used to replay stale PCM silently.
 */
enum period_state { PERIOD_FREE, PERIOD_FILLING, PERIOD_READY, PERIOD_PLAYING };

static int16_t playback[AUDIO_PERIOD_COUNT][AUDIO_PERIOD_FRAMES]
    __attribute__((aligned(64)));
_Static_assert(sizeof(playback) == 5760u, "audio ring must stay within 5760 bytes");
_Static_assert(AUDIO_PRIME_PERIODS <= AUDIO_PERIOD_COUNT, "prefill must fit the audio ring");
_Static_assert(AUDIO_FIFO_PREFILL_FRAMES * sizeof(int16_t) == 32u,
               "FIFO prefill must preserve first DMA burst alignment");
_Static_assert(AUDIO_FIFO_PREFILL_FRAMES < AUDIO_PERIOD_FRAMES,
               "FIFO prefill must fit the first period");
static DMA_DescriptorDef descriptor __attribute__((aligned(64)));
static DMA_InitDescConfTypeDef descriptor_config;
static DMA_InitChannelConfTypeDef channel_config;
static DMA_HandleTypeDef dma;
static AIC_HandleTypeDef aic;
static ICODEC_HandleTypeDef codec;
static TaskHandle_t service_task;
static volatile enum period_state periods[AUDIO_PERIOD_COUNT];
static volatile int active_period = -1;
static volatile unsigned ready_periods;
static volatile unsigned read_period;
static unsigned write_period;
static unsigned fill_frames;
static volatile int running;
static volatile int output_muted;
static unsigned volume_percent = 100u;
static int playback_enabled;
static int initialized;
static int primed;
static int stream_started;
static uint64_t dma_started_us;
static uint64_t dma_finished_us;
static uint64_t last_ready_us;
static uint32_t stop_generation;
static uint32_t source_rate;
static uint32_t source_phase;
static int16_t source_previous;
static int source_previous_valid;
static volatile int tone_cancel_requested;
static audio_stats_t counters;

static const HAL_AIC_PowerGpioTypeDef amplifier = {
    .Port = PE_Instance, .Pin = 17, .Active = 1
};

static unsigned volume_gain(unsigned percent)
{
    /* DAC code 0 is mute; each six codes is 3dB. Keep 100% at the proven
     * code 200 (-13dB), with no gain above the existing playback level. */
    return percent ? 140u + percent * 60u / 100u : 0u;
}

/* Call with the scheduler/interrupt lock held. Mute and amplifier gating are
 * independent of the PCM ring, so silent validation still exercises DMA.
 */
static void apply_output_state(void)
{
    if (output_muted || !volume_percent || !playback_enabled) {
        __HAL_ICODEC_DAC_MUTE_ENABLE(&codec);
        HAL_AIC_PlaybackDisable(&aic, amplifier);
    } else {
        __HAL_ICODEC_DAC_MUTE_DISABLE(&codec);
        HAL_AIC_PlaybackEnable(&aic, amplifier);
    }
}

/* Keep bring-up failures actionable without printing on the DMA hot path. */
static int audio_failure(const char *stage, int result)
{
    prom_printk("AUDIO_FAIL stage=%s result=%d I2STCDR=%08lx I2STCDR1=%08lx I2SDIV=%08lx AICFR=%08lx AICCR=%08lx AICSR=%08lx\n",
                stage, result,
                (unsigned long)READ_REG(CPM_Instance->I2STCDR),
                (unsigned long)READ_REG(CPM_Instance->I2STCDR1),
                (unsigned long)READ_REG(AIC_Instance->I2SDIV),
                (unsigned long)READ_REG(AIC_Instance->AICFR),
                (unsigned long)READ_REG(AIC_Instance->AICCR),
                (unsigned long)READ_REG(AIC_Instance->AICSR));
    if (dma.Instance && channel_config.desc_count) {
        unsigned channel = channel_config.Channel;
        prom_printk("AUDIO_DMA channel=%u DCS=%08lx DTC=%08lx DRT=%08lx DIRQP=%08lx DIP=%08lx completed=%lu errors=%lu timeouts=%lu\n",
                    channel,
                    (unsigned long)READ_REG(dma.Instance->Channel.CHANNEL[channel].DCSn),
                    (unsigned long)READ_REG(dma.Instance->Channel.CHANNEL[channel].DTCn),
                    (unsigned long)READ_REG(dma.Instance->Channel.CHANNEL[channel].DRTn),
                    (unsigned long)READ_REG(dma.Instance->GCR.DIRQP),
                    (unsigned long)READ_REG(dma.Instance->GCR.DIP),
                    (unsigned long)counters.completed_periods,
                    (unsigned long)counters.dma_errors,
                    (unsigned long)counters.dma_timeouts);
    }
    return -1;
}

static void dma_clear_stop(void)
{
    unsigned channel = channel_config.Channel;
    unsigned long value = READ_REG(dma.Instance->Channel.CHANNEL[channel].DCSn);

    /* Separate CTE from terminal/error flag clearing: preserves doorbell edges
     * on this PDMA. Do not use HAL_DMA_Stop's single DCS=0 write here.
     */
    value &= ~DCSn_CTE;
    WRITE_REG(dma.Instance->Channel.CHANNEL[channel].DCSn, value);
    (void)READ_REG(dma.Instance->Channel.CHANNEL[channel].DCSn);
    value &= ~(DCSn_TT | DCSn_HLT | DCSn_AR | DCSn_STOP);
    WRITE_REG(dma.Instance->Channel.CHANNEL[channel].DCSn, value);
    /* Descriptor interrupt clear bits are write-zero-to-clear. */
    WRITE_REG(dma.Instance->GCR.DIC, ~(1UL << channel));
    __asm__ volatile("sync" ::: "memory");
}

static void count_fifo_underrun(void)
{
    if (__HAL_AIC_READ_TUR(&aic)) {
        if (!counters.fifo_underruns) {
            counters.first_fifo_underrun_us = platform_time_us();
            counters.first_fifo_completed_periods = counters.completed_periods;
        }
        ++counters.fifo_underruns;
        __HAL_AIC_CLEAR_TUR(&aic);
    }
}

static void dma_irq(int irq, void *data)
{
    BaseType_t wake = pdFALSE;
    unsigned channel = channel_config.Channel;
    uint32_t pending = READ_REG(dma.Instance->GCR.DIRQP) |
                       READ_REG(dma.Instance->GCR.DIP);
    unsigned long status;
    int finished;
    (void)irq;
    (void)data;

    if (!(pending & (1UL << channel)))
        return;
    status = READ_REG(dma.Instance->Channel.CHANNEL[channel].DCSn);
    dma_clear_stop();
    finished = active_period;
    if (finished < 0)
        return;
    active_period = -1;
    periods[finished] = PERIOD_FREE;
    if (status & (DCSn_AR | DCSn_HLT)) {
        ++counters.dma_errors;
        running = 0;
        __HAL_AIC_DISABLE_TRANSMIT_DMA(&aic);
        __HAL_AIC_DISABLE_REPLAY(&aic);
    } else {
        ++counters.completed_periods;
        /* Only measure a rearm deadline when a successor was already ready.
         * Waiting for a producer after starvation is a different interval.
         */
        dma_finished_us = ready_periods ? platform_time_us() : 0;
        count_fifo_underrun();
    }
    vTaskNotifyGiveFromISR(service_task, &wake);
    portYIELD_FROM_ISR(wake);
}

static void start_period(unsigned period)
{
    unsigned channel = channel_config.Channel;
    unsigned prefill = stream_started ? 0u : AUDIO_FIFO_PREFILL_FRAMES;

    if (prefill) {
        /* PM 16.9.4: ERPL must not open an empty transmit FIFO. ENB is
         * already set by HAL_AIC_Init, so DMA may lose that first-sample race.
         * Preload real PCM and skip those samples in the first descriptor:
         * one complete 480-sample period, with no inserted zero or duplication.
         */
        __HAL_AIC_DISABLE_REPLAY(&aic);
        __HAL_AIC_DISABLE_TRANSMIT_DMA(&aic);
        __HAL_AIC_FLUSH_TXFIFO(&aic);
        __HAL_AIC_CLEAR_TUR(&aic);
        for (unsigned i = 0; i < prefill; ++i)
            __HAL_AIC_WRITE_TX_FIFO(&aic, (uint16_t)playback[period][i]);
    }
    descriptor_config.DescSrcAddress = playback[period] + prefill;
    descriptor_config.DataLength = sizeof(playback[period]) - prefill * sizeof(int16_t);
    HAL_DMA_Initdescriptor(&dma, channel);
    CleanDCache_by_Addr((unsigned long *)&descriptor, sizeof(descriptor));
    dma_clear_stop();
    WRITE_REG(dma.Instance->Channel.CHANNEL[channel].DDAn,
              VIRT_TO_PHYS(&descriptor));
    WRITE_REG(dma.Instance->GCR.DDS, 1UL << channel);
    WRITE_REG(dma.Instance->Channel.CHANNEL[channel].DCSn,
              DCSn_DES8 | DCSn_CTE);
    dma_started_us = platform_time_us();
    if (dma_finished_us) {
        uint64_t elapsed = dma_started_us - dma_finished_us;
        if (elapsed > counters.max_dma_restart_us)
            counters.max_dma_restart_us = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
        dma_finished_us = 0;
    }
    if (!stream_started) {
        __HAL_AIC_ENABLE_TRANSMIT_DMA(&aic);
        __HAL_AIC_ENABLE_REPLAY(&aic);
        __HAL_AIC_ENABLE(&aic);
        stream_started = 1;
        playback_enabled = 1;
        apply_output_state();
    }
}

static void audio_service(void *argument)
{
    (void)argument;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
        taskENTER_CRITICAL();
        if (running && active_period >= 0 &&
            platform_time_us() - dma_started_us > 50000u) {
            ++counters.dma_timeouts;
            dma_clear_stop();
            periods[active_period] = PERIOD_FREE;
            active_period = -1;
            running = 0;
            __HAL_AIC_DISABLE_TRANSMIT_DMA(&aic);
            __HAL_AIC_DISABLE_REPLAY(&aic);
        }
        if (running && active_period < 0) {
            if (primed && !ready_periods) {
                uint64_t now = platform_time_us();
                uint64_t gap = now - last_ready_us;
                if (!counters.queue_underruns) {
                    counters.first_queue_underrun_us = now;
                    counters.first_under_ready_periods = counters.last_ready_periods;
                    counters.first_under_active = counters.last_ready_active;
                    counters.first_under_dma_age_us = counters.last_ready_dma_age_us;
                }
                counters.underrun_ready_gap_us = gap > UINT32_MAX ? UINT32_MAX : (uint32_t)gap;
                ++counters.queue_underruns;
                count_fifo_underrun();
                /* Stop fetching instead of repeating the previous block.
                 * PLAY_ZERO covers FIFO exhaustion until input re-primes.
                 */
                primed = 0;
            }
            /* Keep 50ms of complete periods before starting/recovering.
             * The extra 10ms addresses the observed exhaustion after a 33ms
             * supply gap within the same six-period allocation. This does not
             * identify the producer stall or prove a JIT root-cause fix.
             */
            if ((primed || ready_periods >= AUDIO_PRIME_PERIODS) && ready_periods) {
                unsigned next = read_period;
                configASSERT(periods[next] == PERIOD_READY);
                periods[next] = PERIOD_PLAYING;
                --ready_periods;
                read_period = (next + 1u) % AUDIO_PERIOD_COUNT;
                active_period = (int)next;
                primed = 1;
                count_fifo_underrun();
                start_period(next);
            }
        }
        taskEXIT_CRITICAL();
    }
}

int audio_init(void)
{
    /* 1.152 GHz * 4 / 375 = 12.288 MHz. N_EN/D_EN=0 selects automatic
     * divider parameter calculation, matching the SDK internal-codec example.
     */
    LL_CPM_CGU_I2STypeDef clock = {
        .PLLMux = I2STCDR_SCLK_A,
        .Div_M = 4UL << I2STCDR_I2SDIV_M_Pos,
        .Div_N = 375UL << I2STCDR_I2SDIV_N_Pos,
        .Div_D = 0, .N_EN = 0, .D_EN = 0
    };
    int channel;
    HAL_StatusTypeDef result;

    if (initialized)
        return 0;
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED)
        return audio_failure("scheduler", -1);
    LL_GPIO_setPinMode(PE_Instance, 17, GPIO_MODE_OUTPUT0);
    CPM_GATE_Enable(CPM_Instance, CPM_CLKID_AUDIO);
    CPM_GATE_Enable(CPM_Instance, CPM_CLKID_I2ST);
    CPM_GATE_Enable(CPM_Instance, CPM_CLKID_DMAC);
    result = CPM_CGU_I2S0_Config(CPM_Instance, &clock);
    if (result != HAL_OK)
        return audio_failure("i2s-clock-config", result);
    if ((READ_REG(CPM_Instance->I2STCDR) &
         (I2STCDR_I2SDIV_M | I2STCDR_I2SDIV_N | I2STCDR_I2SCS)) !=
        (clock.Div_M | clock.Div_N | clock.PLLMux))
        return audio_failure("i2s-clock-readback", -1);
    /* PM 20.1.2.7: with DEN=0, D is calculated automatically and its field
     * is don't-care. The hardware reads floor(N/2)=187 (0xbb) for N=375,
     * even though the SDK writes Div_D=0; checking that field rejects a
     * correctly configured clock. Verify the selection bits only.
     */
    if (READ_REG(CPM_Instance->I2STCDR1) &
        (I2STCDR1_I2S_NEN | I2STCDR1_I2S_DEN))
        return audio_failure("i2s-clock-auto", -1);

    dma.Instance = DMA_Instance;
    result = HAL_DMA_Init(&dma);
    if (result != HAL_OK)
        return audio_failure("dma-init", result);
    channel_config.DescMode = DESCRIPTOR;
    channel_config.desc = &descriptor;
    channel_config.desc_count = 1;
    channel_config.descConfig = &descriptor_config;
    channel = HAL_DMA_requestChannel(&dma, &channel_config);
    if (channel < 0)
        return audio_failure("dma-channel", channel);
    descriptor_config.Desc_Interrupt = DMA_INTERRUPT;
    descriptor_config.Desc_Link = NO_LINK;
    descriptor_config.DescDMATransferType = DMA_RQ_AIC_T;
    descriptor_config.DescSrcAddrIncrement = ENABLE;
    descriptor_config.DescSrcPortWidth = DMA_PORT_16BIT;
    descriptor_config.DescDstAddrIncrement = DISABLE;
    descriptor_config.DescDstPortWidth = DMA_PORT_16BIT;
    descriptor_config.DescDstAddress = (void *)&AIC_Instance->AICDR;
    descriptor_config.DescTransferNumByte = 32;
    descriptor_config.DataLength = sizeof(playback[0]);

    aic.Instance = AIC_Instance;
    aic.Stream = HAL_AIC_STREAM_ONLY_PLAYBACK;
    aic.Playback.SampleRates = HAL_AIC_SAMPLE_RATE_48000;
    aic.Playback.Format = HAL_AIC_DATA_FMT_S16LE;
    aic.Playback.Channels = 1;
    aic.Interface = HAL_AIC_OPERATION_MODE_I2S;
    aic.LrFirst = HAL_AIC_FRAME_MODE_LR;
    /* Internal codec Configure selects codec master; AIC receives its BCLK. */
    aic.BclkDirection = HAL_AIC_BCLK_CODEC_SLAVE;
    aic.codec = HAL_AIC_CODEC_INTERNAL;
    result = HAL_AIC_Init(&aic);
    if (result != HAL_OK)
        return audio_failure("aic-init", result);
    __HAL_AIC_DISABLE_REPLAY(&aic);
    __HAL_AIC_DISABLE_TRANSMIT_DMA(&aic);
    __HAL_AIC_PLAY_ZERO(&aic);
    /* PM 16.8.2.6: MDIV accepts even divisors; odd values round down.
     * HAL calculates 1 for 12.288 MHz / (48 kHz * 64 * 4), which reads
     * back as 0. MDIV=0 bypasses the divider and supplies the intended
     * 12.288 MHz MCLK. TDIV is unused while the internal codec is master.
     */
    if (READ_REG(aic.Instance->I2SDIV) & I2SDIV_MDIV_Msk)
        return audio_failure("aic-divider", -1);

    codec.Instance = ICODEC_Instance;
    codec.Interface = HAL_ICODEC_OPERATION_MODE_I2S;
    codec.Stream = HAL_ICODEC_STREAM_ONLY_PLAYBACK;
    codec.Playback.SampleRates = HAL_ICODEC_SAMPLE_RATE_48000;
    codec.Playback.Format = HAL_ICODEC_DATA_FMT_S16LE;
    codec.Playback.dgain = volume_gain(audio_get_volume());
    result = HAL_ICODEC_PowerOn(&codec);
    if (result != HAL_OK)
        return audio_failure("codec-power", result);
    result = HAL_ICODEC_Configure(&codec);
    if (result != HAL_OK)
        return audio_failure("codec-config", result);
    __HAL_ICODEC_DAC_MUTE_ENABLE(&codec);
    if (xTaskCreate(audio_service, "audio", 1024, NULL,
                    configMAX_PRIORITIES - 2u, &service_task) != pdPASS) {
        HAL_ICODEC_PowerOff(&codec);
        return audio_failure("service-task", -1);
    }
    /* Replace the SDK handlers: they use HAL_DMA_Stop's DCS=0 sequence.
     * No other main-PDMA client is active in this first-version firmware.
     */
    ll_request_irq(IRQ_INTC0_PDMA, dma_irq, NULL);
    ll_request_irq(IRQ_INTC0_PDMAD, dma_irq, NULL);
    /* A task may change volume while the hardware is being configured.
     * Commit its latest request before publishing initialized. */
    taskENTER_CRITICAL();
    codec.Playback.dgain = volume_gain(volume_percent);
    (void)HAL_ICODEC_DAC_SetDigGain(&codec, codec.Playback.dgain);
    initialized = 1;
    apply_output_state();
    taskEXIT_CRITICAL();
    return 0;
}

int audio_start(void)
{
    HAL_StatusTypeDef result;
    uint32_t generation;
    taskENTER_CRITICAL();
    generation = stop_generation;
    taskEXIT_CRITICAL();
    if (audio_init() != 0)
        return -1;
    taskENTER_CRITICAL();
    if (generation != stop_generation) {
        taskEXIT_CRITICAL();
        return -ECANCELED;
    }
    /* Nested critical sections keep a console stop from interleaving the
     * stop/reset/DAC-enable sequence. The HAL DAC calls contain no waits.
     */
    audio_stop();
    generation = stop_generation;
    memset(playback, 0, sizeof(playback));
    memset(&counters, 0, sizeof(counters));
    write_period = 0;
    fill_frames = 0;
    source_rate = 0;
    source_phase = 0;
    source_previous_valid = 0;
    primed = 0;
    stream_started = 0;
    dma_finished_us = last_ready_us = 0;
    result = HAL_ICODEC_DAC_Enable(&codec);
    if (result != HAL_OK) {
        taskEXIT_CRITICAL();
        return audio_failure("dac-enable", result);
    }
    __HAL_AIC_FLUSH_TXFIFO(&aic);
    __HAL_AIC_CLEAR_TUR(&aic);
    /* Keep the DAC muted and the amplifier disabled through settling and
     * prefill. start_period opens output once actual PCM is being fetched.
     */
    apply_output_state();
    taskEXIT_CRITICAL();
    vTaskDelay(pdMS_TO_TICKS(300));
    taskENTER_CRITICAL();
    if (generation != stop_generation) {
        taskEXIT_CRITICAL();
        return -ECANCELED;
    }
    running = 1;
    apply_output_state();
    taskEXIT_CRITICAL();
    return 0;
}

void audio_stop(void)
{
    taskENTER_CRITICAL();
    ++stop_generation;
    if (!initialized) {
        taskEXIT_CRITICAL();
        return;
    }
    running = 0;
    playback_enabled = 0;
    apply_output_state();
    __HAL_AIC_DISABLE_TRANSMIT_DMA(&aic);
    __HAL_AIC_DISABLE_REPLAY(&aic);
    dma_clear_stop();
    active_period = -1;
    ready_periods = 0;
    read_period = 0;
    write_period = 0;
    fill_frames = 0;
    primed = 0;
    stream_started = 0;
    dma_finished_us = 0;
    for (unsigned i = 0; i < AUDIO_PERIOD_COUNT; ++i)
        periods[i] = PERIOD_FREE;
    HAL_ICODEC_DAC_Disable(&codec);
    taskEXIT_CRITICAL();
}

void audio_set_muted(int muted)
{
    taskENTER_CRITICAL();
    output_muted = !!muted;
    if (initialized)
        apply_output_state();
    taskEXIT_CRITICAL();
}

int audio_set_volume(unsigned percent)
{
    if (percent > 100u) return -EINVAL;
    taskENTER_CRITICAL();
    volume_percent = percent;
    if (initialized) {
        codec.Playback.dgain = volume_gain(percent);
        (void)HAL_ICODEC_DAC_SetDigGain(&codec, codec.Playback.dgain);
        apply_output_state();
    }
    taskEXIT_CRITICAL();
    return 0;
}

unsigned audio_get_volume(void)
{
    taskENTER_CRITICAL();
    unsigned percent = volume_percent;
    taskEXIT_CRITICAL();
    return percent;
}

void audio_request_stop(void)
{
    tone_cancel_requested = 1;
    audio_stop();
}

static int queue_sample(int16_t value)
{
    /* A console stop may interrupt the producer between any two samples.
     * Claim, write and publish one sample atomically with the ring reset.
     */
    taskENTER_CRITICAL();
    if (!running) {
        taskEXIT_CRITICAL();
        return 0;
    }
    if (!fill_frames) {
        if (periods[write_period] != PERIOD_FREE) {
            taskEXIT_CRITICAL();
            return 0;
        }
        periods[write_period] = PERIOD_FILLING;
    }
    playback[write_period][fill_frames++] = value;
    ++counters.output_frames;
    if (fill_frames == AUDIO_PERIOD_FRAMES) {
        CleanDCache_by_Addr((unsigned long *)playback[write_period],
                           sizeof(playback[0]));
        uint64_t now = platform_time_us();
        /* Full 10 ms blocks can be published in bursts; this measures the
         * producer's software publication gap, not individual sample time.
         */
        if (last_ready_us) {
            uint64_t gap = now - last_ready_us;
            if (gap > counters.max_ready_gap_us)
                counters.max_ready_gap_us = gap > UINT32_MAX ? UINT32_MAX : (uint32_t)gap;
        }
        last_ready_us = now;
        periods[write_period] = PERIOD_READY;
        ++ready_periods;
        counters.last_ready_periods = ready_periods;
        counters.last_ready_active = active_period >= 0;
        uint64_t dma_age = active_period >= 0 ? now - dma_started_us : 0;
        counters.last_ready_dma_age_us = dma_age > UINT32_MAX ? UINT32_MAX : (uint32_t)dma_age;
        write_period = (write_period + 1u) % AUDIO_PERIOD_COUNT;
        fill_frames = 0;
        xTaskNotifyGive(service_task);
    }
    taskEXIT_CRITICAL();
    return 1;
}

size_t audio_submit_stereo(const int16_t *samples, size_t frames,
                           uint32_t sample_rate)
{
    size_t consumed = 0;
    if (!running || !samples || sample_rate < 8000u || sample_rate > 192000u)
        return 0;
    if (source_rate && source_rate != sample_rate)
        return 0;
    source_rate = sample_rate;
    counters.input_rate = sample_rate;
    while (consumed < frames && running) {
        int32_t left = samples[consumed * 2u];
        int32_t right = samples[consumed * 2u + 1u];
        int16_t current = (int16_t)((left + right) / 2);
        if (!source_previous_valid) {
            source_previous = current;
            source_previous_valid = 1;
        } else {
            /* Phase is measured in 1/48000 input intervals. If the ring fills
             * halfway through an interval, retain phase and consume neither
             * the current input frame nor its previous endpoint yet.
             */
            while (source_phase < AUDIO_OUTPUT_RATE) {
                int16_t output = (int16_t)(source_previous +
                    ((int64_t)((int32_t)current - source_previous) *
                     source_phase) / AUDIO_OUTPUT_RATE);
                if (!queue_sample(output))
                    return consumed;
                source_phase += sample_rate;
            }
            source_phase -= AUDIO_OUTPUT_RATE;
            source_previous = current;
        }
        ++consumed;
        ++counters.input_frames;
        if (left || right)
            ++counters.input_nonzero_frames;
        uint32_t left_peak = (uint32_t)(left < 0 ? -left : left);
        uint32_t right_peak = (uint32_t)(right < 0 ? -right : right);
        uint32_t peak = left_peak > right_peak ? left_peak : right_peak;
        if (peak > counters.input_peak)
            counters.input_peak = peak;
    }
    return consumed;
}

void audio_get_stats(audio_stats_t *stats)
{
    if (!stats)
        return;
    taskENTER_CRITICAL();
    *stats = counters;
    stats->running = (uint32_t)running;
    stats->muted = (uint32_t)(output_muted || !volume_percent);
    stats->volume_percent = volume_percent;
    stats->queued_frames = ready_periods * AUDIO_PERIOD_FRAMES + fill_frames;
    if (active_period >= 0)
        stats->queued_frames += AUDIO_PERIOD_FRAMES;
    taskEXIT_CRITICAL();
}

void audio_prepare_test_tone(void)
{
    /* Arm when the command is accepted, before a later stop can cancel it. */
    tone_cancel_requested = 0;
}

int audio_test_tone(void)
{
    /* Repeat each table entry twice: 500 Hz at 48000 Hz. Quarter amplitude
     * gives a 1024/32768 peak (-30.10 dBFS). Fixed two-second duration with
     * 50 ms linear fades at both ends; no floating-point state required.
     */
    static const int16_t sine[48] = {
        0,535,1060,1567,2048,2493,2896,3250,3547,3784,3956,4061,
        4096,4061,3956,3784,3547,3250,2896,2493,2048,1567,1060,535,
        0,-535,-1060,-1567,-2048,-2493,-2896,-3250,-3547,-3784,-3956,-4061,
        -4096,-4061,-3956,-3784,-3547,-3250,-2896,-2493,-2048,-1567,-1060,-535
    };
    int16_t stereo[AUDIO_PERIOD_FRAMES * 2u];
    enum { TONE_FRAMES = AUDIO_OUTPUT_RATE * 2u,
           FADE_FRAMES = AUDIO_OUTPUT_RATE / 20u,
           TONE_PERIODS = TONE_FRAMES / AUDIO_PERIOD_FRAMES };
    /* The resampler consumes one endpoint before producing its first frame. */
    uint32_t frames_left = TONE_FRAMES + 1u;
    uint32_t position = 0;
    uint64_t last_progress;
    audio_stats_t stats;
    int result;

    if (tone_cancel_requested)
        goto cancelled;
    result = audio_start();
    if (tone_cancel_requested)
        goto cancelled;
    if (result != 0)
        return -1;
    last_progress = platform_time_us();
    while (frames_left) {
        size_t count = frames_left < AUDIO_PERIOD_FRAMES ?
                       frames_left : AUDIO_PERIOD_FRAMES;
        size_t consumed = 0;
        if (tone_cancel_requested)
            goto cancelled;
        for (size_t i = 0; i < count; ++i) {
            uint32_t envelope = FADE_FRAMES;
            int32_t value = sine[(position / 2u) % 48u] / 4;
            if (position < FADE_FRAMES)
                envelope = position;
            else if (position > TONE_FRAMES - FADE_FRAMES)
                envelope = TONE_FRAMES - position;
            value = value * (int32_t)envelope / FADE_FRAMES;
            stereo[i * 2u] = (int16_t)value;
            stereo[i * 2u + 1u] = (int16_t)value;
            ++position;
        }
        while (consumed < count) {
            if (tone_cancel_requested)
                goto cancelled;
            size_t accepted = audio_submit_stereo(stereo + consumed * 2u,
                                                 count - consumed,
                                                 AUDIO_OUTPUT_RATE);
            consumed += accepted;
            if (accepted)
                last_progress = platform_time_us();
            else {
                if (tone_cancel_requested)
                    goto cancelled;
                audio_get_stats(&stats);
                if (!stats.running ||
                    platform_time_us() - last_progress > 500000u) {
                    if (tone_cancel_requested)
                        goto cancelled;
                    audio_failure("tone-submit-stalled", (int)stats.running);
                    audio_stop();
                    return -1;
                }
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }
        frames_left -= (uint32_t)count;
    }
    last_progress = platform_time_us();
    do {
        if (tone_cancel_requested)
            goto cancelled;
        audio_get_stats(&stats);
        if (!stats.running || platform_time_us() - last_progress > 200000u) {
            if (tone_cancel_requested)
                goto cancelled;
            audio_failure("tone-drain-stalled", (int)stats.running);
            audio_stop();
            return -1;
        }
        if (stats.completed_periods >= TONE_PERIODS)
            break;
        vTaskDelay(pdMS_TO_TICKS(1));
    } while (1);
    if (tone_cancel_requested)
        goto cancelled;
    audio_stop();
    return stats.dma_errors || stats.dma_timeouts || stats.fifo_underruns ? -1 : 0;

cancelled:
    audio_stop();
    return -ECANCELED;
}
