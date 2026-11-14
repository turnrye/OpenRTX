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
 * M17, at the stock firmware's PA drives.
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
#include "drivers/audio/stm32_adc.h"
#include "drivers/SPI/spi_bitbang.h"
#include "drivers/baseband/AK2365A.h"
#include "drivers/baseband/HR_C6000.h"
#include "drivers/baseband/LMX2571.h"
#include "drivers/baseband/SKY72310.h"
#include "drivers/baseband/radioUtils.h"
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
 * HR_C6000 tone generator deviations for CTCSS and the 1750 Hz tone, the
 * CS7000's values: to be checked off air on the MD-9600.
 */
static constexpr uint8_t CTCSS_DEVIATION = 0x20;
static constexpr uint8_t TONE_DEVIATION = 0x1E;
static constexpr uint32_t TONE_BURST_FREQ = 1750;

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
 * Modulation factor for M17's baseband on PC8, by band (VHF, UHF): the stock
 * firmware's wide setting for audio it sends on PC8 (DTMF, 0x08046f44). To be
 * set by measuring the M17 deviation.
 */
static constexpr uint8_t M17_TX_DEVIATION[2] = { 0x5A, 0x22 };

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
        // Baseband as PWM on PC8 into the HR_C6000, as stock sends DTMF
        C6000.disableTones();
        C6000.startAnalogTx(TxAudioSource::LINE_IN, FmConfig::BW_25kHz);
    } else {
        // Sub-audio and tone burst from the HR_C6000, as on the CS7000
        if (config->txToneEn)
            C6000.setTxCtcss(config->txTone, CTCSS_DEVIATION);
        else if (config->toneEn)
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
    if (radioStatus == TX) {
        paOff();
        C6000.stopAnalogTx();
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
        if (config->opMode == OPMODE_M17)
            txDeviation = M17_TX_DEVIATION[txBand];
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
