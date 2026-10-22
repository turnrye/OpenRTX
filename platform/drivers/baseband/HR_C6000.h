/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HRC6000_H
#define HRC6000_H

#include "core/datatypes.h"
#include "drivers/baseband/HR_Cx000.h"

enum class C6000_SpiOpModes : uint8_t
{
    AUX    = 1,     ///< Auxiliary configuration registers.
    DATA   = 2,     ///< Write TX data register and read RX data register.
    SOUND  = 3,     ///< Voice prompt sample register.
    CONFIG = 4,     ///< Main configuration registers.
    AMBE3K = 5,     ///< AMBE3000 configuration register.
    DATA_R = 6,     ///< Write RX data register and read TX data register.
    AMBE1K = 7      ///< AMBE1000 configuration register.
};

/**
 * Timing source for the DMR slots, written to configuration register 0x40.
 * A receiver has to lock to the slot clock of the far station and therefore
 * idles in passive timing; active timing, where the chip free runs its own
 * slot clock, is only for transmitting in direct mode. OpenGD77 initialises
 * the register to 0x43 and keeps it there whenever the radio is not
 * transmitting.
 */
enum class C6000_DmrTiming : uint8_t
{
    OFF      = 0x03,    ///< Receiver and transmitter stopped.
    RX_IDLE  = 0x43,    ///< RX enabled, slots locked to the received signal.
    TRANSMIT = 0xA3,    ///< TX enabled on the chip's own timing, RX off.
    PASSIVE  = 0xC3,    ///< TX and RX enabled, slots locked to a base station.
    ACTIVE   = 0xE3     ///< TX and RX enabled, chip generates its own timing.
};

/**
 * Bits of the slot timing register which describe the receiver. The transmit
 * enable is deliberately left out: the chip only holds it while the
 * transmitter is keyed, so it does not read back after being written. The
 * timing select bit is not part of the mask either, as it encodes the chosen
 * timing mode rather than whether the receiver is running.
 */
#define C6000_TIMING_RX_MASK    0x40    ///< RX enabled.

/**
 * What the chip has to do in the upcoming slot, written to configuration
 * register 0x41 before the slot begins.
 */
enum class C6000_DmrSlotAction : uint8_t
{
    IDLE     = 0x00,    ///< Neither transmit nor receive.
    RESET    = 0x20,    ///< Sync fail, used to restart the slot state.
    NEXT     = 0x40,    ///< Receive the next slot, as the stock firmware does.
    RECEIVE  = 0x50,    ///< Receive, with the layer 2 access bit set.
    TRANSMIT = 0x80     ///< Transmit.
};

/**
 * Burst the chip builds in a transmit slot, written to configuration register
 * 0x50. A voice burst carries its frame number, 0 to 5 for A to F, in the high
 * nibble.
 */
enum class C6000_DmrBurstType : uint8_t
{
    VOICE        = 0x08,    ///< Voice frame, the frame number added.
    VOICE_HEADER = 0x10,    ///< Voice link control header.
    TERMINATOR   = 0x20     ///< Terminator with link control.
};

/**
 * Vocoder interface mode, configuration register 0x22, as the stock firmware
 * sets it around calls.
 */
enum class C6000_DmrVocoder : uint8_t
{
    OFF    = 0x16,  ///< Decode stopped, at the end of a received call.
    IDLE   = 0x46,  ///< Neither encoding nor decoding.
    ENCODE = 0x86   ///< Encoding, from the last voice header on.
};

/**
 * Voice buffers of the vocoder interface, cleared through configuration
 * register 0x21.
 */
enum class C6000_DmrBuffers : uint8_t
{
    DECODE = 0x01,  ///< The receive buffer.
    ENCODE = 0x02,  ///< The transmit buffer.
    ALL    = 0x03   ///< Both.
};

/**
 * DMR interrupt flags, registers 0x82 (raised) and 0x83 (acknowledge). The
 * stock firmware handles and acknowledges them one at a time, in the order of
 * C6000_DMR_FLAG_ORDER; only the received burst and the completed embedded
 * link control carry work in reception, the slot tick comes every slot once
 * the receiver is locked.
 */
