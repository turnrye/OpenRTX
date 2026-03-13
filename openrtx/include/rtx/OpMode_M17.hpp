/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPMODE_M17_H
#define OPMODE_M17_H

#include "protocols/M17/M17FrameDecoder.hpp"
#include "protocols/M17/M17FrameEncoder.hpp"
#include "protocols/M17/M17Demodulator.hpp"
#include "protocols/M17/M17Modulator.hpp"
#include "protocols/M17/M17LinkSetupFrame.hpp"
#include "protocols/M17/MetaText.hpp"
#include "core/audio_path.h"
#include "OpMode.hpp"
#include <vector>

/**
 * Specialisation of the OpMode class for the management of M17 operating mode.
 */
class OpMode_M17 : public OpMode
{
public:

    /**
     * Constructor.
     */
    OpMode_M17();

    /**
     * Destructor.
     */
    ~OpMode_M17();

    /**
     * Enable the operating mode.
     *
     * Application must ensure this function is being called when entering the
     * new operating mode and always before the first call of "update".
     */
    virtual void enable() override;

    /**
     * Disable the operating mode. This function stops the DMA transfers
     * between the baseband, microphone and speakers. It also ensures that
     * the radio, the audio amplifier and the microphone are in OFF state.
     *
     * Application must ensure this function is being called when exiting the
     * current operating mode.
     */
    virtual void disable() override;

    /**
     * Update the internal FSM.
     * Application code has to call this function periodically, to ensure proper
     * functionality.
     *
     * @param status: pointer to the rtxStatus_t structure containing the current
     * RTX status. Internal FSM may change the current value of the opStatus flag.
     * @param newCfg: flag used inform the internal FSM that a new RTX configuration
     * has been applied.
     */
    virtual void update(rtxStatus_t *const status, const bool newCfg) override;

    /**
     * Get the mode identifier corresponding to the OpMode class.
     *
     * @return the corresponding flag from the opmode enum.
     */
    virtual opmode getID() override
    {
        return OPMODE_M17;
    }

    /**
     * Check if RX squelch is open.
     *
     * @return true if RX squelch is open.
     */
    virtual bool rxSquelchOpen() override
    {
        return dataValid;
    }

    /**
     * Return selected SMS message from queue if any.
     *
     * @param mesg_num: message index to retrieve.
     * @param sender: buffer to receive the sender callsign.
     * @param message: buffer to receive the message text.
     * @return true if a message was returned.
     */
    virtual bool getSMSMessage(uint8_t mesg_num, char *sender, char *message) override;

    /**
     * Delete an SMS message from the queue.
     *
     * @param mesg_num: message index to delete.
     */
    virtual void delSMSMessage(uint8_t mesg_num) override;

private:

    /**
     * Function handling the OFF operating state.
     *
     * @param status: pointer to the rtxStatus_t structure containing the
     * current RTX status.
     */
    void offState(rtxStatus_t *const status);

    /**
     * Function handling the RX operating state.
     *
     * @param status: pointer to the rtxStatus_t structure containing the
     * current RTX status.
     */
    void rxState(rtxStatus_t *const status);

    /**
     * Function handling the TX operating state.
     *
     * @param status: pointer to the rtxStatus_t structure containing the
     * current RTX status.
     */
    void txState(rtxStatus_t *const status);

    /**
     * Function handling packet TX (SMS send).
     *
     * @param status: pointer to the rtxStatus_t structure containing the
     * current RTX status.
     */
    void txPacketState(rtxStatus_t *const status);

    /**
     * Compare two callsigns in plain text form.
     * The comparison does not take into account the country prefixes (strips
     * the '/' and whatever is in front from all callsigns). It does take into
     * account the dash and whatever is after it. In case the incoming callsign
     * is "ALL" the function returns true.
     *
     * \param localCs plain text callsign from the user
     * \param incomingCs plain text destination callsign
     * \return true if local an incoming callsigns match.
     */
    bool compareCallsigns(const std::string& localCs, const std::string& incomingCs);

    // GPS update interval in superframes. Each superframe is 6 LICH frames
    // (~240 ms), so 25 superframes ≈ 6 seconds.
    static constexpr uint16_t GPS_UPDATE_TICKS = 25;

    bool startRx;                      ///< Flag for RX management.
    bool startTx;                      ///< Flag for TX management.
    bool locked;                       ///< Demodulator locked on data stream.
    bool dataValid;                    ///< Demodulated data is valid
    bool extendedCall;                 ///< Extended callsign data received
    bool invertTxPhase;                ///< TX signal phase inversion setting.
    bool invertRxPhase;                ///< RX signal phase inversion setting.
    pathId rxAudioPath;                ///< Audio path ID for RX
    pathId txAudioPath;                ///< Audio path ID for TX
    M17::M17Modulator    modulator;    ///< M17 modulator.
    M17::M17Demodulator  demodulator;  ///< M17 demodulator.
    M17::M17FrameDecoder decoder;      ///< M17 frame decoder
    M17::M17FrameEncoder encoder;      ///< M17 frame encoder
    uint16_t gpsTimer;                 ///< GPS data transmission interval timer
    M17::MetaText metaText;            ///< M17 metatext accumulator
    bool     smsEnabled;               ///< SMS enabled
    bool     smsStarted;               ///< SMS message started flag
    int8_t   smsLastFrame;             ///< SMS frame counter
    uint16_t lastCRC;                  ///< CRC for last valid SMS message
    uint16_t totalSMSLength;           ///< Total characters in SMS recall buffer
    uint16_t numPacketbytes;           ///< Number of packet bytes remaining
    char     smsBuffer[822];           ///< SMS temporary buffer
    uint8_t  full_packet_data[33 * 25]; ///< Packet TX data buffer
    std::vector<char*> smsSender;      ///< SMS Sender Id buffer
    std::vector<char*> smsMessage;     ///< SMS message buffer
};

#endif /* OPMODE_M17_H */
