#include "factory_audio.h"

#include <stddef.h>
#include <stdint.h>

#include "MIMXRT1051.h"
#include "fsl_clock.h"
#include "fsl_iomuxc.h"

#ifndef NCR2_FACTORY_SLOT_AUDIO_PASSTHROUGH
#define NCR2_FACTORY_SLOT_AUDIO_PASSTHROUGH 0
#endif

#ifndef NCR2_FACTORY_AUDIO_TX_DIAGNOSTIC
#define NCR2_FACTORY_AUDIO_TX_DIAGNOSTIC 0
#endif

#ifndef NCR2_FACTORY_AUDIO_TX_NORMAL_DMAMUX
#define NCR2_FACTORY_AUDIO_TX_NORMAL_DMAMUX 0
#endif

#if NCR2_FACTORY_SLOT_AUDIO_PASSTHROUGH

#define NCR2_AUDIO_RX_DMA_CHANNEL UINT32_C(0)
#define NCR2_AUDIO_TX_DMA_CHANNEL UINT32_C(16)
#define NCR2_AUDIO_RX_DMAMUX_SOURCE UINT32_C(19)
#define NCR2_AUDIO_TX_DMAMUX_SOURCE UINT32_C(20)
#define NCR2_AUDIO_DMAMUX_SOURCE_COMPAT_FLAG UINT32_C(0x100)
#define NCR2_AUDIO_DMAMUX_ENABLE UINT32_C(0x80000000)
#define NCR2_AUDIO_DMAMUX_TX_TRIGGER UINT32_C(0x40000000)
#define NCR2_AUDIO_DMA_CONTROL UINT32_C(0x000004c0)
#define NCR2_AUDIO_PAD_CONFIG UINT32_C(0x000010b0)
#define NCR2_AUDIO_TCD_ATTRIBUTES UINT16_C(0x0202)
#define NCR2_AUDIO_MINOR_BYTES UINT32_C(64)
#define NCR2_AUDIO_MAJOR_ITERATIONS UINT16_C(2)
#define NCR2_AUDIO_RX_TCD_CONTROL UINT16_C(0x0012)
#if NCR2_FACTORY_AUDIO_TX_DIAGNOSTIC
#define NCR2_AUDIO_TX_TCD_CONTROL UINT16_C(0x0012)
#else
#define NCR2_AUDIO_TX_TCD_CONTROL UINT16_C(0x0010)
#endif

typedef struct ncr2_audio_tcd
{
    uint32_t source;
    int16_t source_offset;
    uint16_t attributes;
    uint32_t minor_bytes;
    int32_t source_last;
    uint32_t destination;
    int16_t destination_offset;
    uint16_t current_iterations;
    int32_t destination_last_or_next;
    uint16_t control;
    uint16_t starting_iterations;
} ncr2_audio_tcd_t;

_Static_assert(
    sizeof(ncr2_audio_tcd_t) == 32U,
    "software TCD must match RT1051 eDMA layout");

static int32_t g_audio_rx[2][NCR2_FACTORY_AUDIO_WORDS_PER_BLOCK]
    __attribute__((aligned(32)));
static int32_t g_audio_tx[2][NCR2_FACTORY_AUDIO_WORDS_PER_BLOCK]
    __attribute__((aligned(32)));
static ncr2_audio_tcd_t g_audio_rx_tcd[2]
    __attribute__((aligned(32)));
static ncr2_audio_tcd_t g_audio_tx_tcd[2]
    __attribute__((aligned(32)));

static uint32_t address32(const void *pointer)
{
    return (uint32_t)(uintptr_t)pointer;
}

static void clear_words(
    int32_t *words,
    size_t count)
{
    for (size_t index = 0U; index < count; ++index) {
        words[index] = INT32_C(0);
    }
}

