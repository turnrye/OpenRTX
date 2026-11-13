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
 * Transmission: FM only, at the stock firmware's PA drives, no CTCSS yet.
 * There is no lock detect and no temperature, SWR or power sensing on this
 * board.
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
#include "drivers/baseband/HR_C6000.h"
#include "drivers/baseband/LMX2571.h"
#include "drivers/baseband/radioUtils.h"
#include "hwconfig.h"

static constexpr freq_t IF_FREQ = 49950000; // VHF LO above, UHF LO below
static constexpr unsigned int PLL_RESET_MS = 10;
static constexpr unsigned int RX_SETTLE_MS = 4;
static constexpr rssi_t RSSI_LOW_DBM = -120;
static constexpr rssi_t RSSI_HIGH_DBM = -70;
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
 * Provisional, until a bench measurement: the 8-bit RSSI reading taken as
 * -120 dBm, per band (VHF, UHF). The calibration's RSSI points are in the
 * stock firmware's units, which samples the line for 15 ADC cycles and reads
 * it lower than OpenRTX does. These are the dead-air readings of one V5 unit.
 */
static constexpr int32_t RSSI_FLOOR[] = { 121, 94 };

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
static bool rfEnabled; // Only on a board detected as V5

// CTCSS detection on the sub-audio input, as on the CS7000
static int16_t __attribute__((section(".bss2"))) ctcssSamples[128];
static streamCtx ctcssCtx;
static int16_t *prevCtcssBuf;
static CtcssDetector ctcss(ctcssCoeffs2k, (CTCSS_SAMPLE_RATE / 4), 20.0f);

// Synthesizers, indexed by enum Band
static const struct lmx2571 *const pll[] = { &pllVhf, &pllUhf };

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
 * Power up and tune the receivers, in the order of the stock firmware's RX
 * entry (S126.061 at 0x08049a74): both receivers are powered, only the
 * current band is tuned and routed to the HR_C6000.
 */
static void rxPowerUp()
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

    // The RF section of other boards is different: leave it alone
    rfEnabled = (platform_getHwInfo()->flags & MD9600_FLAGS_V5) != 0;
    if (!rfEnabled)
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
    setOutputLow(IF_BW_UHF);
    setOutputLow(IF_BW_VHF);
    setOutputLow(IF_ROUTE_UHF);
    setOutputLow(IF_ROUTE_VHF);
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

    // Reset both synthesizers once, as the stock firmware does at boot
    spiBitbang_init(&pll_spi);
    LMX2571_init(&pllVhf);
    LMX2571_init(&pllUhf);
    sleepFor(0, PLL_RESET_MS);
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
    if ((!rfEnabled) || (rxBand == BND_NONE))
        return;

    C6000.setModOffset(calData.band[rxBand].refTune);
    rxPowerUp();

    if ((config->opMode == OPMODE_FM) && (config->rxToneEn == true))
        stm32_adc_audio_driver.start(STM32_ADC_ADC3, (void *)ADC_CTCSS_CH,
                                     &ctcssCtx);

    radioStatus = RX;
}

void radio_enableTx()
{
    // FM only, on a board detected as V5, inside a band
    if ((!rfEnabled) || (config->txDisable == 1) || (txBand == BND_NONE)
        || (config->opMode != OPMODE_FM))
        return;

    txRetune();

    C6000.setModOffset(calData.band[txBand].refTune);
    C6000.setModAmplitude(txModI, txModQ);
    C6000.setModFactor(txDeviation);
    C6000.startAnalogTx(TxAudioSource::MIC, FmConfig::PREEMPH_EN);
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
    gpio_clearPin(IF_ROUTE_UHF);
    gpio_clearPin(IF_ROUTE_VHF);
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
    txBand = getBandFromFrequency(config->txFrequency);
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
    }

    rxBand = getBandFromFrequency(config->rxFrequency);
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
    /*
     * The RSSI line, 8 bits, is mapped linearly from the band's floor with
     * the slope of the two calibration points at -120 and -70 dBm.
     */
    if (!rfEnabled)
        return RSSI_NO_SIGNAL;

    uint32_t channel = ADC_RSSI_UHF_CH;
    enum Band band = BND_UHF;
    if (rxBand == BND_VHF) {
        channel = ADC_RSSI_VHF_CH;
        band = BND_VHF;
    }

    const int32_t raw = adc_getRawSample(&adc1, channel) >> 4;
    const int32_t span = calData.rssiHigh - calData.rssiLow;

    return RSSI_LOW_DBM
         + ((raw - RSSI_FLOOR[band]) * (RSSI_HIGH_DBM - RSSI_LOW_DBM)) / span;
}

enum opstatus radio_getStatus()
{
    return radioStatus;
}
