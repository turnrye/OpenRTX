/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/platform.h"
#include "interfaces/delays.h"
#include "interfaces/audio.h"
#include "interfaces/radio.h"
#include "protocols/M17/M17Datatypes.hpp"
#include "protocols/M17/M17PacketFrame.hpp"
#include "rtx/OpMode_M17.hpp"
#include "core/audio_codec.h"
#include <errno.h>
#include "core/gps.h"
#include "core/crc.h"
#include "core/state.h"
#include "core/utils.h"
#include "rtx/rtx.h"
#include <cstdlib>
#include <cstring>

#ifdef PLATFORM_MOD17
#include "calibration/calibInfo_Mod17.h"
#include "interfaces/platform.h"

extern mod17Calib_t mod17CalData;
#endif

using namespace std;
using namespace M17;

OpMode_M17::OpMode_M17() : startRx(false), startTx(false), locked(false),
                           dataValid(false), extendedCall(false),
                           invertTxPhase(false), invertRxPhase(false)
{

}

OpMode_M17::~OpMode_M17()
{
    disable();
}

void OpMode_M17::enable()
{
    codec_init();
    modulator.init();
    demodulator.init();
    smsSender.clear();
    smsMessage.clear();
    locked                 = false;
    dataValid              = false;
    extendedCall           = false;
    startRx                = true;
    startTx                = false;
    smsEnabled             = true;
    totalSMSLength         = 0;
    lastCRC                = 0;
    smsStarted             = false;
    smsLastFrame           = 0;
    state.totalSMSMessages = 0;
    state.havePacketData   = false;
}

void OpMode_M17::disable()
{
    startRx = false;
    startTx = false;
    platform_ledOff(GREEN);
    platform_ledOff(RED);
    audioPath_release(rxAudioPath);
    audioPath_release(txAudioPath);
    codec_terminate();
    radio_disableRtx();
    modulator.terminate();
    demodulator.terminate();
    for(size_t i = 0; i < state.totalSMSMessages; i++)
    {
        free(smsSender[i]);
        free(smsMessage[i]);
    }
    smsSender.clear();
    smsMessage.clear();
}

bool OpMode_M17::getSMSMessage(uint8_t mesg_num, char *sender, char *message)
{
    if(state.totalSMSMessages == 0 || mesg_num > (state.totalSMSMessages - 1))
        return false;
    strcpy(sender, smsSender[mesg_num]);
    strcpy(message, smsMessage[mesg_num]);
    return true;
}

void OpMode_M17::delSMSMessage(uint8_t mesg_num)
{
    free(smsSender[mesg_num]);
    free(smsMessage[mesg_num]);
    smsSender.erase(smsSender.begin() + mesg_num);
    smsMessage.erase(smsMessage.begin() + mesg_num);
}

void OpMode_M17::update(rtxStatus_t *const status, const bool newCfg)
{
    (void) newCfg;
    #if defined(PLATFORM_MD3x0) || defined(PLATFORM_MDUV3x0)
    //
    // Invert TX phase for all MDx models.
    // Invert RX phase for MD-3x0 VHF and MD-UV3x0 radios.
    //
    const hwInfo_t* hwinfo = platform_getHwInfo();
    invertTxPhase = true;
    if(hwinfo->vhf_band == 1)
        invertRxPhase = true;
    else
        invertRxPhase = false;
    #elif defined(PLATFORM_MOD17)
    //
    // Get phase inversion settings from calibration.
    //
    invertTxPhase = (mod17CalData.bb_tx_invert == 1) ? true : false;
    invertRxPhase = (mod17CalData.bb_rx_invert == 1) ? true : false;
    #elif defined(PLATFORM_CS7000) || defined(PLATFORM_CS7000P)
    invertTxPhase = true;
    #elif defined(PLATFORM_DM1701)
    invertTxPhase = true;
    invertRxPhase = true;
    #endif

    // Main FSM logic
    switch(status->opStatus)
    {
        case OFF:
            offState(status);
            break;

        case RX:
            rxState(status);
            break;

        case TX:
            if(state.havePacketData)
                txPacketState(status);
            else
                txState(status);
            break;

        default:
            break;
    }

    // Led control logic
    switch(status->opStatus)
    {
        case RX:

            if(dataValid)
                platform_ledOn(GREEN);
            else
                platform_ledOff(GREEN);
            break;

        case TX:
            platform_ledOff(GREEN);
            platform_ledOn(RED);
            break;

        default:
            platform_ledOff(GREEN);
            platform_ledOff(RED);
            break;
    }
}

