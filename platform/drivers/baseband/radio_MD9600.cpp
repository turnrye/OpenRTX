/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Radio driver of the MD-9600 V5 board: one LMX2571 synthesizer per band and
 * no programmable IF chip. FM audio goes from the receiver's discriminator to
 * the speaker through switches, not through the HR_C6000. Sequences and
 * values follow the stock S126.061 firmware, checked against RadioFoundry,
 * docs/spec/hardware-md9600.md, https://github.com/radiofoundry/radiofoundry
 *
 * Transmission: FM, with CTCSS and the 1750 Hz tone from the HR_C6000, and
 * M17 through the HR_C6000 physical layer mode, at the stock firmware's PA
 * drives.
 * There is no lock detect and no temperature, SWR or power sensing on this
 * board.
 *
 * The V1-V3 boards have a SKY72310 synthesizer and an AK2365A IF chip per
 * band instead, and only receive. Their sequence and values are ported from
 * RadioFoundry, boards/md9600/rfseq_md9600.c md9600_rf_rx() and
 * rfplan_md9600.c plan_sky72310(), measured on a V1-V3 radio,
 * https://github.com/radiofoundry/radiofoundry, Copyright (c) 2026 The
 * RadioFoundry Authors, MIT licence.
 */

#include "interfaces/delays.h"
#include "interfaces/nvmem.h"
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "peripherals/adc.h"
#include "peripherals/gpio.h"
#include "calibration/calibInfo_MD9600.h"
#include "core/ctcssDetector.hpp"
#include "core/utils.h"
#include "core/threads.h"
#include <cstring>
#include <pthread.h>
#include "drivers/audio/stm32_adc.h"
#include "drivers/SPI/spi_bitbang.h"
#include "drivers/baseband/AK2365A.h"
#include "drivers/baseband/HR_C6000.h"
#include "drivers/baseband/LMX2571.h"
#include "drivers/baseband/SKY72310.h"
#include "drivers/baseband/radioUtils.h"
#include "drivers/tones/toneGenerator_MDx.h"
#include "hwconfig.h"

static constexpr freq_t IF_FREQ = 49950000; // VHF LO above, UHF LO below
static constexpr unsigned int PLL_RESET_MS = 10;
static constexpr unsigned int RX_SETTLE_MS = 4;
static constexpr rssi_t RSSI_NO_SIGNAL = -154;
static constexpr uint32_t CTCSS_SAMPLE_RATE = 2000;

// No lock detect: settle time of the synthesizer before the PA, as RadioFoundry
static constexpr unsigned int TX_SETTLE_MS = 12;

/*
 * PA ramps of the stock S126.061 firmware (tables at 0x080c7378 and
 * 0x080c7388): fractions of the drive, over 255, one step every 100 us.
 */
static constexpr unsigned int RAMP_STEP_US = 100;
static constexpr uint8_t RAMP_UP[] = {
    0, 0, 0, 0, 80, 160, 180, 200, 210, 220, 230, 240, 250, 255, 255, 255,
};
static constexpr uint8_t RAMP_DOWN[] = {
    250, 240, 230, 220, 200, 180, 160, 100, 50, 0, 0, 0, 0, 0, 0,
};

/*
 * HR_C6000 tone generator deviation for the 1750 Hz tone, the CS7000's
 * value: to be checked off air on the MD-9600.
 */
static constexpr uint8_t TONE_DEVIATION = 0x1E;
static constexpr uint32_t TONE_BURST_FREQ = 1750;

/*
 * Tones of the calibration's CTCSS level rows, in tenths of Hz, and the
 * largest level the stock firmware doubles for 25 kHz channels: from a
 * level of 0x80 it sends the full swing (S126.061 at 0x0805017c).
 */
static constexpr uint16_t CTCSS_ROW_TONE[MD9600_CTCSS_ROWS] = {
    670,
    1514,
    2541,
};
static constexpr uint8_t CTCSS_FULL_SWING_LEVEL = 0x80;

/*
 * Supply range for transmission, in mV: RadioFoundry's cutoff and maximum,
 * docs/spec/hardware-md9600.md section 10.3. The stock firmware also ends
 * transmissions on supply faults.
 */
static constexpr uint16_t TX_MIN_SUPPLY_MV = 10000;
static constexpr uint16_t TX_MAX_SUPPLY_MV = 15000;

// Power levels offered (hwconfig.h), one per PA level, lowest first
static constexpr uint32_t PA_LEVEL_MW[] = { CONFIG_TX_POWER_LEVELS };
static_assert(ARRAY_SIZE(PA_LEVEL_MW) == MD9600_PA_LEVELS,
              "One power level per PA drive row");

/*
 * HR_C6000 modulation factor, by band (VHF, UHF) and width (12.5 kHz, wider),
 * from the stock S126.061 firmware (0x0804fb58).
 */
static constexpr uint8_t TX_DEVIATION[2][2] = {
    { 0x32, 0x64 },
    { 0x14, 0x28 },
};

