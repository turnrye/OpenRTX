/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/delays.h"
#include "interfaces/nvmem.h"
#include "interfaces/radio.h"
#include "peripherals/gpio.h"
#include "calibration/calibInfo_MDx.h"
#include "hwconfig.h"
#include <algorithm>
#include "core/utils.h"
#include "radioUtils.h"
#include "drivers/baseband/HR_C6000.h"
#include "drivers/baseband/AT1846S.h"
#include "rtx/m17_bench.h"
#include <atomic>
#ifdef CONFIG_M17_CHIP_MODEM
#include "drivers/audio/C6000_modem.h"
#endif

/*
 * M17 through the HR_C6000's modem: outer deviation of M17 and of DMR, for
 * which the modulation calibration is set. Measured on an MD-UV390 by
 * sweeping the modulation at 445.975 MHz: the outer deviation is linear in it
 * and reaches 2400 Hz at 1.21 times the DMR value; EVM is flat (3.4-4 %) up
 * to about 2700 Hz. Mod2 changed nothing and stays 0, as for DMR.
 */
static constexpr uint32_t M17_DEV_HZ = 2400;
static constexpr uint32_t DMR_DEV_HZ = 1944;
#ifdef CONFIG_CAT_DEBUG
#include "core/cat/debug.hpp"
#include <array>
#include <cerrno>
#endif


static const rtxStatus_t *config;                // Pointer to data structure with radio configuration

static mduv3x0Calib_t calData;                   // Calibration data
static Band    currRxBand = BND_NONE;            // Current band for RX
static Band    currTxBand = BND_NONE;            // Current band for TX
static uint8_t txpwr_lo   = 0;                   // APC voltage for TX output power control, low power
static uint8_t txpwr_hi   = 0;                   // APC voltage for TX output power control, high power
static uint8_t rxModBias  = 0;                   // VCXO bias for RX
static uint8_t txModBias  = 0;                   // VCXO bias for TX

static enum opstatus radioStatus;                // Current operating status

HR_C6000 C6000((const struct spiDevice *) &c6000_spi, { DMR_CS }); // HR_C6000 driver
static AT1846S& at1846s = AT1846S::instance();   // AT1846S driver

// TEST ONLY: bench console requests, applied on the rtx thread (in
// radio_getRssi()) so that the AT1846S is never driven from two threads
static std::atomic< int >  benchBwReq{0};
static std::atomic< bool > benchRinitReq{false};

extern "C" int m17bench_bw(int khz)
{
    benchBwReq.store(khz);
    return 0;
}

extern "C" int m17bench_rinit()
{
    benchRinitReq.store(true);
    return 0;
}

#ifdef CONFIG_CAT_DEBUG
/*
 * CAT debug commands for the two chips, run on the RTX thread, in order with
 * this driver's own register writes. Numbers are hexadecimal.
 *
 *   ZZC6<page> <reg>;          read a HR_C6000 register of any page (1 is
 *                              auxiliary, 4 configuration):
 *                              ZZC6<page> <reg> <value>;
 *   ZZC6<page> <reg> <value>;  write it; a register above FF, up to 7FF, is
 *                              written with the extended address
 *   ZZAT<reg>;                 read an AT1846S register: ZZAT<reg> <value>;
 *   ZZAT<reg> <value>;         write it
 */

/**
 * ZZC6: HR_C6000 registers.
 */
static int catDebugC6000(const CatRequest &req, CatAnswer &answer)
{
    std::array<uint32_t, 3> arg;
    int count = cat_debugParseHex(req, arg.data(), arg.size());

    // Only writes take an extended address
    bool valid = (count >= 2) && (arg[0] <= 7)
              && (((count == 2) && (arg[1] <= 0xFF))
                  || ((count == 3) && (arg[1] <= 0x7FF) && (arg[2] <= 0xFF)));
    if (valid == false)
        return -EINVAL;

    auto page = static_cast<C6000_SpiOpModes>(arg[0]);
    uint8_t reg = arg[1];

    if (count == 3) {
        return cat_debugOnRtx([page, reg, &arg]() {
            if (arg[1] > 0xFF)
                C6000.writeRegister16(page, arg[1], arg[2]);
            else
                C6000.writeRegister(page, reg, arg[2]);

            return 0;
        });
    }

    uint8_t value;
    int ret = cat_debugOnRtx([page, reg, &value]() {
        value = C6000.readRegister(page, reg);
        return 0;
    });
    if (ret < 0)
        return ret;

    answer.append("ZZC6", 4);
    answer.appendNumber(arg[0], 1, 16);
    answer.append(' ');
    answer.appendNumber(reg, 2, 16);
    answer.append(' ');
    answer.appendNumber(value, 2, 16);
    return 0;
}