void OpMode_M17::offState(rtxStatus_t *const status)
{
    radio_disableRtx();

    codec_stop(txAudioPath);
    audioPath_release(txAudioPath);

    if(startRx)
    {
        status->opStatus = RX;
        return;
    }

    if(platform_getPttStatus() && (status->txDisable == 0))
    {
        startTx = true;
        status->opStatus = TX;
        return;
    }

    if(state.havePacketData)
    {
        startTx = true;
        status->opStatus = TX;
        status->txDisable = 0;
        return;
    }

    // Sleep for 30ms if there is nothing else to do in order to prevent the
    // rtx thread looping endlessly and locking up all the other tasks
    sleepFor(0, 30);
}

void OpMode_M17::rxState(rtxStatus_t *const status)
{
    if(startRx)
    {
        demodulator.startBasebandSampling();

        radio_enableRx();

        startRx = false;
    }

    bool newData = demodulator.update(invertRxPhase);
    bool lock    = demodulator.isLocked();

    // Reset frame decoder when transitioning from unlocked to locked state.
    if((lock == true) && (locked == false))
    {
        decoder.reset();
        locked = lock;
    }

    if(locked)
    {
        // Process new data
        if(newData)
        {
            auto& frame   = demodulator.getFrame();
            auto  type    = decoder.decodeFrame(frame);
            auto  lsf     = decoder.getLsf();
            status->lsfOk = lsf.valid();

            if(status->lsfOk)
            {
                dataValid = true;

                // Retrieve stream source and destination data
                Callsign dst = lsf.getDestination();
                Callsign src = lsf.getSource();
                strncpy(status->M17_dst, dst, 10);
                
                // Copy source callsign (may be overridden for extended callsigns)
                strncpy(status->M17_src, src, 10);

                // Retrieve extended callsign data
                streamType_t streamType = lsf.getType();

                if(streamType.fields.encType == M17_ENCRYPTION_NONE)
                {
                    meta_t& meta = lsf.metadata();

                    switch(streamType.fields.encSubType)
                    {
                        case M17_META_EXTD_CALLSIGN:
                        {
                            extendedCall = true;
                            Callsign exCall1(meta.extended_call_sign.call1);
                            Callsign exCall2(meta.extended_call_sign.call2);

                            // The source callsign only contains the last link when
                            // receiving extended callsign data: store the first
                            // extended callsign in M17_src.
                            strncpy(status->M17_src,  exCall1, 10);
                            strncpy(status->M17_refl, exCall2, 10);
                            strncpy(status->M17_link, src, 10);
                            break;
                        }
                        case M17_META_TEXT:
                        {
                            metaText.addBlock(meta);
                            const char* txt = metaText.getText();
                            if(txt != nullptr)
                                strncpy(status->M17_meta_text, txt, sizeof(status->M17_meta_text) - 1);
                            break;
                        }
                        default:
                            // M17_src already set above
                            break;
                    }
                }
                // M17_src already set above for non-encrypted streams

                // Check CAN on RX, if enabled.
                // If check is disabled, force match to true.
                bool canMatch =  (streamType.fields.CAN == status->can)
                              || (status->canRxEn == false);

                // Check if the destination callsign of the incoming transmission
                // matches with ours
                bool callMatch = (Callsign(status->source_address) == dst)
                               || dst.isSpecial();

                // Open audio path only if CAN and callsign match
                uint8_t pthSts = audioPath_getStatus(rxAudioPath);
                if((pthSts == PATH_CLOSED) && (canMatch == true) && (callMatch == true))
                {
                    rxAudioPath = audioPath_request(SOURCE_MCU, SINK_SPK, PRIO_RX);
                    pthSts = audioPath_getStatus(rxAudioPath);
                }

                // Extract audio data and sent it to codec
                if((type == M17FrameType::STREAM) && (pthSts == PATH_OPEN))
                {
                    // (re)start codec2 module if not already up
                    if(codec_running() == false)
                        codec_startDecode(rxAudioPath);

                    M17StreamFrame sf = decoder.getStreamFrame();
                    codec_pushFrame(sf.payload().data(),     false);
                    codec_pushFrame(sf.payload().data() + 8, false);
                }
                // Check if packet SMS message and SMS receive enabled
                else if(type == M17FrameType::PACKET && smsEnabled &&
                        (canMatch == true) &&
                        ((callMatch == true) || !state.settings.m17_sms_match_call))
                {
                    M17PacketFrame pf = decoder.getPacketFrame();

                    if(!smsStarted && pf.payload()[0] == 0x05)
                    {
                        if(state.totalSMSMessages == 0)
                            lastCRC = 0;
                        if(state.totalSMSMessages == 10)
                        {
                            free(smsSender[0]);
                            free(smsMessage[0]);
                            smsSender.erase(smsSender.begin());
                            smsMessage.erase(smsMessage.begin());
                            state.totalSMSMessages--;
                        }

                        smsLastFrame = 0;
                        char *tmp = (char*)malloc(strlen(status->M17_src) + 1);
                        if(tmp != NULL)
                        {
                            memset(tmp, 0, strlen(status->M17_src) + 1);
                            memcpy(tmp, status->M17_src, strlen(status->M17_src));
                            smsSender.push_back(tmp);
                            smsStarted = true;
                            totalSMSLength = 0;
                            memset(smsBuffer, 0, 821);
                        }
                    }

                    if(smsStarted)
                    {
                        uint8_t rx_fn   = (pf.payload()[25] >> 2) & 0x1F;
                        uint8_t rx_last =  pf.payload()[25] >> 7;

                        if(rx_fn <= 31 && rx_fn == smsLastFrame && !rx_last)
                        {
                            memcpy(&smsBuffer[totalSMSLength],
                                   pf.payload().data(), 25);
                            smsLastFrame++;
                            totalSMSLength += 25;
                        }
                        else if(rx_last)
                        {
                            memcpy(&smsBuffer[totalSMSLength],
                                   pf.payload().data(),
                                   rx_fn < 25 ? rx_fn : 25);
                            totalSMSLength += rx_fn < 25 ? rx_fn : 25;

                            uint16_t packet_crc = __builtin_bswap16(
                                crc_m17(smsBuffer, totalSMSLength - 2));
                            uint16_t crc;
                            memcpy((uint8_t*)&crc,
                                   &smsBuffer[totalSMSLength - 2], 2);

                            char *tmp = (char*)malloc(totalSMSLength - 3);
                            if(tmp != NULL && crc == packet_crc && crc != lastCRC)
                            {
                                memset(tmp, 0, totalSMSLength - 3);
                                memcpy(tmp, &smsBuffer[1], totalSMSLength - 3);
                                smsMessage.push_back(tmp);
                                state.totalSMSMessages++;
                                lastCRC = crc;
                            }
                            else
                            {
                                if(tmp != NULL)
                                    free(tmp);
                                free(smsSender[state.totalSMSMessages]);
                                smsSender.pop_back();
                            }
                            smsStarted = false;
                        }
                    }
                }
            }
        }
    }

    locked = lock;

    if(platform_getPttStatus() || state.havePacketData)
    {
        demodulator.stopBasebandSampling();
        locked = false;
        status->opStatus = OFF;
    }

    // Force invalidation of LSF data as soon as lock is lost (for whatever cause)
    if(locked == false)
    {
        status->lsfOk = false;
        dataValid     = false;
        extendedCall  = false;
        smsStarted    = false;
        status->M17_meta_text[0] = '\0';
        status->M17_link[0] = '\0';
        status->M17_refl[0] = '\0';

        metaText.reset();
        codec_stop(rxAudioPath);
        audioPath_release(rxAudioPath);
    }
}