/*
 * M17 in the HR_C6000 physical layer mode (RadioFoundry,
 * docs/spec/modem-hrc6000.md 10.3). The chip sends its TX RAM, two halves of
 * 36 bytes (144 symbols, 30 ms), in alternate slots; M17 frame bytes are the
 * dibits it takes. The calibration's DMR modulation pair is set for a 1944 Hz
 * outer deviation and scales linearly: M17 wants 2400 Hz. Measured on a V5
 * board, 2402/800 Hz at 446.0 MHz and 2374/801 Hz at 146.52 MHz.
 */
static constexpr uint32_t M17_DEV_HZ = 2400;
static constexpr uint32_t DMR_CAL_DEV_HZ = 1944;
static constexpr size_t M17_SLOT_BYTES = 36;
static constexpr size_t M17_FIFO_SIZE = 2048;
static constexpr uint8_t M17_IDLE = 0x77; // +3 -3, the preamble
static constexpr uint32_t M17_TS_TIMEOUT_MS = 200;

/*
 * V5 RSSI: dBm at a reading of 0, and counts per 100 dB, both bands.
 * RadioFoundry's V4A/V5 fit, docs/spec/hardware-md9600.md section 5.7. On a
 * V5 into a dummy load it puts the floor at -123 dBm (VHF) and -129 dBm (UHF),
 * the receiver's noise floor. The calibration's RSSI points are in the stock
 * firmware's units (15 ADC cycles a sample, reading lower) and don't apply.
 */
static constexpr rssi_t V5_RSSI_FLOOR = -155;
static constexpr int32_t V5_RSSI_COUNTS_PER_100_DB = 195;

/*
 * V1-V3 boards, by band (VHF, UHF), from RadioFoundry (file header): the
 * SKY72310 reference divider, 2.8 and 4.2 MHz at the phase detector, and the
 * VCO, at twice the LO on VHF. The settle times are RadioFoundry's: no lock
 * detect, and the AK2365A calibration takes 1.3 ms.
 */
static constexpr uint8_t SKY_REF_DIV[] = { 6, 4 };
static constexpr uint8_t SKY_VCO_MULT[] = { 2, 1 };
static constexpr uint8_t SKY_PD_GAIN = 31;
static constexpr unsigned int SKY_SETTLE_MS = 2;
static constexpr unsigned int IF_CAL_MS = 2;
static constexpr unsigned int V1V3_RX_SETTLE_MS = 3;

/*
 * V1-V3 RSSI, by band (VHF, UHF): dBm at a reading of 0, then half a dB per
 * count. RadioFoundry's fit, docs/spec/hardware-md9600.md section 5.7, not a
 * measurement.
 */
static constexpr rssi_t V1V3_RSSI_FLOOR[] = { -135, -145 };

/**
 * RF section of a board revision: its synthesizers and IF stage. Supplies,
 * front-end tuning, audio switches and the HR_C6000 are the same on all.
 */
struct rf_ops {
    void (*init)();      ///< Set up, receivers off.
    void (*rxPowerUp)(); ///< Power up the receivers, tuned on rxBand.
    void (*rxOff)();     ///< Revision's own receive lines off.
    rssi_t (*rssi)(const enum Band band, const int32_t raw); ///< 8-bit RSSI.
};

static const rtxStatus_t *config;
static struct md9600Calib calData;
static enum Band rxBand = BND_NONE;
static uint8_t rxTune;
static enum Band txBand = BND_NONE;
static uint8_t txDrive; // PA drive (APC DAC), 8 bit
static uint8_t txModI;
static uint8_t txModQ;
static uint8_t txDeviation;
static uint8_t txM17Mod1;       // HR_C6000 MOD1 (0x46) for M17
static uint8_t txM17Mod2;       // HR_C6000 MOD2 (0x45) for M17
static uint8_t txCtcssLevel;    // CTCSS PWM swing on PC7
static enum opstatus radioStatus;
static const struct rf_ops *rf; // RF section of the board, NULL if unknown

// CTCSS detection on the sub-audio input, as on the CS7000
static int16_t __attribute__((section(".bss2"))) ctcssSamples[128];
static streamCtx ctcssCtx;
static int16_t *prevCtcssBuf;
static CtcssDetector ctcss(ctcssCoeffs2k, (CTCSS_SAMPLE_RATE / 4), 20.0f);

// Synthesizers and IF chips, indexed by enum Band
static const struct lmx2571 *const pll[] = { &pllVhf, &pllUhf };
static const struct sky73210 *const skyPll[] = { &skyVhf, &skyUhf };
static const struct ak2365a *const ifChip[] = { &ifVhf, &ifUhf };

HR_C6000 C6000((const struct spiDevice *)&c6000_spi, { DMR_CS });

/**
 * Configure a gpio as an output, driven low.
 *
 * @param port: gpio port.
 * @param pin: gpio pin number.
 */
static void setOutputLow(GPIO_TypeDef *port, const uint8_t pin)
{
    gpio_setMode(port, pin, OUTPUT);
    gpio_clearPin(port, pin);
}