/**
 * ZZAT: AT1846S registers.
 */
static int catDebugAt1846s(const CatRequest &req, CatAnswer &answer)
{
    std::array<uint32_t, 2> arg;
    int count = cat_debugParseHex(req, arg.data(), arg.size());

    bool valid = (count >= 1) && (arg[0] <= 0xFF)
              && ((count == 1) || (arg[1] <= 0xFFFF));
    if (valid == false)
        return -EINVAL;

    uint8_t reg = arg[0];

    if (count == 2) {
        return cat_debugOnRtx([reg, &arg]() {
            at1846s.writeRegister(reg, arg[1]);
            return 0;
        });
    }

    uint16_t value;
    int ret = cat_debugOnRtx([reg, &value]() {
        value = at1846s.readRegister(reg);
        return 0;
    });
    if (ret < 0)
        return ret;

    answer.append("ZZAT", 4);
    answer.appendNumber(reg, 2, 16);
    answer.append(' ');
    answer.appendNumber(value, 4, 16);
    return 0;
}

static std::array<CatDebugCommand, 2> catDebugCommands = { {
    { { 'C', '6' }, catDebugC6000, nullptr },
    { { 'A', 'T' }, catDebugAt1846s, nullptr },
} };
#endif

void radio_init(const rtxStatus_t *rtxState)
{
    config      = rtxState;
    radioStatus = OFF;

    /*
     * Configure RTX GPIOs
     */
    gpio_setMode(VHF_LNA_EN,   OUTPUT);
    gpio_setMode(UHF_LNA_EN,   OUTPUT);
    gpio_setMode(TX_PA_EN,     OUTPUT);
    gpio_setMode(RF_APC_SW,    OUTPUT);
    gpio_setMode(PA_SEL_SW,    OUTPUT);

    gpio_clearPin(VHF_LNA_EN);
    gpio_clearPin(UHF_LNA_EN);
    gpio_clearPin(TX_PA_EN);
    gpio_clearPin(RF_APC_SW);
    gpio_clearPin(PA_SEL_SW);

    // TODO: keep audio connected to HR_C6000, for volume control
#ifndef PLATFORM_DM1701
    gpio_setMode(RX_AUDIO_MUX, OUTPUT);
    gpio_setPin(RX_AUDIO_MUX);
#endif

    /*
     * Configure and enable DAC
     */
    gpio_setMode(APC_REF, ANALOG);

    RCC->APB1ENR |= RCC_APB1ENR_DACEN;
    DAC->CR = DAC_CR_EN1;
    DAC->DHR12R1 = 0;

    /*
     * Load calibration data
     */
    nvm_readCalibData(&calData);

    /*
     * Initialize AT1846S keep AF output disabled at power on.
     * HR_C6000 is initialized in advance by the audio system.
     */
    at1846s.init();
    radio_disableAfOutput();

#ifdef CONFIG_CAT_DEBUG
    for (auto &cmd : catDebugCommands)
        cat_debugRegister(cmd);
#endif
}

// TEST ONLY: both RF chips initialised again, as at power on, and the radio
// put back in its mode, for the bench console
static void benchRinit()
{
    radio_disableRtx();
    C6000.terminate();
    at1846s.terminate();
    C6000.init();
    C6000.benchForgetWrites();
    at1846s.init();
    radio_disableAfOutput();
    radio_setOpmode(static_cast< enum opmode >(config->opMode));
    radio_updateConfiguration();
    radio_enableRx();
}

void radio_terminate()
{
    radio_disableRtx();
    C6000.terminate();
    at1846s.terminate();

    DAC->DHR12R1 = 0;
    RCC->APB1ENR &= ~RCC_APB1ENR_DACEN;
}

void radio_setOpmode(const enum opmode mode)
{
    switch(mode)
    {
        case OPMODE_FM:
            at1846s.setOpMode(AT1846S_OpMode::FM);  // AT1846S in FM mode
            C6000.fmMode();                         // HR_C6000 in FM mode
            C6000.setInputGain(-3);                 // Input gain in dB, as per TYT firmware
            break;

        case OPMODE_DMR:
            at1846s.setOpMode(AT1846S_OpMode::DMR);
            at1846s.setBandwidth(AT1846S_BW::_12P5);
//             C6000.dmrMode();
            break;

        case OPMODE_M17:
            at1846s.setOpMode(AT1846S_OpMode::DMR); // AT1846S in DMR mode, disables RX filter
            at1846s.setBandwidth(AT1846S_BW::_25);  // Set bandwidth to 25kHz for proper deviation
            C6000.fmMode();                         // HR_C6000 in FM mode
            #if defined(PLATFORM_DM1701)
            C6000.setInputGain(-6);                 // Input gain in dB, found experimentally
            C6000.writeCfgRegister(0x35, 0x12);     // FM deviation coefficient
            #else
            C6000.setInputGain(+6);                 // Input gain in dB, found experimentally
            #endif
            break;

        default:
            break;
    }
}

