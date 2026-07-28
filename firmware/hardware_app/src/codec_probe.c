/*
 * Locate the AK4619VN control bus.
 *
 * The converter is known from the board: an AKM AK4619VN at U3, addressed
 * over I2C at 7-bit 0b001000C, where the low bit follows the CAD pin. What
 * is not known is which RT1051 pads carry that bus, so this walks the
 * plausible pad pairs and reports which one answers.
 *
 * The bus is bit-banged rather than driven through LPI2C. A scan has to
 * reconfigure pads repeatedly, and bit-banging keeps that self-contained:
 * no peripheral clock roots to set up per instance, and the timing is ours.
 * Codec configuration is a handful of writes at startup, so the peripheral
 * buys nothing here.
 *
 * Pads carrying SEMC and FlexSPI are deliberately excluded. The application
 * executes from SDRAM, so driving an EMC pad would fault the core midway
 * through the scan, and the recovery path depends on the FlexSPI NOR.
 */

#include "codec_probe.h"

#include "MIMXRT1051.h"
#include "fsl_clock.h"
#include "fsl_iomuxc.h"

/*
 * Open drain, keeper and 22k pull-up enabled, so releasing a line lets the
 * pull-up assert it and a slave can still hold it down.
 */
#define NCR2_I2C_PAD_CONFIG UINT32_C(0xF832)

#define NCR2_I2C_HALF_PERIOD_NS UINT32_C(20000)
#define NCR2_NANOSECONDS_PER_SECOND UINT32_C(1000000000)
#define NCR2_I2C_STRETCH_LIMIT UINT32_C(10000)
#define NCR2_AK4619_SIGNATURE_REGISTER UINT8_C(0x04)
#define NCR2_AK4619_SIGNATURE_VALUE UINT8_C(0x22)

typedef struct codec_bus_candidate {
    GPIO_Type *port;
    uint8_t scl_bit;
    uint8_t sda_bit;
} codec_bus_candidate_t;

/*
 * Ordered to match the reported blink code, one based.
 */
static const codec_bus_candidate_t g_candidates[] = {
    { GPIO1, 0U, 1U },    /* 1: AD_B0_00/01, LPI2C1 */
    { GPIO1, 16U, 17U },  /* 2: AD_B1_00/01, LPI2C1 */
    { GPIO1, 12U, 13U },  /* 3: AD_B0_12/13, LPI2C4 */
    { GPIO1, 23U, 22U },  /* 4: AD_B1_07/06, LPI2C3 */
    { GPIO2, 4U, 5U },    /* 5: B0_04/05,    LPI2C2 */
    { GPIO3, 12U, 13U },  /* 6: SD_B0_00/01, LPI2C3 */
    { GPIO3, 4U, 5U },    /* 7: SD_B1_04/05, LPI2C1 */
};

_Static_assert(
    (sizeof(g_candidates) / sizeof(g_candidates[0])) ==
        NCR2_CODEC_BUS_CANDIDATE_COUNT,
    "candidate blink codes must match the documented pad order");