static constexpr uint8_t C6000_DMR_FLAG_BURST       = 0x08;
static constexpr uint8_t C6000_DMR_FLAG_EMBEDDED_LC = 0x10;
static constexpr uint8_t C6000_DMR_FLAG_SLOT        = 0x20;
static constexpr uint8_t C6000_DMR_FLAG_ORDER[]     =
{
    0x80, 0x08, 0x40, 0x20, 0x10, 0x04, 0x02, 0x01
};

/**
 * Burst status, register 0x51: the sync class in bits 1..0 and, in bit 2,
 * whether the check on the burst failed; masked together they give a class
 * only for a burst that passed. A data burst has its data type in bits 7..4.
 */
static constexpr uint8_t C6000_DMR_SYNC_MASK       = 0x07;
static constexpr uint8_t C6000_DMR_CLASS_VOICE     = 0x00;  ///< Frames B to F.
static constexpr uint8_t C6000_DMR_CLASS_VOICE_SYNC = 0x01; ///< Frame A.
static constexpr uint8_t C6000_DMR_CLASS_DATA      = 0x02;  ///< Data or control.
static constexpr uint8_t C6000_DMR_DATA_VOICE_HEADER = 0x01;
static constexpr uint8_t C6000_DMR_DATA_TERMINATOR = 0x02;

/**
 * Status of the DMR receiver, sampled from the chip in one go so that the
 * bits describing a given burst are consistent with each other.
 */
struct C6000_DmrStatus
{
    uint8_t intFlags;   ///< Interrupt flags, register 0x82.
    uint8_t rxStatus;   ///< Data type, sync class and CRC result, register 0x51.
    uint8_t cach;       ///< Received color code and timecode, register 0x52.
    uint8_t syncStatus; ///< Type of the detected sync pattern, register 0x5F.
};

/**
 * Length of the voice payload of a DMR burst: three 9-byte AMBE frames.
 */
#define C6000_VOICE_PAYLOAD_LENGTH  27

/**
 * Length of the link control data the chip hands over once it has decoded and
 * error corrected a burst.
 */
static constexpr size_t C6000_LC_LENGTH = 12;

class HR_C6000 : public HR_Cx000 < C6000_SpiOpModes >
{
public:

    /**
     * Constructor.
     *
     * @param uSpi: pointer to SPI device for "user" SPI interface.
     * @param uCs: gpioPin object for "user" SPI chip select.
     */
    HR_C6000(const struct spiDevice *uSpi, const struct gpioPin uCs) :
        HR_Cx000< C6000_SpiOpModes >(uSpi, uCs) { }

    /**
     * Configure CTCSS tone transmission.
     *
     * @param tone: CTCSS tone frequency.
     * @param deviation: CTCSS tone deviation.
     */
    void setTxCtcss(const tone_t tone, const uint8_t deviation);

    /**
     * Configure CTCSS tone detection.
     *
     * @param tone: CTCSS tone frequency.
     */
    void setRxCtcss(const tone_t tone);

    /**
     * Test if RX CTCSS tone has been detected.
     *
     * @return true if RX CTCSS tone has been detected.
     */
    inline bool ctcssDetected()
    {
        uint8_t reg = readCfgRegister(0x93);
        return ((reg & 0x01) != 0) ? true : false;
    }

    /**
     * Disable all tone encode and decode.
     */
    inline void disableTones()
    {
        writeCfgRegister(0xA1, 0x00);     // Disable all tones
    }

    /**
     * Transmit a tone of a given frequency.
     *
     * @param freq: tone frequency in Hz.
     * @param deviation: tone deviation.
     */
    void sendTone(const uint32_t freq, const uint8_t deviation);

    /**
     * Set the color code the receiver accepts, the chip drops every burst
     * carrying a different one.
     *
     * @param code: color code, from 0 to 15.
     */
    inline void setDmrColorCode(const uint8_t code)
    {
        writeCfgRegister(0x1F, (code & 0x0F) << 4);
    }

    /**
     * Get the color code of the last received burst, which the chip reports
     * together with the channel access information.
     *
     * NOTE: this is what was received, not what was configured. Register 0x1F
     * holds the color code the receiver filters on but does not read back as
     * written, so there is no way to ask the chip what it was set to.
     *
     * @return the color code of the last received burst.
     */
    inline uint8_t getDmrRxColorCode()
    {
        return (readCfgRegister(0x52) >> 4) & 0x0F;
    }