static void configure_pins(void)
{
    CLOCK_EnableClock(kCLOCK_Iomuxc);
    CLOCK_EnableClock(kCLOCK_IomuxcGpr);

    IOMUXC_SetPinMux(
        IOMUXC_GPIO_AD_B1_09_SAI1_MCLK,
        1U);
    IOMUXC_SetPinMux(
        IOMUXC_GPIO_AD_B1_12_SAI1_RX_DATA00,
        1U);
    IOMUXC_SetPinMux(
        IOMUXC_GPIO_B1_01_SAI1_TX_DATA00,
        1U);
    IOMUXC_SetPinMux(
        IOMUXC_GPIO_B1_02_SAI1_TX_BCLK,
        1U);
    IOMUXC_SetPinMux(
        IOMUXC_GPIO_B1_03_SAI1_TX_SYNC,
        1U);

    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_AD_B1_09_SAI1_MCLK,
        NCR2_AUDIO_PAD_CONFIG);
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_AD_B1_12_SAI1_RX_DATA00,
        NCR2_AUDIO_PAD_CONFIG);
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_B1_01_SAI1_TX_DATA00,
        NCR2_AUDIO_PAD_CONFIG);
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_B1_02_SAI1_TX_BCLK,
        NCR2_AUDIO_PAD_CONFIG);
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_B1_03_SAI1_TX_SYNC,
        NCR2_AUDIO_PAD_CONFIG);
}

static void configure_clocks(void)
{
    const clock_audio_pll_config_t audio_pll = {
        .loopDivider = UINT8_C(32),
        .postDivider = UINT8_C(1),
        .numerator = UINT32_C(7800),
        .denominator = UINT32_C(10000),
        .src = UINT8_C(0),
    };

    CLOCK_InitAudioPll(&audio_pll);
    CLOCK_SetMux(kCLOCK_Sai1Mux, 2U);
    CLOCK_SetDiv(kCLOCK_Sai1PreDiv, 0U);
    CLOCK_SetDiv(kCLOCK_Sai1Div, 31U);
    CLOCK_EnableClock(kCLOCK_Sai1);
    CLOCK_EnableClock(kCLOCK_Dma);
    IOMUXC_GPR->GPR1 |= IOMUXC_GPR_GPR1_SAI1_MCLK_DIR_MASK;
}

static void configure_sai(void)
{
    /*
     * These are the executed factory register values. The CSR status bits
     * written as one are write-one-to-clear flags on physical hardware.
     */
    SAI1->TCSR = UINT32_C(0);
    SAI1->RCSR = UINT32_C(0);
    SAI1->TCR1 = UINT32_C(0x00000010);
    SAI1->TCR2 = UINT32_C(0x07000001);
    SAI1->TCR3 = UINT32_C(0x00010000);
    SAI1->TCR4 = UINT32_C(0x00031f1b);
    SAI1->TCR5 = UINT32_C(0x1f1f1f00);
    SAI1->RCR1 = UINT32_C(0x00000010);
    SAI1->RCR2 = UINT32_C(0x47000001);
    SAI1->RCR3 = UINT32_C(0x00010000);
    SAI1->RCR4 = UINT32_C(0x00031f1b);
    SAI1->RCR5 = UINT32_C(0x1f1f1f00);
}

static void configure_tcd(
    ncr2_audio_tcd_t *tcd,
    uint32_t source,
    int16_t source_offset,
    uint32_t destination,
    int16_t destination_offset,
    ncr2_audio_tcd_t *next,
    uint16_t control)
{
    tcd->source = source;
    tcd->source_offset = source_offset;
    tcd->attributes = NCR2_AUDIO_TCD_ATTRIBUTES;
    tcd->minor_bytes = NCR2_AUDIO_MINOR_BYTES;
    tcd->source_last = INT32_C(0);
    tcd->destination = destination;
    tcd->destination_offset = destination_offset;
    tcd->current_iterations = NCR2_AUDIO_MAJOR_ITERATIONS;
    tcd->destination_last_or_next =
        (int32_t)address32(next);
    tcd->control = control;
    tcd->starting_iterations =
        NCR2_AUDIO_MAJOR_ITERATIONS;
}