static void mux_candidate(uint32_t index)
{
    CLOCK_EnableClock(kCLOCK_Iomuxc);
    CLOCK_EnableClock(kCLOCK_Gpio1);
    CLOCK_EnableClock(kCLOCK_Gpio2);
    CLOCK_EnableClock(kCLOCK_Gpio3);

    switch (index) {
    case 0U:
        IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B0_00_GPIO1_IO00, 1U);
        IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B0_01_GPIO1_IO01, 1U);
        IOMUXC_SetPinConfig(
            IOMUXC_GPIO_AD_B0_00_GPIO1_IO00, NCR2_I2C_PAD_CONFIG);
        IOMUXC_SetPinConfig(
            IOMUXC_GPIO_AD_B0_01_GPIO1_IO01, NCR2_I2C_PAD_CONFIG);
        break;
    case 1U:
        IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_00_GPIO1_IO16, 1U);
        IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_01_GPIO1_IO17, 1U);
        IOMUXC_SetPinConfig(
            IOMUXC_GPIO_AD_B1_00_GPIO1_IO16, NCR2_I2C_PAD_CONFIG);
        IOMUXC_SetPinConfig(
            IOMUXC_GPIO_AD_B1_01_GPIO1_IO17, NCR2_I2C_PAD_CONFIG);
        break;
    case 2U:
        IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B0_12_GPIO1_IO12, 1U);
        IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B0_13_GPIO1_IO13, 1U);
        IOMUXC_SetPinConfig(
            IOMUXC_GPIO_AD_B0_12_GPIO1_IO12, NCR2_I2C_PAD_CONFIG);
        IOMUXC_SetPinConfig(
            IOMUXC_GPIO_AD_B0_13_GPIO1_IO13, NCR2_I2C_PAD_CONFIG);
        break;
    case 3U:
        IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_07_GPIO1_IO23, 1U);
        IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_06_GPIO1_IO22, 1U);
        IOMUXC_SetPinConfig(
            IOMUXC_GPIO_AD_B1_07_GPIO1_IO23, NCR2_I2C_PAD_CONFIG);
        IOMUXC_SetPinConfig(
            IOMUXC_GPIO_AD_B1_06_GPIO1_IO22, NCR2_I2C_PAD_CONFIG);
        break;
    case 4U:
        IOMUXC_SetPinMux(IOMUXC_GPIO_B0_04_GPIO2_IO04, 1U);
        IOMUXC_SetPinMux(IOMUXC_GPIO_B0_05_GPIO2_IO05, 1U);
        IOMUXC_SetPinConfig(
            IOMUXC_GPIO_B0_04_GPIO2_IO04, NCR2_I2C_PAD_CONFIG);
        IOMUXC_SetPinConfig(
            IOMUXC_GPIO_B0_05_GPIO2_IO05, NCR2_I2C_PAD_CONFIG);
        break;
    case 5U:
        IOMUXC_SetPinMux(IOMUXC_GPIO_SD_B0_00_GPIO3_IO12, 1U);
        IOMUXC_SetPinMux(IOMUXC_GPIO_SD_B0_01_GPIO3_IO13, 1U);
        IOMUXC_SetPinConfig(
            IOMUXC_GPIO_SD_B0_00_GPIO3_IO12, NCR2_I2C_PAD_CONFIG);
        IOMUXC_SetPinConfig(
            IOMUXC_GPIO_SD_B0_01_GPIO3_IO13, NCR2_I2C_PAD_CONFIG);
        break;
    default:
        IOMUXC_SetPinMux(IOMUXC_GPIO_SD_B1_04_GPIO3_IO04, 1U);
        IOMUXC_SetPinMux(IOMUXC_GPIO_SD_B1_05_GPIO3_IO05, 1U);
        IOMUXC_SetPinConfig(
            IOMUXC_GPIO_SD_B1_04_GPIO3_IO04, NCR2_I2C_PAD_CONFIG);
        IOMUXC_SetPinConfig(
            IOMUXC_GPIO_SD_B1_05_GPIO3_IO05, NCR2_I2C_PAD_CONFIG);
        break;
    }
}

static void bus_delay(void)
{
    const uint32_t iterations =
        (SystemCoreClock / (NCR2_NANOSECONDS_PER_SECOND /
                            NCR2_I2C_HALF_PERIOD_NS)) / UINT32_C(8);

    for (volatile uint32_t index = iterations;
         index != UINT32_C(0);
         --index) {
        __NOP();
    }
}

static void release_line(const codec_bus_candidate_t *bus, uint8_t bit)
{
    bus->port->DR_SET = UINT32_C(1) << bit;
}

static void pull_line(const codec_bus_candidate_t *bus, uint8_t bit)
{
    bus->port->DR_CLEAR = UINT32_C(1) << bit;
}

static uint32_t read_line(
    const codec_bus_candidate_t *bus,
    uint8_t bit)
{
    return (bus->port->PSR >> bit) & UINT32_C(1);
}

/*
 * Release SCL and wait for it to actually read high, so a slave holding the
 * clock down cannot desynchronise the transfer. Bounded, because on a pad
 * pair with no device the line may be held low by whatever else it drives.
 */
static int clock_high(const codec_bus_candidate_t *bus)
{
    release_line(bus, bus->scl_bit);
    for (uint32_t spin = UINT32_C(0);
         spin < NCR2_I2C_STRETCH_LIMIT;
         ++spin) {
        if (read_line(bus, bus->scl_bit) != UINT32_C(0)) {
            bus_delay();
            return 0;
        }
    }
    return -1;
}

static int bus_idle(const codec_bus_candidate_t *bus)
{
    const uint32_t mask =
        (UINT32_C(1) << bus->scl_bit) | (UINT32_C(1) << bus->sda_bit);

    bus->port->DR_SET = mask;
    bus->port->GDIR |= mask;
    bus_delay();
    if (clock_high(bus) != 0 ||
        read_line(bus, bus->sda_bit) == UINT32_C(0)) {
        return -1;
    }
    return 0;
}