/**
 * Write the front-end tuning DAC of a band. The 8-bit value goes left-aligned
 * into the 12-bit DAC, as in the stock firmware.
 *
 * @param band: band to tune.
 * @param value: tuning value from calibration.
 */
static void setFrontEndTune(const enum Band band, const uint8_t value)
{
    if (band == BND_VHF)
        DAC->DHR12L2 = value * 0xFF;
    else
        DAC->DHR12L1 = value * 0xFF;
}

/**
 * V5: IF filter and route lines off, then reset both synthesizers once, as
 * the stock firmware does at boot.
 */
static void v5Init()
{
    setOutputLow(IF_BW_UHF);
    setOutputLow(IF_BW_VHF);
    setOutputLow(IF_ROUTE_UHF);
    setOutputLow(IF_ROUTE_VHF);

    spiBitbang_init(&pll_spi);
    LMX2571_init(&pllVhf);
    LMX2571_init(&pllUhf);
    sleepFor(0, PLL_RESET_MS);
}

/**
 * V5: power up and tune the receivers, in the order of the stock firmware's
 * RX entry (S126.061 at 0x08049a74): both receivers are powered, only the
 * current band is tuned and routed to the HR_C6000.
 */
static void v5RxPowerUp()
{
    const bool vhf = (rxBand == BND_VHF);
    const bool wide = (config->bandwidth != BW_12_5)
                   || (config->opMode == OPMODE_M17);

    gpio_clearPin(RF_APC_EN);
    gpio_clearPin(RF_T5_UHF);
    gpio_clearPin(RF_T5_VHF);

    if (vhf)
        wide ? gpio_setPin(IF_BW_VHF) : gpio_clearPin(IF_BW_VHF);
    else
        wide ? gpio_setPin(IF_BW_UHF) : gpio_clearPin(IF_BW_UHF);

    gpio_setPin(VCO_SEL_UHF);
    gpio_setPin(VCO_SEL_VHF);
    gpio_setPin(RF_C5_UHF);
    gpio_setPin(RF_C5_VHF);

    freq_t lo = config->rxFrequency - IF_FREQ;
    if (vhf)
        lo = config->rxFrequency + IF_FREQ;

    LMX2571_setFrequency(pll[rxBand], lo);
    setFrontEndTune(rxBand, rxTune);

    gpio_setPin(RF_R5_UHF);
    gpio_setPin(RF_R5_VHF);
    sleepFor(0, RX_SETTLE_MS);

    vhf ? gpio_setPin(IF_ROUTE_VHF) : gpio_clearPin(IF_ROUTE_VHF);
    vhf ? gpio_clearPin(IF_ROUTE_UHF) : gpio_setPin(IF_ROUTE_UHF);
}

/**
 * V5: disconnect both IFs from the HR_C6000.
 */
static void v5RxOff()
{
    gpio_clearPin(IF_ROUTE_UHF);
    gpio_clearPin(IF_ROUTE_VHF);
}

/**
 * V5: the RSSI reading through RadioFoundry's fit, the same on both bands.
 *
 * @param band: receiving band.
 * @param raw: 8-bit RSSI reading.
 * @return the RSSI, in dBm.
 */
static rssi_t v5Rssi(const enum Band band, const int32_t raw)
{
    (void)band;

    return V5_RSSI_FLOOR + (raw * 100) / V5_RSSI_COUNTS_PER_100_DB;
}

// C++14 has no designated initializers: fields in declaration order
static const struct rf_ops v5Ops = {
    v5Init,      // init
    v5RxPowerUp, // rxPowerUp
    v5RxOff,     // rxOff
    v5Rssi,      // rssi
};

/**
 * V1-V3: the synthesizer and IF chip buses. The chips are written on every
 * power-up of the receivers, which also powers the IF chips.
 */
static void v1v3Init()
{
    spiBitbang_init(&pll_spi);
    spiBitbang_init(&if_spi);
}

/**
 * V1-V3: power up and tune the receivers, as RadioFoundry (file header):
 * both receivers powered on their receive VCOs, the band's synthesizer on the
 * LO, the front-end tuned, then the band's IF chip set up. The IF chips share
 * their reset line: the other band's is left in standby.
 */
static void v1v3RxPowerUp()
{
    const bool wide = (config->bandwidth != BW_12_5)
                   || (config->opMode == OPMODE_M17);

    gpio_clearPin(RF_APC_EN);
    gpio_clearPin(RF_T5_UHF);
    gpio_clearPin(RF_T5_VHF);

    gpio_setPin(RF_R5_UHF);
    gpio_setPin(RF_R5_VHF);
    gpio_setPin(RF_C5_UHF);
    gpio_setPin(RF_C5_VHF);
    gpio_setPin(VCO_SEL_UHF);
    gpio_setPin(VCO_SEL_VHF);

    freq_t lo = config->rxFrequency - IF_FREQ;
    if (rxBand == BND_VHF)
        lo = config->rxFrequency + IF_FREQ;

    SKY73210_init(skyPll[rxBand], SKY_PD_GAIN);
    SKY73210_setFrequency(skyPll[rxBand], lo * SKY_VCO_MULT[rxBand],
                          SKY_REF_DIV[rxBand]);
    sleepFor(0, SKY_SETTLE_MS);

    setFrontEndTune(rxBand, rxTune);

    if (wide)
        AK2365A_setupRx(ifChip[rxBand], AK2365A_BPF_6, true);
    else
        AK2365A_setupRx(ifChip[rxBand], AK2365A_BPF_3, false);

    sleepFor(0, IF_CAL_MS + V1V3_RX_SETTLE_MS);
}