static void install_tcd(
    uint32_t channel,
    const ncr2_audio_tcd_t *tcd)
{
    DMA0->TCD[channel].SADDR = tcd->source;
    DMA0->TCD[channel].SOFF =
        (uint16_t)tcd->source_offset;
    DMA0->TCD[channel].ATTR = tcd->attributes;
    DMA0->TCD[channel].NBYTES_MLNO =
        tcd->minor_bytes;
    DMA0->TCD[channel].SLAST = tcd->source_last;
    DMA0->TCD[channel].DADDR = tcd->destination;
    DMA0->TCD[channel].DOFF =
        (uint16_t)tcd->destination_offset;
    DMA0->TCD[channel].CITER_ELINKNO =
        tcd->current_iterations;
    DMA0->TCD[channel].DLAST_SGA =
        tcd->destination_last_or_next;
    DMA0->TCD[channel].CSR = tcd->control;
    DMA0->TCD[channel].BITER_ELINKNO =
        tcd->starting_iterations;
}

static void configure_dma(void)
{
    const uint32_t receive_data =
        (uint32_t)(uintptr_t)&SAI1->RDR[0];
    const uint32_t transmit_data =
        (uint32_t)(uintptr_t)&SAI1->TDR[0];

    clear_words(
        &g_audio_rx[0][0],
        NCR2_FACTORY_AUDIO_WORDS_PER_BLOCK);
    clear_words(
        &g_audio_rx[1][0],
        NCR2_FACTORY_AUDIO_WORDS_PER_BLOCK);
    clear_words(
        &g_audio_tx[0][0],
        NCR2_FACTORY_AUDIO_WORDS_PER_BLOCK);
    clear_words(
        &g_audio_tx[1][0],
        NCR2_FACTORY_AUDIO_WORDS_PER_BLOCK);

    configure_tcd(
        &g_audio_rx_tcd[0],
        receive_data,
        INT16_C(0),
        address32(&g_audio_rx[0][0]),
        INT16_C(4),
        &g_audio_rx_tcd[1],
        NCR2_AUDIO_RX_TCD_CONTROL);
    configure_tcd(
        &g_audio_rx_tcd[1],
        receive_data,
        INT16_C(0),
        address32(&g_audio_rx[1][0]),
        INT16_C(4),
        &g_audio_rx_tcd[0],
        NCR2_AUDIO_RX_TCD_CONTROL);
    configure_tcd(
        &g_audio_tx_tcd[0],
        address32(&g_audio_tx[0][0]),
        INT16_C(4),
        transmit_data,
        INT16_C(0),
        &g_audio_tx_tcd[1],
        NCR2_AUDIO_TX_TCD_CONTROL);
    configure_tcd(
        &g_audio_tx_tcd[1],
        address32(&g_audio_tx[1][0]),
        INT16_C(4),
        transmit_data,
        INT16_C(0),
        &g_audio_tx_tcd[0],
        NCR2_AUDIO_TX_TCD_CONTROL);

    DMAMUX->CHCFG[NCR2_AUDIO_RX_DMA_CHANNEL] =
        NCR2_AUDIO_DMAMUX_ENABLE |
        NCR2_AUDIO_DMAMUX_SOURCE_COMPAT_FLAG |
        NCR2_AUDIO_RX_DMAMUX_SOURCE;
    DMAMUX->CHCFG[NCR2_AUDIO_TX_DMA_CHANNEL] =
        NCR2_AUDIO_DMAMUX_ENABLE |
#if !NCR2_FACTORY_AUDIO_TX_NORMAL_DMAMUX
        /*
         * Preserve the executed factory value in the reusable compatibility
         * target. The RT1050 reference manual only defines periodic trigger
         * mode for channels 0..3, so the hardware diagnostic deliberately
         * omits this bit on channel 16 and uses ordinary peripheral-request
         * routing.
         */
        NCR2_AUDIO_DMAMUX_TX_TRIGGER |
#endif
        NCR2_AUDIO_DMAMUX_SOURCE_COMPAT_FLAG |
        NCR2_AUDIO_TX_DMAMUX_SOURCE;

    DMA0->CR = NCR2_AUDIO_DMA_CONTROL;
    DMA0->CERQ = (uint8_t)NCR2_AUDIO_TX_DMA_CHANNEL;
    DMA0->CERQ = (uint8_t)NCR2_AUDIO_RX_DMA_CHANNEL;
    DMA0->CINT = (uint8_t)NCR2_AUDIO_RX_DMA_CHANNEL;

    install_tcd(NCR2_AUDIO_RX_DMA_CHANNEL, &g_audio_rx_tcd[0]);
    install_tcd(NCR2_AUDIO_TX_DMA_CHANNEL, &g_audio_tx_tcd[0]);

    NVIC_ClearPendingIRQ(DMA0_DMA16_IRQn);
    NVIC_EnableIRQ(DMA0_DMA16_IRQn);
    DMA0->SERQ = (uint8_t)NCR2_AUDIO_TX_DMA_CHANNEL;
    DMA0->SERQ = (uint8_t)NCR2_AUDIO_RX_DMA_CHANNEL;
}

