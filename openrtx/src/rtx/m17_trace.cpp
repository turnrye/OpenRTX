/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * M17 receive trace, see m17_trace.hpp and README.md. Lines are ASCII,
 * fields separated by single spaces, times in ms since boot:
 *
 *   ID <label>                       answer to "ID"
 *   ON <ms> / OFF <ms>               answer to "TRACE ON" / "TRACE OFF"
 *   HB <ms> <stack> <free> <dropped> once a second: receive thread stack
 *                                    size, its least free stack since
 *                                    boot, lines dropped so far
 *   LOCK <ms> <0|1>                  demodulator lock changed
 *   LSF <ms> <valid> <F|I> <hex>     link setup data changed: its 30 bytes,
 *                                    from a link setup frame or the LICH
 *   STR <ms> <fn> <lsf> <hex>        stream frame: the frame number and 16
 *                                    payload bytes the decoder holds (the
 *                                    previous ones when it rejected this
 *                                    frame), and whether the LSF is valid
 *   PKT <ms> <counter> <eof> <hex>   packet frame: counter, end flag and
 *                                    the 25 data bytes
 *   EOT <ms>                         end of transmission frame
 *   FRM <ms> <type>                  any other frame type
 *   PLAY <ms> <fn>                   a stream frame went to the speaker
 *
 * TEST ONLY bench console, read by the UI thread in every mode, named after
 * the rtxlink CAT fields. Each command answers one line, or ERR <ms> <cmd>:
 *
 *   MODE [M17|FM]                    MODE <ms> <M17|FM|..>
 *   FREQ [<rx Hz> [<tx Hz>]]         FREQ <ms> <rx> <tx>
 *   POWER [<mW>]                     POWER <ms> <mW>
 *   MODEM [CHIP|ANALOG]              MODEM <ms> <CHIP|ANALOG>
 *   MOD [<mod1> <mod2>|CAL]          MOD <ms> <SET|CAL> <mod1> <mod2>:
 *                                    HR_C6000 Mod1 (0x46), Mod2 (0x45) at
 *                                    the next key-up, set or the pair the
 *                                    radio takes from its calibration
 *   CALL [<callsign>]                CALL <ms> <callsign>
 *   PTT [ON|OFF]                     PTT <ms> <0|1>
 *   TX <ms>                          TX <ms> <duration>: key for that long
 *   STAT                             STAT <ms> <mode> <rx> <tx> <mW> <modem>
 *                                    <SET|CAL> <mod1> <mod2> <ptt>
 */

#include "rtx/m17_trace.hpp"
#include "rtx/m17_bench.h"
#include "core/state.h"
#include "interfaces/delays.h"
#include "core/memory_profiling.h"
#include "core/threads.h"
#include <pthread.h>
#include <atomic>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

extern "C" ssize_t vcom_readBlock(void *buf, size_t len);
extern "C" ssize_t vcom_writeBlock(const void *buf, size_t len);

extern "C" int m17bench_diag(char *buf, size_t len);
extern "C" int m17bench_regs(uint8_t start, uint8_t count, char *buf,
                             size_t len);
extern "C" void m17bench_wreg(uint8_t reg, uint8_t value);
extern "C" int m17bench_copy(int16_t *out, int max);

// Builds without a USB serial port read and write nothing
extern "C" __attribute__((weak)) ssize_t vcom_readBlock(void *buf, size_t len)
{
    (void) buf;
    (void) len;
    return 0;
}

extern "C" __attribute__((weak)) ssize_t vcom_writeBlock(const void *buf,
                                                         size_t len)
{
    (void) buf;
    return len;
}

using namespace M17;