void OpMode_M17::txState(rtxStatus_t *const status)
{
    frame_t m17Frame;

    if(startTx)
    {
        startTx = false;

        M17LinkSetupFrame lsf;

        lsf.clear();
        lsf.setSource(status->source_address);

        Callsign dst(status->destination_address);
        if(!dst.isEmpty())
            lsf.setDestination(dst);

        streamType_t type;
        type.fields.dataMode = M17_DATAMODE_STREAM;     // Stream
        type.fields.dataType = M17_DATATYPE_VOICE;      // Voice data
        type.fields.CAN      = status->can;             // Channel access number

        lsf.setType(type);

        if(strlen(state.settings.M17_meta_text) > 0) {
            metaText.setText(state.settings.M17_meta_text);
            metaText.getNextBlock(lsf.metadata());
        }

        if(state.settings.gps_enabled) {
            lsf.setGnssData(&state.gps_data, M17_GNSS_STATION_HANDHELD);
            gpsTimer = 0;
        }

        encoder.reset();
        encoder.encodeLsf(lsf, m17Frame);

        txAudioPath = audioPath_request(SOURCE_MIC, SINK_MCU, PRIO_TX);
        codec_startEncode(txAudioPath);
        radio_enableTx();

        modulator.invertPhase(invertTxPhase);
        modulator.start();
        modulator.sendPreamble();
        modulator.sendFrame(m17Frame);
    }
    payload_t dataFrame;
    bool      lastFrame = false;

    // Wait until there are 16 bytes of compressed speech, then send them
    codec_popFrame(dataFrame.data(),     true);
    codec_popFrame(dataFrame.data() + 8, true);

    if(platform_getPttStatus() == false)
    {
        lastFrame = true;
        startRx   = true;
        status->opStatus = OFF;
    }

    encoder.encodeStreamFrame(dataFrame, m17Frame, lastFrame);
    modulator.sendFrame(m17Frame);

    // After encoding a stream frame the encoder advances its LICH counter.
    // When it wraps back to zero a new superframe begins and the encoder
    // will accept an updated LSF.  Schedule the next meta-text block or
    // GPS update at this boundary so the new data is transmitted during
    // the upcoming superframe.
    if(encoder.superframeBoundary())
    {
        if(strlen(state.settings.M17_meta_text) > 0) {
            auto lsf = encoder.getCurrentLsf();
            metaText.getNextBlock(lsf.metadata());
            encoder.updateLsfData(lsf);
        }

        if(state.settings.gps_enabled) {
            gpsTimer++;

            if(gpsTimer >= GPS_UPDATE_TICKS) {
                auto lsf = encoder.getCurrentLsf();
                lsf.setGnssData(&state.gps_data, M17_GNSS_STATION_HANDHELD);
                encoder.updateLsfData(lsf);
                gpsTimer = 0;
            }
        }
    }

    if(lastFrame)
    {
        encoder.encodeEotFrame(m17Frame);
        modulator.sendFrame(m17Frame);
        modulator.stop();
    }
}