static int bus_start(const codec_bus_candidate_t *bus)
{
    release_line(bus, bus->sda_bit);
    if (clock_high(bus) != 0 ||
        read_line(bus, bus->sda_bit) == UINT32_C(0)) {
        return -1;
    }
    pull_line(bus, bus->sda_bit);
    bus_delay();
    pull_line(bus, bus->scl_bit);
    bus_delay();
    return 0;
}

static void bus_stop(const codec_bus_candidate_t *bus)
{
    pull_line(bus, bus->sda_bit);
    bus_delay();
    (void)clock_high(bus);
    release_line(bus, bus->sda_bit);
    bus_delay();
}

/* Returns 0 when the slave acknowledged. */
static int bus_write_byte(
    const codec_bus_candidate_t *bus,
    uint8_t value)
{
    uint32_t acknowledged;

    for (uint32_t bit = UINT32_C(0); bit < UINT32_C(8); ++bit) {
        if ((value & UINT8_C(0x80)) != UINT8_C(0)) {
            release_line(bus, bus->sda_bit);
        } else {
            pull_line(bus, bus->sda_bit);
        }
        value = (uint8_t)(value << 1);
        bus_delay();
        if (clock_high(bus) != 0) {
            return -1;
        }
        pull_line(bus, bus->scl_bit);
        bus_delay();
    }

    release_line(bus, bus->sda_bit);
    bus_delay();
    if (clock_high(bus) != 0) {
        return -1;
    }
    acknowledged = read_line(bus, bus->sda_bit);
    pull_line(bus, bus->scl_bit);
    bus_delay();
    return (acknowledged == UINT32_C(0)) ? 0 : -1;
}

/*
 * Probing by acknowledge alone is not sound. v0.7.4 proved that candidate 2
 * held SDA low: every byte looked acknowledged and every read returned
 * zero. Require both released lines to read high and require the AK4619's
 * nonzero reset signature, MIC gain register 0x04 = 0x22.
 */
static int probe_address(
    const codec_bus_candidate_t *bus,
    uint8_t address)
{
    uint8_t received = UINT8_C(0);
    int ok = -1;

    if (bus_idle(bus) != 0) {
        return -1;
    }
    if (bus_start(bus) != 0) {
        return -1;
    }
    if (bus_write_byte(bus, (uint8_t)(address << 1)) == 0 &&
        bus_write_byte(
            bus, NCR2_AK4619_SIGNATURE_REGISTER) == 0 &&
        bus_start(bus) == 0 &&
        bus_write_byte(
            bus, (uint8_t)((address << 1) | UINT8_C(1))) == 0) {
        /* Read one byte and NAK it, then require the bus to recover high. */
        release_line(bus, bus->sda_bit);
        ok = 0;
        for (uint32_t bit = UINT32_C(0); bit < UINT32_C(8); ++bit) {
            received = (uint8_t)(received << 1);
            if (clock_high(bus) != 0) {
                ok = -1;
                break;
            }
            received = (uint8_t)(
                received |
                (uint8_t)read_line(bus, bus->sda_bit));
            pull_line(bus, bus->scl_bit);
            bus_delay();
        }
        release_line(bus, bus->sda_bit);
        bus_delay();
        if (clock_high(bus) != 0) {
            ok = -1;
        }
        pull_line(bus, bus->scl_bit);
        bus_delay();
        if (received != NCR2_AK4619_SIGNATURE_VALUE) {
            ok = -1;
        }
    }
    bus_stop(bus);
    if (read_line(bus, bus->scl_bit) == UINT32_C(0) ||
        read_line(bus, bus->sda_bit) == UINT32_C(0)) {
        ok = -1;
    }
    return ok;
}

static const codec_bus_candidate_t *g_active;
static uint8_t g_active_address;

int ncr2_codec_write_register(uint8_t reg, uint8_t value)
{
    const uint8_t bytes[3] = {
        (uint8_t)(g_active_address << 1), reg, value,
    };
    int status = NCR2_I2C_OK;

    if (g_active == NULL) {
        return NCR2_I2C_BUS_STUCK;
    }
    if (bus_idle(g_active) != 0 ||
        bus_start(g_active) != 0) {
        return NCR2_I2C_BUS_STUCK;
    }
    for (uint32_t index = UINT32_C(0); index < UINT32_C(3); ++index) {
        if (bus_write_byte(g_active, bytes[index]) != 0) {
            /* Byte one is the address, two the register, three the data. */
            status = (int)(index + UINT32_C(1));
            break;
        }
    }
    bus_stop(g_active);
    return status;
}