/// Ring buffer between the receive thread and the writer thread.
static constexpr size_t RING_SIZE = 4096;
/// Largest write: one USB full speed bulk packet.
static constexpr size_t WRITE_CHUNK = 64;
/// Longest line, LSF with its 60 hex digits.
static constexpr size_t LINE_SIZE = 112;
/// Longest command from the host.
static constexpr size_t COMMAND_SIZE = 40;
/// Period of the stack report.
static constexpr long long HEARTBEAT_MS = 1000;
/// Pause of the writer thread when the ring is empty.
static constexpr unsigned int WRITER_IDLE_MS = 5;
/// Stack of the writer thread.
static constexpr size_t WRITER_STACK = 1024;

static char ring[RING_SIZE];               ///< Lines waiting to be written.
static std::atomic<uint32_t> head{0};      ///< Next byte the producer fills.
static std::atomic<uint32_t> tail{0};      ///< Next byte the writer sends.
static std::atomic<bool> tracing{false};   ///< The host asked for the trace.
static bool writerStarted = false;         ///< The writer thread is running.
static uint32_t dropped = 0;               ///< Lines that didn't fit.
static long long lastHeartbeat = 0;        ///< Time of the last HB line.
static bool lastLock = false;              ///< Lock state last reported.

static char line[LINE_SIZE];               ///< Line being formatted.
static size_t lineLen = 0;                 ///< Characters in line.
static char command[COMMAND_SIZE];         ///< Command being received.
static size_t commandLen = 0;              ///< Characters in command.

static pthread_mutex_t ringMutex = PTHREAD_MUTEX_INITIALIZER; ///< Producers.
static std::atomic<bool> resetReq{false};  ///< TRACE ON: reset the trace state.
static std::atomic<long long> pttUntil{0}; ///< Host keying ends at this tick.
static std::atomic<bool> modSet{false};    ///< Modulation pair overridden.
static std::atomic<uint16_t> modPair{0};   ///< Mod1 << 8 | Mod2.
static std::atomic<uint16_t> calPair{0};   ///< The calibration's pair.
static std::atomic<bool> syncReq{false};   ///< Settings changed: sync rtx.
static std::atomic<bool> rxFlip{false};    ///< Extra RX phase inversion.
static std::atomic<bool> rxInverted{false};///< RX inversion last applied.

static LinkSetupFrame lastLsf;             ///< Link setup data last reported.
static LinkSetupFrame lsfCopy;             ///< Scratch copy of the decoder's.
static StreamFrame streamCopy;             ///< Scratch copy of the decoder's.

/**
 * Write out the ring buffer, in chunks, on its own thread: a USB write
 * waits for the host, which must never hold up reception.
 *
 * @param arg: unused.
 * @return never.
 */
static void *writer(void *arg)
{
    (void) arg;

    while (true) {
        uint32_t t = tail.load(std::memory_order_relaxed);
        uint32_t h = head.load(std::memory_order_acquire);
        if (t == h) {
            delayMs(WRITER_IDLE_MS);
            continue;
        }

        size_t start = t % RING_SIZE;
        size_t len = h - t;
        if (len > WRITE_CHUNK)
            len = WRITE_CHUNK;
        if (len > RING_SIZE - start)
            len = RING_SIZE - start;

        vcom_writeBlock(&ring[start], len);
        tail.store(t + len, std::memory_order_release);
    }

    return nullptr;
}

/**
 * Start the writer thread, once.
 */
static void startWriter()
{
    if (writerStarted)
        return;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, WRITER_STACK);
#ifdef _MIOSIX
    // Below the receive and UI threads: it only drains the ring buffer
    struct sched_param param;
    param.sched_priority = THREAD_PRIO_LOW;
    pthread_attr_setschedparam(&attr, &param);
#endif
    pthread_t thread;
    writerStarted = (pthread_create(&thread, &attr, writer, nullptr) == 0);
}

/**
 * Start a line with its keyword.
 *
 * @param keyword: the line's keyword.
 */
static void begin(const char *keyword)
{
    lineLen = 0;
    while ((*keyword != '\0') && (lineLen < LINE_SIZE))
        line[lineLen++] = *keyword++;
}