/**
 * V1-V3: hold both IF chips in reset while their supply is off.
 */
static void v1v3RxOff()
{
    AK2365A_terminate(&ifVhf);
    AK2365A_terminate(&ifUhf);
}

/**
 * V1-V3: the RSSI reading on RadioFoundry's fit.
 *
 * @param band: receiving band.
 * @param raw: 8-bit RSSI reading.
 * @return the RSSI, in dBm.
 */
static rssi_t v1v3Rssi(const enum Band band, const int32_t raw)
{
    return V1V3_RSSI_FLOOR[band] + (raw / 2);
}

static const struct rf_ops v1v3Ops = {
    v1v3Init,      // init
    v1v3RxPowerUp, // rxPowerUp
    v1v3RxOff,     // rxOff
    v1v3Rssi,      // rssi
};

/**
 * Band of a frequency, from the band edges in the radio's hardware info: the
 * MD-9600's UHF band reaches 480 MHz, past the shared band table.
 *
 * @param freq: frequency, in Hz.
 * @return the band, BND_NONE outside both.
 */
static enum Band bandOf(const freq_t freq)
{
    static constexpr freq_t MHZ = 1000000;
    const hwInfo_t *hw = platform_getHwInfo();

    if ((freq >= hw->vhf_minFreq * MHZ) && (freq <= hw->vhf_maxFreq * MHZ))
        return BND_VHF;

    if ((freq >= hw->uhf_minFreq * MHZ) && (freq <= hw->uhf_maxFreq * MHZ))
        return BND_UHF;

    return BND_NONE;
}

/**
 * Number of calibration points of a band.
 *
 * @param band: band.
 * @return the number of points.
 */
static uint8_t calPoints(const enum Band band)
{
    if (band == BND_VHF)
        return MD9600_CAL_POINTS_VHF;

    return MD9600_CAL_POINTS_UHF;
}

/**
 * Linear interpolation between two points, as the stock firmware's
 * 0x08030d34: the end values outside them.
 *
 * @param x: where to interpolate.
 * @param x0: first point.
 * @param x1: second point, above the first.
 * @param y0: value at the first point.
 * @param y1: value at the second point.
 * @return interpolated value.
 */
static uint8_t interpLinear(const int32_t x, const int32_t x0, const int32_t x1,
                            const int32_t y0, const int32_t y1)
{
    if (x <= x0)
        return y0;

    if (x >= x1)
        return y1;

    return y0 + ((x - x0) * (y1 - y0)) / (x1 - x0);
}

/**
 * CTCSS PWM swing for a tone, as the stock firmware sets it (S126.061 at
 * 0x0805017c): each level row interpolated at the frequency, then the two
 * rows around the tone interpolated at the tone, doubled for 25 kHz.
 *
 * @param cal: calibration of the band.
 * @param freq: transmit frequency.
 * @param points: calibration points of the band.
 * @param tone: CTCSS tone, in tenths of Hz.
 * @param wide: true for a 25 kHz channel.
 * @return peak-to-peak swing, in PWM steps of 256.
 */
static uint8_t ctcssLevel(const struct md9600BandCalib *cal, const freq_t freq,
                          const uint8_t points, const uint16_t tone,
                          const bool wide)
{
    uint8_t row[MD9600_CTCSS_ROWS];

    for (size_t i = 0; i < MD9600_CTCSS_ROWS; i++)
        row[i] = interpCalParameter(freq, cal->rxFreq, cal->ctcssLevel[i],
                                    points);

    size_t lo = MD9600_CTCSS_67_0;
    if (tone > CTCSS_ROW_TONE[MD9600_CTCSS_151_4])
        lo = MD9600_CTCSS_151_4;

    const uint8_t level = interpLinear(
        tone, CTCSS_ROW_TONE[lo], CTCSS_ROW_TONE[lo + 1], row[lo], row[lo + 1]);
    if (!wide)
        return level;

    if (level >= CTCSS_FULL_SWING_LEVEL)
        return UINT8_MAX;

    return 2 * level;
}

/**
 * PA level for a requested power: the highest whose factory label does not
 * exceed it, the lowest otherwise.
 *
 * @param power: requested power, in mW.
 * @return the PA level.
 */
static uint8_t paLevel(const uint32_t power)
{
    uint8_t level = MD9600_PA_LEVEL_5W;

    for (uint8_t i = 0; i < MD9600_PA_LEVELS; i++) {
        if (power >= PA_LEVEL_MW[i])
            level = i;
    }

    return level;
}