bool radio_checkRxDigitalSquelch()
{
    return at1846s.rxCtcssDetected();
}

void radio_enableAfOutput()
{
    // Undocumented register, bits [1:0] seem to enable/disable FM audio RX.
    // TODO: AF output management for DMR mode
    // 0xFD enable FM receive.
    C6000.writeCfgRegister(0x26, 0xFD);
}

void radio_disableAfOutput()
{
    // Undocumented register, disable FM receive
    C6000.writeCfgRegister(0x26, 0xFE);
}

void radio_enableRx()
{
    gpio_clearPin(TX_PA_EN);
    gpio_clearPin(RF_APC_SW);
    gpio_clearPin(VHF_LNA_EN);
    gpio_clearPin(UHF_LNA_EN);
    DAC->DHR12R1 = 0;

    if(currRxBand == BND_NONE) return;

    C6000.setModOffset(rxModBias);
    at1846s.setFrequency(config->rxFrequency);
    at1846s.setFuncMode(AT1846S_FuncMode::RX);

    if(currRxBand == BND_VHF)
    {
        gpio_setPin(VHF_LNA_EN);
    }
    else
    {
        gpio_setPin(UHF_LNA_EN);
    }

    if(config->rxToneEn)
    {
        at1846s.enableRxCtcss(config->rxTone);
    }

    radioStatus = RX;
}

void radio_enableTx()
{
    if(config->txDisable == 1) return;

    // TEST ONLY: modulation pair from the bench console
    uint8_t benchMod1, benchMod2;
    if(m17bench_mod(&benchMod1, &benchMod2))
        C6000.setModAmplitude(benchMod2, benchMod1);

    gpio_clearPin(VHF_LNA_EN);
    gpio_clearPin(UHF_LNA_EN);
    gpio_clearPin(TX_PA_EN);
    gpio_clearPin(RF_APC_SW);

    C6000.setModOffset(txModBias);
    at1846s.setFrequency(config->txFrequency);

    // Constrain output power between 1W and 5W.
    float power  = static_cast < float >(config->txPower) / 1000.0f;
          power  = std::max(std::min(power, 5.0f), 1.0f);
    float pwrHi  = static_cast< float >(txpwr_hi);
    float pwrLo  = static_cast< float >(txpwr_lo);
    float apc    = pwrLo + (pwrHi - pwrLo)/4.0f*(power - 1.0f);
    DAC->DHR12L1 = static_cast< uint8_t >(apc) * 0xFF;

    switch(config->opMode)
    {
        case OPMODE_FM:
        {
            FmConfig cfg = (config->bandwidth == BW_12_5) ? FmConfig::BW_12p5kHz
                                                          : FmConfig::BW_25kHz;
            C6000.startAnalogTx(TxAudioSource::MIC, cfg | FmConfig::PREEMPH_EN);
        }
            break;

        case OPMODE_M17:
            // Baseband from the PWM, unless the chip's own modem sends M17
            if(config->m17Chip == 0)
                C6000.startAnalogTx(TxAudioSource::LINE_IN,
                                    FmConfig::BW_25kHz);
            break;

        default:
            break;
    }

    //
    // FIXME: workaround to fix a small carrier-only gap which appears at the
    // beginning of each transmission. This problem is particularly evident in
    // M17 mode because it causes the truncation of the preamble sequence.
    //
    sleepFor(0, 50);

    at1846s.setFuncMode(AT1846S_FuncMode::TX);


    if(currTxBand == BND_VHF)
    {
        gpio_clearPin(PA_SEL_SW);
    }
    else
    {
        gpio_setPin(PA_SEL_SW);
    }

    gpio_setPin(TX_PA_EN);
    gpio_setPin(RF_APC_SW);

    if(config->txToneEn)
    {
        at1846s.enableTxCtcss(config->txTone);
    }

    if (config->toneEn)
    {
        at1846s.enableTone(17500);
    }

    radioStatus = TX;
}