/**
 * Append a space and a decimal number.
 *
 * @param value: the number.
 */
static void dec(unsigned long long value)
{
    char digits[20];
    size_t n = 0;

    do {
        digits[n++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while ((value != 0) && (n < sizeof(digits)));

    if (lineLen < LINE_SIZE)
        line[lineLen++] = ' ';
    while ((n > 0) && (lineLen < LINE_SIZE))
        line[lineLen++] = digits[--n];
}

/**
 * Append a space and bytes as hex digits.
 *
 * @param data: the bytes.
 * @param len: how many.
 */
static void hex(const uint8_t *data, size_t len)
{
    static constexpr char DIGITS[] = "0123456789abcdef";

    if (lineLen < LINE_SIZE)
        line[lineLen++] = ' ';
    for (size_t i = 0; (i < len) && ((lineLen + 2) <= LINE_SIZE); i++) {
        line[lineLen++] = DIGITS[data[i] >> 4];
        line[lineLen++] = DIGITS[data[i] & 0x0F];
    }
}

/**
 * Append a space and one character.
 *
 * @param c: the character.
 */
static void chr(char c)
{
    if ((lineLen + 2) <= LINE_SIZE) {
        line[lineLen++] = ' ';
        line[lineLen++] = c;
    }
}

/**
 * End the line and queue it for the writer, or count it as dropped when
 * the ring is full. The receive thread is the only producer.
 */
static void push(const char *data, size_t len)
{
    pthread_mutex_lock(&ringMutex);
    uint32_t h = head.load(std::memory_order_relaxed);
    uint32_t t = tail.load(std::memory_order_acquire);
    if ((RING_SIZE - (h - t)) < len) {
        dropped++;
    } else {
        for (size_t i = 0; i < len; i++)
            ring[(h + i) % RING_SIZE] = data[i];
        head.store(h + static_cast<uint32_t>(len), std::memory_order_release);
    }
    pthread_mutex_unlock(&ringMutex);
}

static void send()
{
    if (lineLen < LINE_SIZE)
        line[lineLen++] = '\n';

    push(line, lineLen);
}

/**
 * Queue one console reply line, keyword and time first.
 *
 * @param keyword: the reply's keyword.
 * @param fmt: printf format of the rest, may be empty.
 */
static void reply(const char *keyword, const char *fmt, ...)
{
    char buf[LINE_SIZE];
    int n = snprintf(buf, sizeof(buf), "%s %lld", keyword,
                     static_cast<long long>(getTick()));

    if ((n > 0) && (fmt[0] != '\0') && (static_cast<size_t>(n) < sizeof(buf) - 2)) {
        buf[n++] = ' ';
        va_list ap;
        va_start(ap, fmt);
        int m = vsnprintf(&buf[n], sizeof(buf) - n - 1, fmt, ap);
        va_end(ap);
        if (m > 0)
            n += m;
    }

    if (n > static_cast<int>(sizeof(buf)) - 2)
        n = sizeof(buf) - 2;
    buf[n++] = '\n';
    push(buf, n);
}

/**
 * Start a line with its keyword and the time.
 *
 * @param keyword: the line's keyword.
 */
static void event(const char *keyword)
{
    begin(keyword);
    dec(static_cast<unsigned long long>(getTick()));
}

/**
 * Name of an operating mode.
 *
 * @param mode: the mode.
 * @return its name.
 */
static const char *modeName(uint8_t mode)
{
    switch (mode) {
        case OPMODE_FM:
            return "FM";
        case OPMODE_M17:
            return "M17";
        case OPMODE_DMR:
            return "DMR";
        default:
            return "NONE";
    }
}

/**
 * Act on a complete command from the host. Runs on the UI thread.
 */
static void handleCommand()
{
    startWriter();

    char *arg = strchr(command, ' ');
    if (arg != nullptr)
        *arg++ = '\0';
    const bool hasArg = (arg != nullptr) && (*arg != '\0');

    if (strcmp(command, "ID") == 0) {
        reply("ID", "%s", M17_TRACE_LABEL);
    } else if (strcmp(command, "TRACE") == 0) {
        if (hasArg && (strcmp(arg, "ON") == 0)) {
            resetReq.store(true);
            tracing.store(true);
            reply("ON", "");
        } else {
            reply("OFF", "");
            tracing.store(false);
        }
    } else if (strcmp(command, "MODE") == 0) {
        if (hasArg) {
            pthread_mutex_lock(&state_mutex);
            if (strcmp(arg, "M17") == 0)
                state.channel.mode = OPMODE_M17;
            else if (strcmp(arg, "FM") == 0)
                state.channel.mode = OPMODE_FM;
            pthread_mutex_unlock(&state_mutex);
            syncReq.store(true);
        }
        reply("MODE", "%s", modeName(state.channel.mode));
    } else if (strcmp(command, "FREQ") == 0) {
        if (hasArg) {
            char *end;
            uint32_t rx = strtoul(arg, &end, 10);
            uint32_t tx = (*end == ' ') ? strtoul(end + 1, nullptr, 10) : rx;
            pthread_mutex_lock(&state_mutex);
            state.channel.rx_frequency = rx;
            state.channel.tx_frequency = tx;
            pthread_mutex_unlock(&state_mutex);
            syncReq.store(true);
        }
        reply("FREQ", "%lu %lu",
              static_cast<unsigned long>(state.channel.rx_frequency),
              static_cast<unsigned long>(state.channel.tx_frequency));
    } else if (strcmp(command, "POWER") == 0) {
        if (hasArg) {
            pthread_mutex_lock(&state_mutex);
            state.channel.power = strtoul(arg, nullptr, 10);
            pthread_mutex_unlock(&state_mutex);
            syncReq.store(true);
        }
        reply("POWER", "%lu", static_cast<unsigned long>(state.channel.power));
    } else if (strcmp(command, "MODEM") == 0) {
        if (hasArg) {
            pthread_mutex_lock(&state_mutex);
            state.settings.m17_chip_modem = (strcmp(arg, "CHIP") == 0) ? 1 : 0;
            pthread_mutex_unlock(&state_mutex);
            syncReq.store(true);
        }
        reply("MODEM", "%s", state.settings.m17_chip_modem ? "CHIP" : "ANALOG");
    } else if (strcmp(command, "MOD") == 0) {
        if (hasArg && (strcmp(arg, "CAL") == 0)) {
            modSet.store(false);
        } else if (hasArg) {
            char *end;
            uint32_t m1 = strtoul(arg, &end, 10);
            uint32_t m2 = strtoul(end, nullptr, 10);
            modPair.store(static_cast<uint16_t>(((m1 & 0xFF) << 8) | (m2 & 0xFF)));
            modSet.store(true);
        }
        const bool set = modSet.load();
        const uint16_t pair = set ? modPair.load() : calPair.load();
        reply("MOD", "%s %u %u", set ? "SET" : "CAL", pair >> 8, pair & 0xFF);
    } else if (strcmp(command, "CALL") == 0) {
        if (hasArg) {
            pthread_mutex_lock(&state_mutex);
            strncpy(state.settings.callsign, arg,
                    sizeof(state.settings.callsign) - 1);
            state.settings.callsign[sizeof(state.settings.callsign) - 1] = '\0';
            pthread_mutex_unlock(&state_mutex);
            syncReq.store(true);
        }
        reply("CALL", "%s", state.settings.callsign);
    } else if (strcmp(command, "PTT") == 0) {
        if (hasArg)
            pttUntil.store((strcmp(arg, "ON") == 0) ? (getTick() + 600000) : 0);
        reply("PTT", "%d", m17bench_ptt() ? 1 : 0);
    } else if (strcmp(command, "TX") == 0) {
        long ms = hasArg ? strtol(arg, nullptr, 10) : 0;
        pttUntil.store(getTick() + ms);
        reply("TX", "%ld", ms);
    } else if (strcmp(command, "COPY") == 0) {
        // COPY: the FM copy's last block of raw samples, 16 per line in hex,
        // COPY <ms> <index> <hex>..., then COPY <ms> END <count>
        static int16_t block[256];
        const int n = m17bench_copy(block, 256);
        for (int i = 0; i < n; i += 16) {
            char hexs[16 * 4 + 1];
            int k = 0;
            for (int j = i; (j < i + 16) && (j < n); j++)
                k += snprintf(&hexs[k], sizeof(hexs) - k, "%04x",
                              static_cast<uint16_t>(block[j]));
            reply("COPY", "%d %s", i, hexs);
        }
        reply("COPY", "END %d", n);
    } else if (strcmp(command, "RXFLIP") == 0) {
        // RXFLIP [0|1]: invert the M17 receive phase once more
        if (hasArg)
            rxFlip.store(strcmp(arg, "1") == 0);
        reply("RXFLIP", "%d %d", rxFlip.load() ? 1 : 0,
              (rxInverted.load() != rxFlip.load()) ? 1 : 0);
    } else if (strcmp(command, "WREG") == 0) {
        // WREG <reg hex> <value hex>: write an HR_C6000 config register
        char *end = nullptr;
        unsigned long reg = hasArg ? strtoul(arg, &end, 16) : 0;
        unsigned long value = (end != nullptr) && (*end == ' ')
                            ? strtoul(end + 1, nullptr, 16) : 0;
        m17bench_wreg(static_cast<uint8_t>(reg), static_cast<uint8_t>(value));
        reply("WREG", "%02lx %02lx", reg, value);
    } else if (strcmp(command, "REGS") == 0) {
        // REGS <start hex> [<count, at most 32>]: HR_C6000 config registers
        char *end = nullptr;
        unsigned long start = hasArg ? strtoul(arg, &end, 16) : 0;
        unsigned long count = (end != nullptr) && (*end == ' ')
                            ? strtoul(end + 1, nullptr, 10) : 32;
        if (count > 32)
            count = 32;
        char regs[LINE_SIZE - 16];
        m17bench_regs(static_cast<uint8_t>(start), static_cast<uint8_t>(count),
                      regs, sizeof(regs));
        reply("REGS", "%02lx %s", start, regs);
    } else if (strcmp(command, "DIAG") == 0) {
        char diag[LINE_SIZE - 16];
        m17bench_diag(diag, sizeof(diag));
        reply("DIAG", "%s", diag);
    } else if (strcmp(command, "STAT") == 0) {
        char mod[16];
        const bool set = modSet.load();
        const uint16_t pair = set ? modPair.load() : calPair.load();
        snprintf(mod, sizeof(mod), "%s %u %u", set ? "SET" : "CAL", pair >> 8,
                 pair & 0xFF);
        reply("STAT", "%s %lu %lu %lu %s %s %d", modeName(state.channel.mode),
              static_cast<unsigned long>(state.channel.rx_frequency),
              static_cast<unsigned long>(state.channel.tx_frequency),
              static_cast<unsigned long>(state.channel.power),
              state.settings.m17_chip_modem ? "CHIP" : "ANALOG", mod,
              m17bench_ptt() ? 1 : 0);
    } else {
        reply("ERR", "%s", command);
    }
}

void m17bench_poll()
{
    char c;
    while (vcom_readBlock(&c, 1) == 1) {
        if ((c == '\r') || (c == '\n')) {
            command[commandLen] = '\0';
            if (commandLen > 0)
                handleCommand();
            commandLen = 0;
        } else if (commandLen < (COMMAND_SIZE - 1)) {
            command[commandLen++] = c;
        }
    }
}

// The platform's diagnostics line, if it has one
extern "C" __attribute__((weak)) int m17bench_diag(char *buf, size_t len)
{
    return snprintf(buf, len, "none");
}

// The FM copy's last block, if the build has one
extern "C" __attribute__((weak)) int m17bench_copy(int16_t *out, int max)
{
    (void) out;
    (void) max;
    return 0;
}

// The platform's register write, if it has one
extern "C" __attribute__((weak)) void m17bench_wreg(uint8_t reg, uint8_t value)
{
    (void) reg;
    (void) value;
}

// The platform's register dump, if it has one
extern "C" __attribute__((weak)) int m17bench_regs(uint8_t start,
                                                   uint8_t count, char *buf,
                                                   size_t len)
{
    (void) start;
    (void) count;
    return snprintf(buf, len, "none");
}

bool m17bench_ptt()
{
    return getTick() < pttUntil.load();
}

bool m17bench_mod(uint8_t *mod1, uint8_t *mod2)
{
    if (!modSet.load())
        return false;

    uint16_t pair = modPair.load();
    *mod1 = pair >> 8;
    *mod2 = pair & 0xFF;
    return true;
}

void m17bench_setCal(uint8_t mod1, uint8_t mod2)
{
    calPair.store(static_cast<uint16_t>((mod1 << 8) | mod2));
}

bool m17bench_rxFlip()
{
    return rxFlip.load();
}

void m17bench_noteRxPhase(bool inverted)
{
    rxInverted.store(inverted);
}

bool m17bench_takeSync()
{
    return syncReq.exchange(false);
}

void m17trace_poll()
{
    if (resetReq.exchange(false)) {
        lastLsf.clear();
        lastHeartbeat = 0;
    }

    if (!tracing.load())
        return;

    long long now = getTick();
    if ((now - lastHeartbeat) >= HEARTBEAT_MS) {
        lastHeartbeat = now;
        event("HB");
        dec(getStackSize());
        dec(getAbsoluteFreeStack());
        dec(dropped);
        send();
    }
}

void m17trace_lock(bool lock)
{
    if (lock == lastLock)
        return;

    lastLock = lock;
    if (!tracing.load())
        return;

    event("LOCK");
    dec(lock ? 1 : 0);
    send();
}

void m17trace_frame(uint8_t type, FrameDecoder &decoder)
{
    if (!tracing.load())
        return;

    FrameType frameType = static_cast<FrameType>(type);

    lsfCopy = decoder.getLsf();
    if (memcmp(lsfCopy.getData(), lastLsf.getData(),
               sizeof(LinkSetupFrame)) != 0) {
        lastLsf = lsfCopy;
        event("LSF");
        dec(lsfCopy.valid() ? 1 : 0);
        chr((frameType == FrameType::LINK_SETUP) ? 'F' : 'I');
        hex(lsfCopy.getData(), sizeof(LinkSetupFrame));
        send();
    }

    switch (frameType) {
        case FrameType::STREAM:
            streamCopy = decoder.getStreamFrame();
            event("STR");
            dec(streamCopy.getFrameNumber());
            dec(lsfCopy.valid() ? 1 : 0);
            hex(streamCopy.data(), sizeof(payload_t));
            send();
            break;

        case FrameType::PACKET: {
            const PacketFrame &packet = decoder.getPacketFrame();
            event("PKT");
            dec(packet.getCounter());
            dec(packet.isEof() ? 1 : 0);
            hex(packet.data(), PacketFrame::DATA_SIZE);
            send();
            break;
        }

        case FrameType::EOT:
            event("EOT");
            send();
            break;

        case FrameType::LINK_SETUP:
            break;

        default:
            event("FRM");
            dec(type);
            send();
            break;
    }
}

void m17trace_played(StreamFrame &frame)
{
    if (!tracing.load())
        return;

    event("PLAY");
    dec(frame.getFrameNumber());
    send();
}