#endif

ncr2_factory_audio_counters_t
    g_ncr2_factory_audio_counters;

__attribute__((weak))
void ncr2_factory_audio_process_block(
    const int32_t *input,
    int32_t *output,
    size_t frames)
{
    const size_t words =
        frames * NCR2_FACTORY_AUDIO_SLOTS;

    for (size_t index = 0U; index < words; ++index) {
        output[index] = input[index];
    }
}

uint16_t ncr2_factory_audio_init(void)
{
#if NCR2_FACTORY_SLOT_AUDIO_PASSTHROUGH
    __disable_irq();
    configure_pins();
    configure_clocks();
    configure_sai();
    configure_dma();

    SAI1->TCSR =
        UINT32_C(0x80050001);
    SAI1->RCSR =
        UINT32_C(0x80050001);
    __DSB();
    __ISB();
    __enable_irq();
    return NCR2_FACTORY_AUDIO_OK;
#else
    return NCR2_FACTORY_AUDIO_DISABLED;
#endif
}

void DMA0_DMA16_IRQHandler(void)
{
#if NCR2_FACTORY_SLOT_AUDIO_PASSTHROUGH
    const uint32_t pending = DMA0->INT;
    uint32_t completed;
    uint32_t next;

    /*
     * Channels 0 and 16 share this vector. The recovered TX TCD did not
     * request interrupts, but the hardware diagnostic enables its major
     * interrupt so "the RX callback filled a TX buffer" cannot be mistaken
     * for proof that channel 16 consumed it.
     */
    if ((pending & (UINT32_C(1) << NCR2_AUDIO_TX_DMA_CHANNEL)) != 0U) {
        DMA0->CINT = (uint8_t)NCR2_AUDIO_TX_DMA_CHANNEL;
        ++g_ncr2_factory_audio_counters.tx_blocks;
    }

    if ((pending & (UINT32_C(1) << NCR2_AUDIO_RX_DMA_CHANNEL)) == 0U) {
        if ((pending & (UINT32_C(1) << NCR2_AUDIO_TX_DMA_CHANNEL)) != 0U) {
            return;
        }
        ++g_ncr2_factory_audio_counters.unexpected_interrupts;
        DMA0->CINT = UINT8_C(0x40);
        return;
    }

    DMA0->CINT = (uint8_t)NCR2_AUDIO_RX_DMA_CHANNEL;
    next = (uint32_t)DMA0->TCD[
        NCR2_AUDIO_RX_DMA_CHANNEL
    ].DLAST_SGA;
    if (next == address32(&g_audio_rx_tcd[0])) {
        completed = UINT32_C(0);
    } else if (next == address32(&g_audio_rx_tcd[1])) {
        completed = UINT32_C(1);
    } else {
        ++g_ncr2_factory_audio_counters.unexpected_interrupts;
        return;
    }

    ++g_ncr2_factory_audio_counters.rx_blocks;
    ncr2_factory_audio_process_block(
        &g_audio_rx[completed][0],
        &g_audio_tx[completed][0],
        NCR2_FACTORY_AUDIO_FRAMES_PER_BLOCK);
    ++g_ncr2_factory_audio_counters.copied_blocks;
    __DSB();
#else
    ++g_ncr2_factory_audio_counters.unexpected_interrupts;
#endif
}