    /**
     * Select the timing source for the DMR slots.
     *
     * @param timing: timing source to be used.
     */
    inline void setDmrTiming(const C6000_DmrTiming timing)
    {
        writeCfgRegister(0x40, static_cast< uint8_t >(timing));
    }

    /**
     * Get the raw content of the slot timing register.
     *
     * @return the value of the timing register.
     */
    inline uint8_t getDmrTiming()
    {
        return readCfgRegister(0x40);
    }

    /**
     * Tell the chip what to do in the upcoming slot. Has to be called before
     * the slot begins, that is from the handling of a timeslot interrupt.
     *
     * @param action: action to be performed in the next slot.
     */
    inline void setDmrSlotAction(const C6000_DmrSlotAction action)
    {
        writeCfgRegister(0x41, static_cast< uint8_t >(action));
    }

    /**
     * Tell the chip which burst to build in the upcoming transmit slot.
     *
     * @param type: burst type.
     * @param frame: voice frame number, 0 to 5, for a voice burst.
     */
    inline void setDmrBurstType(const C6000_DmrBurstType type,
                                const uint8_t frame = 0)
    {
        writeCfgRegister(0x50, static_cast< uint8_t >(type) | (frame << 4));
    }

    /**
     * Set the mode of the vocoder interface.
     *
     * @param mode: vocoder interface mode.
     */
    inline void setDmrVocoder(const C6000_DmrVocoder mode)
    {
        writeCfgRegister(0x22, static_cast< uint8_t >(mode));
    }

    /**
     * Clear voice buffers of the vocoder interface.
     *
     * @param which: buffers to clear.
     */
    inline void clearDmrBuffers(const C6000_DmrBuffers which)
    {
        writeCfgRegister(0x21, static_cast< uint8_t >(which));
    }

    /**
     * Reset configuration register 0x20, which the stock firmware writes to
     * zero ahead of clearing the voice buffers when a call ends.
     */
    inline void resetDmrVoiceControl()
    {
        writeCfgRegister(0x20, 0x00);
    }

    /**
     * Set the own DMR ID, which the chip uses when transmitting.
     *
     * @param id: 24-bit DMR ID.
     */
    inline void setDmrOwnId(const uint32_t id)
    {
        writeCfgRegister(0x14, id & 0xFF);
        writeCfgRegister(0x15, (id >> 8) & 0xFF);
        writeCfgRegister(0x16, (id >> 16) & 0xFF);
    }

    /**
     * Turn the chip's privacy (encryption) on or off; it is off for clear
     * voice.
     *
     * @param enable: true for privacy.
     */
    inline void setDmrPrivacy(const bool enable)
    {
        uint8_t reg = readCfgRegister(0x56) & ~0x10;
        writeCfgRegister(0x56, enable ? (reg | 0x10) : reg);
    }

    /**
     * Restart the receiver's search for a transmission, as the radio's stock
     * firmware does when it loses the lock or a call ends: stop reception,
     * reset the slot state machine, ask for the next slot and start reception
     * again. The order of these steps differs between the radios.
     */
    void dmrArmReceiver();

    /**
     * Put the chip's DMR mode in the setting the radio's stock firmware
     * transmits with, at key-up. The direct mode setup puts it back after the
     * transmission where the stock firmware does so.
     */
    void dmrTransmitMode();

    /**
     * Read the status and colour code of the last burst, as the slot handler
     * samples them.
     *
     * @param status: burst status, register 0x51.
     * @param colorCode: received colour code.
     */
    inline void readDmrBurst(uint8_t *status, uint8_t *colorCode)
    {
        *status    = readCfgRegister(0x51);
        *colorCode = (readCfgRegister(0x52) >> 4) & 0x0F;
    }

    /**
     * Get the raised DMR interrupt flags.
     *
     * @return register 0x82.
     */
    inline uint8_t getDmrInterrupts()
    {
        return readCfgRegister(0x82);
    }