/**
 * Step the PA drive (APC reference, DAC2) through a ramp table.
 *
 * @param table: fractions of the drive, over 255.
 * @param len: number of steps.
 */
static void paRamp(const uint8_t *table, const size_t len)
{
    for (size_t i = 0; i < len; i++) {
        DAC->DHR12L2 = txDrive * table[i];
        delayUs(RAMP_STEP_US);
    }
}

/**
 * Retune for transmission, as the stock firmware (S126.061 at 0x08049800):
 * PA loop and receivers off, then the band's TX VCO, supply and modulation
 * switch, then the synthesizer on the carrier.
 */
static void txRetune()
{
    gpio_clearPin(RF_APC_EN);
    gpio_clearPin(RF_T5_UHF);
    gpio_clearPin(RF_T5_VHF);
    gpio_clearPin(FM_MUTE);
    gpio_clearPin(RF_R5_UHF);
    gpio_clearPin(RF_R5_VHF);
    gpio_clearPin(IF_ROUTE_UHF);
    gpio_clearPin(IF_ROUTE_VHF);

    // In RX, PA5 holds the VHF front-end tune, above full drive: clear it
    DAC->DHR12R2 = 0;
    DAC->DHR12R1 = 0;

    if (txBand == BND_VHF) {
        gpio_clearPin(VCO_SEL_VHF);
        gpio_setPin(RF_C5_VHF);
        gpio_clearPin(RF_C5_UHF);
        gpio_setPin(MOD_SW);
    } else {
        gpio_clearPin(VCO_SEL_UHF);
        gpio_setPin(RF_C5_UHF);
        gpio_clearPin(RF_C5_VHF);
        gpio_clearPin(MOD_SW);
    }

    LMX2571_setFrequency(pll[txBand], config->txFrequency);
}

/**
 * Power the PA: drive at zero, the band's transmitter supply and the power
 * loop on, then the stock firmware's ramp (S126.061 at 0x0806f19e).
 */
static void paOn()
{
    DAC->DHR12R2 = 0;

    if (txBand == BND_VHF)
        gpio_setPin(RF_T5_VHF);
    else
        gpio_setPin(RF_T5_UHF);

    gpio_setPin(RF_APC_EN);
    paRamp(RAMP_UP, ARRAY_SIZE(RAMP_UP));
}

/**
 * Ramp the PA down, then drive at zero and supply and power loop off, as the
 * stock firmware (S126.061 at 0x0806f256, 0x0806f288).
 */
static void paOff()
{
    paRamp(RAMP_DOWN, ARRAY_SIZE(RAMP_DOWN));
    DAC->DHR12R2 = 0;
    gpio_clearPin(RF_T5_UHF);
    gpio_clearPin(RF_T5_VHF);
    gpio_clearPin(RF_APC_EN);
}

void radio_init(const rtxStatus_t *rtxState)
{
    config = rtxState;
    radioStatus = OFF;

    /*
     * Nothing tells a V4 board from a V1-V3 one: both have the AK2365A IF
     * chips, and the V4's synthesizer is write-only. A V4 gets the V1-V3
     * words and can only mistune its receiver. Neither transmits. RadioFoundry,
     * docs/spec/hardware-md9600.md section 16.
     */
    const uint16_t flags = platform_getHwInfo()->flags;
    rf = NULL;
    if (flags & MD9600_FLAGS_V5)
        rf = &v5Ops;
    else if (flags & MD9600_FLAGS_V1_V4)
        rf = &v1v3Ops;

    // Unknown board: leave the RF section alone
    if (rf == NULL)
        return;

    /*
     * RF control lines, all off. The receiver supplies take the SWD pins: from
     * here on, a debugger connects under reset only.
     */
    setOutputLow(RF_C5_UHF);
    setOutputLow(RF_C5_VHF);
    setOutputLow(RF_R5_UHF);
    setOutputLow(RF_R5_VHF);
    setOutputLow(RF_T5_UHF);
    setOutputLow(RF_T5_VHF);
    setOutputLow(RF_APC_EN);
    setOutputLow(VCO_SEL_UHF);
    setOutputLow(VCO_SEL_VHF);
    setOutputLow(MOD_SW);
    setOutputLow(FM_SW);
    setOutputLow(AF_BAND);
    setOutputLow(FM_MUTE);

    // Front-end tuning DACs
    gpio_setMode(TUNE_UHF, ANALOG);
    gpio_setMode(TUNE_VHF, ANALOG);
    RCC->APB1ENR |= RCC_APB1ENR_DACEN;
    DAC->CR = DAC_CR_EN1 | DAC_CR_EN2;
    DAC->DHR12R1 = 0;
    DAC->DHR12R2 = 0;

    // ADC3 stream for CTCSS detection
    gpio_setMode(AIN_CTCSS, ANALOG);
    ctcssCtx.buffer = ctcssSamples;
    ctcssCtx.bufSize = ARRAY_SIZE(ctcssSamples);
    ctcssCtx.bufMode = BUF_CIRC_DOUBLE;
    ctcssCtx.sampleRate = CTCSS_SAMPLE_RATE;
    ctcssCtx.running = 0;
    stm32adc_init(STM32_ADC_ADC3);

    nvm_readCalibData(&calData);
    rf->init();

    // HR_C6000 timeslot interrupt on PC0, falling edge, masked until an M17
    // transmission
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    __DSB();
    gpio_setMode(GPIOC, 0, INPUT);
    SYSCFG->EXTICR[0] = (SYSCFG->EXTICR[0] & ~SYSCFG_EXTICR1_EXTI0)
                      | SYSCFG_EXTICR1_EXTI0_PC;
    EXTI->FTSR |= EXTI_FTSR_TR0;
    EXTI->RTSR &= ~EXTI_RTSR_TR0;
    EXTI->IMR &= ~EXTI_IMR_MR0;
    NVIC_SetPriority(EXTI0_IRQn, 10);
}

