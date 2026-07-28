#ifndef NCR2_CODEC_PROBE_H
#define NCR2_CODEC_PROBE_H

#include <stdint.h>

/*
 * AK4619VN control address. The datasheet fixes the top seven bits as
 * 0b0010000, with the low bit selected by the CAD pin, so the device
 * answers at 0x10 or 0x11.
 */
#define NCR2_AK4619_ADDRESS_BASE UINT8_C(0x10)

/*
 * Candidate pad pairs, in the order reported as a blink code:
 *
 * 1 AD_B0_00/01   LPI2C1     4 AD_B1_07/06   LPI2C3
 * 2 AD_B1_00/01   LPI2C1     5 B0_04/05      LPI2C2
 * 3 AD_B0_12/13   LPI2C4     6 SD_B0_00/01   LPI2C3
 *                            7 SD_B1_04/05   LPI2C1
 *
 * The EMC pads that can also carry LPI2C are excluded: the application runs
 * from SDRAM behind the SEMC, so muxing one would fault the core mid-scan.
 * SD_B1_04/05 are included because they double as FLEXSPI_A_SS1_B and
 * FLEXSPI_A_DQS, neither of which a single quad NOR uses. SD_B1_10/11 stay
 * excluded because they carry FlexSPI data.
 */
#define NCR2_CODEC_BUS_CANDIDATE_COUNT UINT32_C(7)

#define NCR2_CODEC_PROBE_NOT_FOUND UINT32_C(0)
#define NCR2_CODEC_PROBE_FOUND UINT32_C(1)

typedef struct ncr2_codec_bus {
    uint32_t candidate;
    uint8_t address;
} ncr2_codec_bus_t;

/*
 * Walk the candidate pad pairs looking for an acknowledge from the codec.
 * Returns NCR2_CODEC_PROBE_FOUND and fills `found` on success.
 */
uint32_t ncr2_codec_probe(ncr2_codec_bus_t *found);

/*
 * Transfer outcomes. Anything other than OK names the byte that was
 * rejected, which distinguishes a device that ignores a register write from
 * a bus that never worked at all.
 */
#define NCR2_I2C_OK 0
#define NCR2_I2C_NAK_ADDRESS 1
#define NCR2_I2C_NAK_REGISTER 2
#define NCR2_I2C_NAK_DATA 3
#define NCR2_I2C_BUS_STUCK 4

int ncr2_codec_write_register(uint8_t reg, uint8_t value);
int ncr2_codec_read_register(uint8_t reg, uint8_t *value);

/*
 * Reproduce the complete 0x00..0x14 register image and write order traced
 * from the stock Metal engine. Returns the number of failed writes, so zero
 * means the converter accepted every register. The serial clocks must
 * already be running and GPIO1_IO26 must have completed its low-high pulse.
 */
uint32_t ncr2_codec_configure(void);

#endif