void radio_disableRtx()
{
    gpio_clearPin(VHF_LNA_EN);
    gpio_clearPin(UHF_LNA_EN);
    gpio_clearPin(TX_PA_EN);
    gpio_clearPin(RF_APC_SW);
    DAC->DHR12L1 = 0;

    // If we are currently transmitting, stop tone and C6000 TX
    if(radioStatus == TX)
    {
        C6000.stopAnalogTx();
    }

    at1846s.disableTone();
    at1846s.disableCtcss();
    at1846s.setFuncMode(AT1846S_FuncMode::OFF);
    radioStatus = OFF;
}

void radio_updateConfiguration()
{
    currRxBand = getBandFromFrequency(config->rxFrequency);
    currTxBand = getBandFromFrequency(config->txFrequency);

    if((currRxBand == BND_NONE) || (currTxBand == BND_NONE)) return;

    /*
     * VCXO bias voltage, separated values for TX and RX to allow for cross-band
     * operation.
     */
    txModBias = calData.vhfCal.freqAdjustMid;
    rxModBias = calData.vhfCal.freqAdjustMid;
    if(currRxBand == BND_UHF) rxModBias = calData.uhfCal.freqAdjustMid;
    if(currTxBand == BND_UHF) txModBias = calData.uhfCal.freqAdjustMid;

    uint8_t calPoints    = 5;
    freq_t  *txCalPoints = calData.vhfCal.txFreq;
    uint8_t *loPwrCal    = calData.vhfCal.txLowPower;
    uint8_t *hiPwrCal    = calData.vhfCal.txHighPower;
    uint8_t *qRangeCal   = (config->opMode == OPMODE_FM)
                         ? calData.vhfCal.analogSendQrange
                         : calData.vhfCal.sendQrange;

    if(currTxBand == BND_UHF)
    {
        calPoints   = 9;
        txCalPoints = calData.uhfCal.txFreq;
        loPwrCal    = calData.uhfCal.txLowPower;
        hiPwrCal    = calData.uhfCal.txHighPower;
        qRangeCal   = (config->opMode == OPMODE_FM)
                    ? calData.uhfCal.analogSendQrange
                    : calData.uhfCal.sendQrange;
    }

    // APC voltage for TX output power control
    txpwr_lo = interpCalParameter(config->txFrequency, txCalPoints, loPwrCal,
                                                                    calPoints);
    txpwr_hi = interpCalParameter(config->txFrequency, txCalPoints, hiPwrCal,
                                                                    calPoints);

    // HR_C6000 modulation amplitude
    uint8_t Q = interpCalParameter(config->txFrequency, txCalPoints, qRangeCal,
                                                                     calPoints);

    // The chip's modem sends M17 symbols as DMR ones: scale the DMR
    // calibration from DMR's outer deviation to M17's
    if((config->opMode == OPMODE_M17) && (config->m17Chip != 0))
        Q = std::min< uint32_t >((Q * M17_DEV_HZ + DMR_DEV_HZ / 2) / DMR_DEV_HZ,
                                 UINT8_MAX);

    C6000.setModAmplitude(0, Q);
    m17bench_setCal(Q, 0);      // TEST ONLY: for the bench console
    #ifdef CONFIG_M17_CHIP_MODEM
    C6000modem_useChip((config->opMode == OPMODE_M17) && (config->m17Chip != 0));
    #endif

    // Set bandwidth, only for analog FM mode
    if(config->opMode == OPMODE_FM)
    {
        switch(config->bandwidth)
        {
            case BW_12_5:
                at1846s.setBandwidth(AT1846S_BW::_12P5);
                break;

             case BW_25:
                at1846s.setBandwidth(AT1846S_BW::_25);
                break;

             default:
                 break;
        }
    }

    /*
     * Update VCO frequency and tuning parameters if current operating status
     * is different from OFF.
     * This is done by calling again the corresponding functions, which is safe
     * to do and avoids code duplication.
     */
    if(radioStatus == RX) radio_enableRx();
    if(radioStatus == TX) radio_enableTx();
}

rssi_t radio_getRssi()
{
    // TEST ONLY: bench console requests, on this (the rtx) thread
    const int bw = benchBwReq.exchange(0);
    if(bw != 0)
        at1846s.setBandwidth((bw == 12) ? AT1846S_BW::_12P5 : AT1846S_BW::_25);
    if(benchRinitReq.exchange(false))
        benchRinit();

    return static_cast< rssi_t >(at1846s.readRSSI());
}

enum opstatus radio_getStatus()
{
    return radioStatus;
}