void radio_terminate()
{
    radio_disableRtx();

    DAC->DHR12R1 = 0;
    DAC->DHR12R2 = 0;
    RCC->APB1ENR &= ~RCC_APB1ENR_DACEN;
}

void radio_setOpmode(const enum opmode mode)
{
    // M17 is received through the same FM stages, from the discriminator
    if ((mode == OPMODE_FM) || (mode == OPMODE_M17))
        gpio_setPin(FM_SW);
    else
        gpio_clearPin(FM_SW);
}

bool radio_checkRxDigitalSquelch()
{
    int16_t *data;

    // Sampling stopped: no tone squelch requested, or not receiving
    if (ctcssCtx.running == 0)
        return false;

    // Update the detector each time the ADC has new data
    size_t len = stm32_adc_audio_driver.data(&ctcssCtx, &data);
    if (data != prevCtcssBuf) {
        prevCtcssBuf = data;
        ctcss.update(data, len);
    }

    return ctcss.toneDetected(ctcssFreqToIndex(config->rxTone));
}

void radio_enableAfOutput()
{
    if (rxBand == BND_VHF)
        gpio_setPin(AF_BAND);
    else
        gpio_clearPin(AF_BAND);

    gpio_setPin(FM_MUTE);
}

void radio_disableAfOutput()
{
    gpio_clearPin(FM_MUTE);
}

void radio_enableRx()
{
    if ((rf == NULL) || (rxBand == BND_NONE))
        return;

    C6000.setModOffset(calData.band[rxBand].refTune);
    rf->rxPowerUp();

    if ((config->opMode == OPMODE_FM) && (config->rxToneEn == true))
        stm32_adc_audio_driver.start(STM32_ADC_ADC3, (void *)ADC_CTCSS_CH,
                                     &ctcssCtx);

    radioStatus = RX;
}

/**
 * Scale one value of the DMR modulation pair to the M17 deviation.
 *
 * @param dmr: calibration value, for DMR_CAL_DEV_HZ.
 * @return the value for M17_DEV_HZ.
 */
static uint8_t m17Mod(const uint32_t dmr)
{
    const uint32_t v = (dmr * M17_DEV_HZ + DMR_CAL_DEV_HZ / 2) / DMR_CAL_DEV_HZ;
    return (v > 255) ? 255 : v;
}

static uint8_t *m17Fifo;
static volatile size_t m17Head;
static volatile size_t m17Count;
static volatile bool m17Active;
static volatile bool m17Flush;
static volatile bool m17Stop;
static volatile bool m17Done;
static volatile uint32_t tsCount;
static pthread_t m17Thread;
static pthread_mutex_t m17Mutex = PTHREAD_MUTEX_INITIALIZER;

static const uint8_t m17SaveRegs[] = { 0x10, 0x40, 0x41, 0x20,
                                       0x21, 0x22, 0x45, 0x46 };
static uint8_t m17Saved[sizeof(m17SaveRegs)];

// HR_C6000 timeslot interrupt (PC0, falling edge), every 30 ms
void __attribute__((used)) EXTI0_IRQHandler()
{
    EXTI->PR = EXTI_PR_PR0;
    tsCount = tsCount + 1;
}

/**
 * Enable or disable the timeslot interrupt.
 *
 * @param on: true to enable it.
 */
static void tsInterrupt(const bool on)
{
    if (on) {
        EXTI->PR = EXTI_PR_PR0;
        EXTI->IMR |= EXTI_IMR_MR0;
        NVIC_ClearPendingIRQ(EXTI0_IRQn);
        NVIC_EnableIRQ(EXTI0_IRQn);
    } else {
        EXTI->IMR &= ~EXTI_IMR_MR0;
        NVIC_DisableIRQ(EXTI0_IRQn);
    }
}

/**
 * Wait for the next timeslot interrupt.
 *
 * @param last: interrupt count seen last, updated.
 * @return interrupts since then, 0 if none came within M17_TS_TIMEOUT_MS.
 */
static uint32_t tsWait(uint32_t *last)
{
    for (uint32_t i = 0; i < M17_TS_TIMEOUT_MS; i++) {
        const uint32_t now = tsCount;
        if (now != *last) {
            const uint32_t d = now - *last;
            *last = now;
            return d;
        }

        sleepFor(0, 1);
    }

    return 0;
}

