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
 * Receive only: transmission is not supported yet.
 */

#include "interfaces/delays.h"
#include "interfaces/nvmem.h"
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "peripherals/adc.h"
#include "peripherals/gpio.h"
#include "calibration/calibInfo_MD9600.h"
#include "core/utils.h"
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
static enum opstatus radioStatus;
static bool rfEnabled; // Only on a board detected as V5

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
    return false;
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
    radioStatus = RX;
}

void radio_enableTx()
{
}

void radio_disableRtx()
{
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

    radioStatus = OFF;
}

void radio_updateConfiguration()
{
    rxBand = getBandFromFrequency(config->rxFrequency);
    if (rxBand == BND_NONE)
        return;

    const struct md9600BandCalib *cal = &calData.band[rxBand];
    uint8_t points = MD9600_CAL_POINTS_UHF;
    if (rxBand == BND_VHF)
        points = MD9600_CAL_POINTS_VHF;

    rxTune = interpCalParameter(config->rxFrequency, cal->rxFreq, cal->rxTune,
                                points);

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