    /**
     * Sample the status of the DMR receiver.
     *
     * @param status: destination for the status registers.
     */
    void readDmrStatus(struct C6000_DmrStatus *status);

    /**
     * Hand the transmit link control to the chip, so that it can build the
     * voice header and terminator bursts around it. The chip performs the error
     * correction and interleaving itself, so the bytes written here are the
     * plain link control.
     *
     * @param lc: link control to transmit, C6000_LC_LENGTH bytes long.
     */
    void writeDmrLinkControl(const uint8_t *lc);

    /**
     * Acknowledge the interrupt sources which have been served, leaving the
     * ones raised in the meantime untouched.
     *
     * @param flags: interrupt flags to be cleared, as read from register 0x82.
     */
    inline void clearDmrInterrupts(const uint8_t flags)
    {
        writeCfgRegister(0x83, flags);
    }

    /**
     * Fetch the link control data of the last decoded burst. The chip performs
     * the error correction itself, so the bytes read here are already clean.
     *
     * NOTE: the chip keeps returning the link control of the previous
     * transmission until the current one has been decoded, callers have to
     * confirm it over more than one burst before trusting it.
     *
     * @param lc: destination buffer, C6000_LC_LENGTH bytes long.
     */
    void readDmrLinkControl(uint8_t *lc);

    /**
     * Read the voice payload of the last received DMR burst from the data
     * page of the control interface: three 9-byte AMBE frames. In direct mode
     * the chip keeps them at offset 0x66 of the page, where the Connect Systems
     * firmware reads them.
     *
     * @param payload: destination buffer, C6000_VOICE_PAYLOAD_LENGTH bytes.
     */
    void readDmrVoicePayload(uint8_t *payload);

    /**
     * Set up the DMR protocol side of the chip for direct mode, the way the
     * radio's stock firmware does when it loads a digital channel. The radios
     * differ here: each one's stock firmware sets the mode, interrupt and
     * receive registers its own way. The board's front end (IQ routing,
     * filters, codec and gains) is left to dmrMode().
     */
    void dmrDirectMode();

private:

    /**
     * Write a register with 16-bit address.
     *
     * @param opMode: "operating mode" specifier, see datasheet for details.
     * @param addr: register address.
     * @param value: value to be written.
     */
    void writeReg16(const C6000_SpiOpModes opMode, const uint16_t addr, const uint8_t value)
    {
        uint8_t data[4];

        data[0] = SPI_FLAGS_EXTD | static_cast< uint8_t >(opMode);
        data[2] = (addr >> 8) & 0x07;
        data[1] = addr & 0xFF;
        data[3] = value;

        ScopedChipSelect cs(uSpi, uCs);
        spi_send(uSpi, data, 4);
    }

    /**
     * Read a register with 16-bit address.
     *
     * @param opMode: "operating mode" specifier, see datasheet for details.
     * @param addr: register address.
     * @return: value read from the register.
     */
    uint8_t readReg16(const C6000_SpiOpModes opMode, const uint16_t addr)
    {
        uint8_t data[3];
        uint8_t value[1];

        data[0] = SPI_FLAGS_READ | SPI_FLAGS_EXTD
                | static_cast< uint8_t >(opMode);
        data[2] = (addr >> 8) & 0x07;
        data[1] = addr & 0xFF;

        ScopedChipSelect cs(uSpi, uCs);
        spi_send(uSpi, data, sizeof(data));
        spi_receive(uSpi, value, 1);

        return value[0];
    }

    /**
     * Read a block of consecutive registers, the chip increments the address
     * on its own after every byte.
     *
     * @param opMode: "operating mode" specifier, selects the register page.
     * @param addr: address of the first register to be read.
     * @param data: destination buffer.
     * @param len: amount of bytes to be read.
     */
    void readPage(const C6000_SpiOpModes opMode, const uint8_t addr,
                  uint8_t *data, const size_t len)
    {
        uint8_t cmd[2];

        cmd[0] = SPI_FLAGS_READ | static_cast< uint8_t >(opMode);
        cmd[1] = addr;

        ScopedChipSelect cs(uSpi, uCs);
        spi_send(uSpi, cmd, sizeof(cmd));
        spi_receive(uSpi, data, len);
    }
};

#endif /* HRC6000_H */