/*
 * Random address read: address the register with a write, then repeat START
 * and read one byte, NAKing it so the device stops driving. Reading back
 * what was written is the only way to tell a real device from a bus that
 * merely looks like it acknowledged.
 */
int ncr2_codec_read_register(uint8_t reg, uint8_t *value)
{
    uint8_t received = UINT8_C(0);

    if (g_active == NULL || value == NULL) {
        return NCR2_I2C_BUS_STUCK;
    }
    if (bus_idle(g_active) != 0 ||
        bus_start(g_active) != 0) {
        return NCR2_I2C_BUS_STUCK;
    }
    if (bus_write_byte(g_active, (uint8_t)(g_active_address << 1)) != 0) {
        bus_stop(g_active);
        return NCR2_I2C_NAK_ADDRESS;
    }
    if (bus_write_byte(g_active, reg) != 0) {
        bus_stop(g_active);
        return NCR2_I2C_NAK_REGISTER;
    }
    if (bus_start(g_active) != 0) {
        bus_stop(g_active);
        return NCR2_I2C_BUS_STUCK;
    }
    if (bus_write_byte(
            g_active,
            (uint8_t)((g_active_address << 1) | UINT8_C(1))) != 0) {
        bus_stop(g_active);
        return NCR2_I2C_NAK_ADDRESS;
    }

    release_line(g_active, g_active->sda_bit);
    for (uint32_t bit = UINT32_C(0); bit < UINT32_C(8); ++bit) {
        received = (uint8_t)(received << 1);
        if (clock_high(g_active) != 0) {
            bus_stop(g_active);
            return NCR2_I2C_BUS_STUCK;
        }
        received = (uint8_t)(
            received | (uint8_t)read_line(g_active, g_active->sda_bit));
        pull_line(g_active, g_active->scl_bit);
        bus_delay();
    }
    /* NAK the final byte, then stop. */
    release_line(g_active, g_active->sda_bit);
    bus_delay();
    (void)clock_high(g_active);
    pull_line(g_active, g_active->scl_bit);
    bus_stop(g_active);

    *value = received;
    return NCR2_I2C_OK;
}

/*
 * Exact register image written by the verified stock Metal engine.
 *
 * The factory routine at ITCM 0x795c pulses GPIO1_IO26 low-to-high, then
 * writes registers 0x00 through 0x14 in ascending order and reads all 21
 * back. The values below are the source table recovered from DTCM address
 * 0x20000000 by the offline execution trace. Keeping the complete image is
 * important: the earlier source build inferred a five-register subset from
 * the datasheet and changed register 0x12 from the factory value 0x04 to
 * 0x00. That build had working capture and TX DMA but no converter return.
 */
static const uint8_t g_factory_codec_registers[] = {
    0x37U, 0xACU, 0x1CU, 0x03U, 0x22U, 0x22U, 0x30U,
    0x30U, 0x30U, 0x30U, 0x00U, 0x55U, 0x00U, 0x00U,
    0x18U, 0x18U, 0x18U, 0x18U, 0x04U, 0x05U, 0x0AU,
};

uint32_t ncr2_codec_configure(void)
{
    uint32_t failures = UINT32_C(0);

    for (uint32_t index = UINT32_C(0);
         index < sizeof(g_factory_codec_registers);
         ++index) {
        if (ncr2_codec_write_register(
                (uint8_t)index,
                g_factory_codec_registers[index]) != 0) {
            ++failures;
        }
    }
    return failures;
}

uint32_t ncr2_codec_probe(ncr2_codec_bus_t *found)
{
    for (uint32_t index = UINT32_C(0);
         index < NCR2_CODEC_BUS_CANDIDATE_COUNT;
         ++index) {
        const codec_bus_candidate_t *bus = &g_candidates[index];

        mux_candidate(index);

        for (uint8_t address = NCR2_AK4619_ADDRESS_BASE;
             address <= NCR2_AK4619_ADDRESS_BASE + UINT8_C(1);
             ++address) {
            if (probe_address(bus, address) == 0) {
                g_active = bus;
                g_active_address = address;
                if (found != NULL) {
                    found->candidate = index;
                    found->address = address;
                }
                return NCR2_CODEC_PROBE_FOUND;
            }
        }
    }
    return NCR2_CODEC_PROBE_NOT_FOUND;
}