/**
 * Take up to n queued M17 bytes.
 */
static size_t m17Pop(uint8_t *buf, const size_t n)
{
    pthread_mutex_lock(&m17Mutex);
    const size_t got = (m17Count < n) ? m17Count : n;
    for (size_t i = 0; i < got; i++)
        buf[i] = m17Fifo[(m17Head + i) % M17_FIFO_SIZE];

    m17Head = (m17Head + got) % M17_FIFO_SIZE;
    m17Count -= got;
    pthread_mutex_unlock(&m17Mutex);

    return got;
}

/**
 * M17 transmission: set the HR_C6000 up in its physical layer mode, then give
 * it one slot of queued bytes at each timeslot interrupt. Idle goes out in
 * slot 2, when the PA comes up, data from slot 3, and one idle slot after the
 * last byte before the PA goes down.
 */
static void *m17TxFunc(void *arg)
{
    (void)arg;
    uint8_t chunk[M17_SLOT_BYTES];

    for (size_t i = 0; i < sizeof(m17SaveRegs); i++)
        m17Saved[i] = C6000.readCfgRegister(m17SaveRegs[i]);

    C6000.writeCfgRegister(0x45, txM17Mod2);
    C6000.writeCfgRegister(0x46, txM17Mod1);
    C6000.writeCfgRegister(0x41, 0x00); // No slot activity during set-up
    C6000.writeCfgRegister(0x10, 0x66); // Physical layer mode
    C6000.writeCfgRegister(0x20, 0x00);
    C6000.writeCfgRegister(0x21, 0x02); // Impolite access
    C6000.writeCfgRegister(0x22, 0x86);

    memset(chunk, M17_IDLE, sizeof(chunk));
    C6000.writeRam(2, 0x00, chunk, M17_SLOT_BYTES);
    C6000.writeRam(2, 0x24, chunk, M17_SLOT_BYTES);

    tsInterrupt(true);
    uint32_t last = tsCount;
    C6000.writeCfgRegister(0x40, 0xE3); // Own timing: the slot clock starts

    uint32_t n = 0;
    uint32_t idleTail = 0;
    bool pa = false;

    while ((m17Stop == false) && (idleTail <= 1)) {
        const uint32_t d = tsWait(&last);
        if (d == 0)
            break;

        n += d;

        // Slot n + 1 is set up at interrupt n and reads 0x00 when n is odd
        const uint8_t half = (n & 1) ? 0x00 : 0x24;

        if ((n >= 2) && (pa == false)) {
            paOn();
            pa = true;
        }

        memset(chunk, M17_IDLE, sizeof(chunk));
        if ((n >= 2) && (m17Pop(chunk, M17_SLOT_BYTES) == 0) && m17Flush)
            idleTail++;

        C6000.writeRam(2, half, chunk, M17_SLOT_BYTES);
        C6000.writeCfgRegister(0x41, 0x80); // Send in the next slot
    }

    if (pa)
        paOff();

    tsInterrupt(false);
    C6000.writeCfgRegister(0x41, 0x00);
    for (size_t i = 0; i < sizeof(m17SaveRegs); i++)
        C6000.writeCfgRegister(m17SaveRegs[i], m17Saved[i]);

    m17Done = true;
    return NULL;
}

/**
 * Start the M17 transmit thread.
 */
static void m17TxStart()
{
    if (m17Fifo == NULL)
        m17Fifo = new (std::nothrow) uint8_t[M17_FIFO_SIZE];

    if (m17Fifo == NULL)
        return;

    m17Head = 0;
    m17Count = 0;
    m17Flush = false;
    m17Stop = false;
    m17Done = false;

    pthread_attr_t attr;
    struct sched_param param;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1024);
    param.sched_priority = THREAD_PRIO_HIGH;
    pthread_attr_setschedparam(&attr, &param);
    m17Active = (pthread_create(&m17Thread, &attr, m17TxFunc, NULL) == 0);
}

/**
 * Stop the M17 transmit thread: it drops the PA and restores the HR_C6000.
 */
static void m17TxStop()
{
    if (m17Active == false)
        return;

    m17Stop = true;
    pthread_join(m17Thread, NULL);
    m17Active = false;
}

void radio_sendM17Data(const uint8_t *data, const size_t len)
{
    size_t i = 0;

    while ((i < len) && m17Active && (m17Done == false)) {
        pthread_mutex_lock(&m17Mutex);
        while ((i < len) && (m17Count < M17_FIFO_SIZE)) {
            m17Fifo[(m17Head + m17Count) % M17_FIFO_SIZE] = data[i++];
            m17Count++;
        }
        pthread_mutex_unlock(&m17Mutex);

        if (i < len)
            sleepFor(0, 5);
    }
}

void radio_flushM17Data()
{
    if (m17Active == false)
        return;

    // A full queue drains in about one second
    m17Flush = true;
    for (int i = 0; (i < 400) && (m17Done == false); i++)
        sleepFor(0, 5);
}