void OpMode_M17::txPacketState(rtxStatus_t *const status)
{
    frame_t      m17Frame;
    pktPayload_t packetFrame;
    uint8_t      full_packet_data[33 * 25] = {0};

    if(!startRx && locked)
    {
        demodulator.stopBasebandSampling();
        locked = false;
        status->opStatus = OFF;
    }

    // Do not transmit if SMS message is empty
    if(strlen(state.sms_message) == 0)
    {
        if(platform_getPttStatus() == false)
            state.havePacketData = false;
        startRx = true;
        status->opStatus = OFF;
        return;
    }

    startTx = false;

    M17LinkSetupFrame lsf;
    lsf.clear();
    lsf.setSource(std::string(status->source_address));

    std::string dst(status->destination_address);
    if(!dst.empty())
        lsf.setDestination(dst);

    memset(full_packet_data, 0, 33 * 25);
    full_packet_data[0] = 0x05;
    memcpy(&full_packet_data[1], state.sms_message, strlen(state.sms_message));
    numPacketbytes = strlen(state.sms_message) + 2;  // 0x05 and 0x00

    uint16_t packet_crc    = __builtin_bswap16(crc_m17(full_packet_data,
                                                       numPacketbytes));
    full_packet_data[numPacketbytes]     = packet_crc & 0xFF;
    full_packet_data[numPacketbytes + 1] = packet_crc >> 8;
    numPacketbytes += 2;  // Count 2-byte CRC

    streamType_t type;
    type.fields.dataMode = M17_DATAMODE_PACKET;
    type.fields.dataType = 0;
    type.fields.CAN      = status->can;

    lsf.setType(type);
    lsf.updateCrc();

    encoder.reset();
    encoder.encodeLsf(lsf, m17Frame);

    radio_enableTx();

    modulator.invertPhase(invertTxPhase);
    modulator.start();
    modulator.sendPreamble();
    modulator.sendFrame(m17Frame);

    uint8_t cnt = 0;
    while(numPacketbytes > 25)
    {
        memcpy(packetFrame.data(), &full_packet_data[cnt * 25], 25);
        packetFrame[25] = cnt << 2;
        encoder.encodePacketFrame(packetFrame, m17Frame);
        modulator.sendFrame(m17Frame);
        cnt++;
        numPacketbytes -= 25;
    }

    memset(packetFrame.data(), 0, 26);
    memcpy(packetFrame.data(), &full_packet_data[cnt * 25], numPacketbytes);
    packetFrame[25] = 0x80 | (numPacketbytes << 2);
    encoder.encodePacketFrame(packetFrame, m17Frame);
    modulator.sendFrame(m17Frame);

    encoder.encodeEotFrame(m17Frame);
    modulator.sendFrame(m17Frame);
    modulator.stop();

    startRx = true;
    state.havePacketData = false;
    memset(state.sms_message, 0, 821);
    lastCRC = 0;
    status->txDisable = 1;
    status->opStatus = OFF;
}

bool OpMode_M17::compareCallsigns(const std::string& localCs,
                                  const std::string& incomingCs)
{
    if((incomingCs == "ALL") || (incomingCs == "INFO") || (incomingCs == "ECHO"))
        return true;

    std::string truncatedLocal(localCs);
    std::string truncatedIncoming(incomingCs);

    int slashPos = localCs.find_first_of('/');
    if(slashPos <= 2)
        truncatedLocal = localCs.substr(slashPos + 1);

    slashPos = incomingCs.find_first_of('/');
    if(slashPos <= 2)
        truncatedIncoming = incomingCs.substr(slashPos + 1);

    if(truncatedLocal == truncatedIncoming)
        return true;

    // Remove any appended spaces from callsign
    int spacePos = truncatedLocal.find_first_of(' ');
    if(spacePos >= 4)
        truncatedLocal = truncatedLocal.substr(0, spacePos);

    spacePos = truncatedIncoming.find_first_of(' ');
    if(spacePos >= 4)
        truncatedIncoming = truncatedIncoming.substr(0, spacePos);

    if(truncatedLocal == truncatedIncoming)
        return true;

    return false;
}