void radio_enableTx()
{
    // FM and M17, on a board detected as V5, inside a band
    const bool m17 = (config->opMode == OPMODE_M17);
    if ((rf != &v5Ops) || (config->txDisable == 1) || (txBand == BND_NONE)
        || ((config->opMode != OPMODE_FM) && (!m17)))
        return;

    // Not on a supply out of range: there is no other PA protection
    const uint16_t supply = platform_getVbat();
    if ((supply < TX_MIN_SUPPLY_MV) || (supply > TX_MAX_SUPPLY_MV))
        return;

    txRetune();

    C6000.setModOffset(calData.band[txBand].refTune);
    C6000.setModAmplitude(txModI, txModQ);
    C6000.setModFactor(txDeviation);

    if (m17) {
        // The transmit thread keys the PA in step with the HR_C6000's slots
        gpio_clearPin(FM_SW);
        sleepFor(0, TX_SETTLE_MS);
        m17TxStart();
        radioStatus = TX;
        return;
    } else {
        // Sub-audio on PC7 at the calibrated level, as the stock firmware
        if (config->txToneEn) {
            toneGen_setToneFreq(config->txTone / 10.0f);
            toneGen_setToneLevel(txCtcssLevel);
        }

        // Tone burst from the HR_C6000, as on the CS7000
        if (config->toneEn)
            C6000.sendTone(TONE_BURST_FREQ, TONE_DEVIATION);
        else
            C6000.disableTones();

        C6000.startAnalogTx(TxAudioSource::MIC, FmConfig::PREEMPH_EN);
    }

    gpio_clearPin(FM_SW);

    sleepFor(0, TX_SETTLE_MS);
    paOn();
    radioStatus = TX;
}

void radio_disableRtx()
{
    if (m17Active) {
        m17TxStop();
        gpio_setPin(FM_SW);
        radioStatus = OFF;
    }

    if (radioStatus == TX) {
        paOff();
        C6000.stopAnalogTx();
        toneGen_setToneLevel(0);
        gpio_setPin(FM_SW);
    }

    gpio_clearPin(FM_MUTE);
    if (rf != NULL)
        rf->rxOff();
    gpio_clearPin(RF_R5_UHF);
    gpio_clearPin(RF_R5_VHF);
    gpio_clearPin(RF_C5_UHF);
    gpio_clearPin(RF_C5_VHF);
    gpio_clearPin(VCO_SEL_UHF);
    gpio_clearPin(VCO_SEL_VHF);
    gpio_clearPin(RF_T5_UHF);
    gpio_clearPin(RF_T5_VHF);
    gpio_clearPin(RF_APC_EN);

    DAC->DHR12R1 = 0;
    DAC->DHR12R2 = 0;

    // Stop CTCSS sampling and reset the detector
    if (ctcssCtx.running) {
        stm32_adc_audio_driver.terminate(&ctcssCtx);
        ctcss.reset();
    }

    radioStatus = OFF;
}

void radio_updateConfiguration()
{
    // Transmission, applied at the next key-up
    txBand = bandOf(config->txFrequency);
    if (txBand != BND_NONE) {
        const struct md9600BandCalib *cal = &calData.band[txBand];
        const freq_t freq = config->txFrequency;
        const uint8_t points = calPoints(txBand);
        const uint8_t level = paLevel(config->txPower);
        const bool wide = (config->bandwidth != BW_12_5);

        txDrive = interpCalParameter(freq, cal->rxFreq, cal->paDrive[level],
                                     points);
        txModI = interpCalParameter(freq, cal->rxFreq, cal->fmModI, points);
        txModQ = interpCalParameter(freq, cal->rxFreq, cal->fmModQ, points);
        txDeviation = TX_DEVIATION[txBand][wide];
        txCtcssLevel = ctcssLevel(cal, freq, points, config->txTone, wide);

        const uint32_t mod2 = interpCalParameter(freq, cal->rxFreq,
                                                 cal->dmrModI, points);
        const uint32_t mod1 = interpCalParameter(freq, cal->rxFreq,
                                                 cal->dmrModQ, points);
        txM17Mod2 = m17Mod(mod2);
        txM17Mod1 = m17Mod(mod1);
    }

    rxBand = bandOf(config->rxFrequency);
    if (rxBand == BND_NONE)
        return;

    const struct md9600BandCalib *cal = &calData.band[rxBand];
    rxTune = interpCalParameter(config->rxFrequency, cal->rxFreq, cal->rxTune,
                                calPoints(rxBand));

    if (radioStatus == RX)
        radio_enableRx();
}

rssi_t radio_getRssi()
{
    if (rf == NULL)
        return RSSI_NO_SIGNAL;

    uint32_t channel = ADC_RSSI_UHF_CH;
    enum Band band = BND_UHF;
    if (rxBand == BND_VHF) {
        channel = ADC_RSSI_VHF_CH;
        band = BND_VHF;
    }

    // The RSSI line, 8 bits
    return rf->rssi(band, adc_getRawSample(&adc1, channel) >> 4);
}

enum opstatus radio_getStatus()
{
    return radioStatus;
}
