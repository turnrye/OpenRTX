/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// TEST ONLY: bench campaign for the HR_C6000's physical layer receive

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include "interfaces/delays.h"
#include "core/threads.h"
#include "C6000_phy.h"

extern "C" void m17bench_emit(const char *line);

namespace
{

/// Register pages, as the HR_C6000's SPI command byte numbers them.
constexpr uint8_t PAGE_AUX    = 1;
constexpr uint8_t PAGE_DATA   = 2;
constexpr uint8_t PAGE_CONFIG = 4;

constexpr uint16_t RECORD_ADDR  = 0x030;  ///< Record in page 2.
constexpr size_t   RECORD_BYTES = 288;    ///< 144 symbols, 2 bytes each.
constexpr size_t   HEX_PER_LINE = 96;     ///< Hex digits per PHD line.
constexpr size_t   MAX_SETS     = 16;
constexpr size_t   MAX_WATCH    = 8;

struct RegWrite
{
    uint8_t page;
    uint8_t reg;
    uint8_t value;
};

HR_C6000 *c6000 = nullptr;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
std::atomic<bool> active{false};
std::atomic<uint32_t> tsCount{0};

std::array<RegWrite, MAX_SETS> sets;
size_t nSets = 0;
std::array<RegWrite, MAX_WATCH> watch;
size_t nWatch = 0;
RegWrite tick;
bool tickOn = false;
bool baseOn = true;
bool full = false;
uint8_t pattern[6] = {0xD5, 0xD7, 0xF7, 0x7F, 0xD7, 0x57};

uint32_t rearmK = 0;           ///< Records without a sync before a re-arm.
uint32_t rearmDelay = 40;      ///< ms from the 0x41 write to the 0x40 one.
uint8_t rearmR41 = 0x60;
uint8_t armR40 = 0x40;         ///< 0x40 value of the set-up, for re-arms.
long long rearmAt = -1;

uint32_t tsSeen = 0;
uint32_t seq = 0, polls = 0, otherFlags = 0, rearms = 0, noSync = 0;
uint8_t lastFlags = 0;

/// Configuration registers the set-up changes, saved for PHY OFF.
constexpr std::array<uint8_t, 7> BASE_REGS = {0x41, 0x10, 0x36, 0x5F, 0x81,
                                              0x40, 0x34};
std::array<uint8_t, BASE_REGS.size()> savedBase;
std::array<uint8_t, MAX_SETS> savedSets;

void write(const RegWrite &w)
{
    c6000->benchWrite(w.page, w.reg, w.value);
}

void sendRecord(uint8_t r51, uint8_t r5f, uint8_t flags)
{
    static uint8_t rec[RECORD_BYTES];
    char line[112];

    int n = snprintf(line, sizeof(line), "PHR %lld %lu %lu %02x %02x %02x",
                     static_cast<long long>(getTick()),
                     static_cast<unsigned long>(seq),
                     static_cast<unsigned long>(tsCount.load()),
                     r51, r5f, flags);
    for(size_t i = 0; i < nWatch; i++)
        n += snprintf(&line[n], sizeof(line) - n, " %02x",
                      c6000->benchRead(watch[i].page, watch[i].reg));
    m17bench_emit(line);

    c6000->benchReadRam(PAGE_DATA, RECORD_ADDR, rec, sizeof(rec));

    // The level bytes, or all of them: two bytes per symbol, the second a
    // copy of the slice in the chips seen so far
    const size_t step  = full ? 1 : 2;
    const size_t bytes = sizeof(rec) / step;
    size_t part = 0;
    for(size_t i = 0; i < bytes; part++)
    {
        int k = snprintf(line, sizeof(line), "PHD %lu %u ",
                         static_cast<unsigned long>(seq),
                         static_cast<unsigned>(part));
        for(size_t j = 0; (j < HEX_PER_LINE / 2) && (i < bytes); j++, i++)
            k += snprintf(&line[k], sizeof(line) - k, "%02x", rec[i * step]);
        m17bench_emit(line);
    }
}

void applyBase()
{
    c6000->benchWrite(PAGE_CONFIG, 0x41, 0x00);
    c6000->benchWrite(PAGE_CONFIG, 0x10, 0x02);    // Physical layer receive
    c6000->benchWrite(PAGE_CONFIG, 0x36, 0x00);    // Nothing to the line out
    for(uint8_t i = 0; i < sizeof(pattern); i++)
        c6000->benchWrite(PAGE_AUX, 0x47 + i, pattern[i]);
    c6000->benchWrite(PAGE_CONFIG, 0x5F, 0xF0);    // All comparators
    c6000->benchWrite(PAGE_CONFIG, 0x83, 0xFF);    // Clear the flags
    c6000->benchWrite(PAGE_CONFIG, 0x40, 0x40);
    c6000->benchWrite(PAGE_CONFIG, 0x41, 0x40);
}

bool parseHex(char **p, uint32_t *v)
{
    char *end;
    unsigned long x = strtoul(*p, &end, 16);
    if(end == *p)
        return false;
    *v = x;
    *p = end;
    return true;
}

bool parseWrite(char *p, RegWrite *w)
{
    uint32_t page, reg, value;
    if(!parseHex(&p, &page) || !parseHex(&p, &reg) || !parseHex(&p, &value))
        return false;
    *w = {static_cast<uint8_t>(page), static_cast<uint8_t>(reg),
          static_cast<uint8_t>(value)};
    return true;
}

int stat(char *reply, size_t len)
{
    return snprintf(reply, len,
        "%s seq %lu polls %lu other %lu last %02x rearms %lu sets %u watch %u "
        "tick %u base %u full %u",
        active.load() ? "ON" : "OFF", static_cast<unsigned long>(seq),
        static_cast<unsigned long>(polls), static_cast<unsigned long>(otherFlags),
        lastFlags, static_cast<unsigned long>(rearms),
        static_cast<unsigned>(nSets), static_cast<unsigned>(nWatch),
        tickOn ? 1u : 0u, baseOn ? 1u : 0u, full ? 1u : 0u);
}

void turnOn()
{
    for(size_t i = 0; i < BASE_REGS.size(); i++)
        savedBase[i] = c6000->writtenCfgRegister(BASE_REGS[i]);
    for(size_t i = 0; i < nSets; i++)
        savedSets[i] = (sets[i].page == PAGE_CONFIG)
                     ? c6000->writtenCfgRegister(sets[i].reg) : 0;

    if(baseOn)
        applyBase();
    armR40 = 0x40;
    for(size_t i = 0; i < nSets; i++)
    {
        write(sets[i]);
        if((sets[i].page == PAGE_CONFIG) && (sets[i].reg == 0x40))
            armR40 = sets[i].value;
    }

    seq = polls = otherFlags = rearms = noSync = 0;
    lastFlags = 0;
    rearmAt = -1;
    tsSeen = tsCount.load();
    active.store(true);
}

void turnOff()
{
    active.store(false);
    for(size_t i = nSets; i > 0; i--)
    {
        if(sets[i - 1].page == PAGE_CONFIG)
            c6000->benchWrite(PAGE_CONFIG, sets[i - 1].reg, savedSets[i - 1]);
    }
    for(size_t i = BASE_REGS.size(); i > 0; i--)
        c6000->benchWrite(PAGE_CONFIG, BASE_REGS[i - 1], savedBase[i - 1]);
    c6000->benchWrite(PAGE_CONFIG, 0x83, 0xFF);
}

}  // namespace

/**
 * Poll the chip every 2 ms on a thread of its own, with the stack the line
 * formatting needs.
 *
 * @param arg: unused.
 * @return never.
 */
static void *phyThread(void *arg)
{
    (void) arg;
    while(true)
    {
        C6000phy_task();
        sleepFor(0, 2);
    }
    return nullptr;
}

void C6000phy_init(HR_C6000 *chip)
{
    c6000 = chip;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 2048);
    sched_param param;
    param.sched_priority = THREAD_PRIO_HIGH;
    pthread_attr_setschedparam(&attr, &param);
    pthread_t thread;
    pthread_create(&thread, &attr, phyThread, nullptr);
}

void C6000phy_timeslotIrq()
{
    tsCount.fetch_add(1);
}

bool C6000phy_active()
{
    return active.load();
}

void C6000phy_task()
{
    if((c6000 == nullptr) || (active.load() == false))
        return;

    pthread_mutex_lock(&mutex);
    if(active.load())
    {
        const uint32_t ts = tsCount.load();
        if(tickOn && (ts != tsSeen))
            write(tick);
        tsSeen = ts;

        if((rearmAt >= 0) && (getTick() >= rearmAt))
        {
            c6000->benchWrite(PAGE_CONFIG, 0x40, armR40);
            rearmAt = -1;
        }

        const uint8_t flags = c6000->benchRead(PAGE_CONFIG, 0x82);
        polls++;
        lastFlags = flags;
        if((flags & 0x01) != 0)
        {
            const uint8_t r51 = c6000->benchRead(PAGE_CONFIG, 0x51);
            const uint8_t r5f = c6000->benchRead(PAGE_CONFIG, 0x5F);
            sendRecord(r51, r5f, flags);
            c6000->benchWrite(PAGE_CONFIG, 0x83, 0x01);
            seq++;

            if((r51 & 0x03) == 0x02)
                noSync = 0;
            else if((rearmK != 0) && (++noSync >= rearmK) && (rearmAt < 0))
            {
                c6000->benchWrite(PAGE_CONFIG, 0x41, rearmR41);
                rearmAt = getTick() + rearmDelay;
                noSync = 0;
                rearms++;
            }
        }
        else if(flags != 0)
        {
            otherFlags++;
        }
    }
    pthread_mutex_unlock(&mutex);
}

int C6000phy_command(char *args, char *reply, size_t len)
{
    if(c6000 == nullptr)
        return -1;

    char *arg = strchr(args, ' ');
    if(arg != nullptr)
        *arg++ = '\0';

    int ret = 0;
    pthread_mutex_lock(&mutex);
    if(strcmp(args, "ON") == 0)
    {
        if(active.load() == false)
            turnOn();
    }
    else if(strcmp(args, "OFF") == 0)
    {
        if(active.load())
            turnOff();
    }
    else if(strcmp(args, "CLEAR") == 0)
    {
        nSets = 0;
        nWatch = 0;
        tickOn = false;
        rearmK = 0;
        baseOn = true;
        full = false;
    }
    else if((strcmp(args, "SET") == 0) && (arg != nullptr) && (nSets < MAX_SETS))
    {
        RegWrite w;
        if(parseWrite(arg, &w))
            sets[nSets++] = w;
        else
            ret = -1;
    }
    else if((strcmp(args, "WATCH") == 0) && (arg != nullptr) && (nWatch < MAX_WATCH))
    {
        uint32_t page, reg;
        char *p = arg;
        if(parseHex(&p, &page) && parseHex(&p, &reg))
            watch[nWatch++] = {static_cast<uint8_t>(page), static_cast<uint8_t>(reg), 0};
        else
            ret = -1;
    }
    else if((strcmp(args, "TICK") == 0) && (arg != nullptr))
    {
        if(strcmp(arg, "OFF") == 0)
            tickOn = false;
        else if(parseWrite(arg, &tick))
            tickOn = true;
        else
            ret = -1;
    }
    else if((strcmp(args, "REARM") == 0) && (arg != nullptr))
    {
        uint32_t k, d, r41;
        char *p = arg;
        if(strcmp(arg, "OFF") == 0)
            rearmK = 0;
        else if(parseHex(&p, &k) && parseHex(&p, &d) && parseHex(&p, &r41))
        {
            rearmK = k;
            rearmDelay = d;
            rearmR41 = static_cast<uint8_t>(r41);
        }
        else
            ret = -1;
    }
    else if((strcmp(args, "PAT") == 0) && (arg != nullptr) && (strlen(arg) >= 12))
    {
        for(int i = 0; i < 6; i++)
        {
            char byte[3] = {arg[2 * i], arg[2 * i + 1], '\0'};
            pattern[i] = static_cast<uint8_t>(strtoul(byte, nullptr, 16));
        }
    }
    else if((strcmp(args, "BASE") == 0) && (arg != nullptr))
    {
        baseOn = (arg[0] == '1');
    }
    else if((strcmp(args, "FULL") == 0) && (arg != nullptr))
    {
        full = (arg[0] == '1');
    }
    else if(strcmp(args, "STAT") != 0)
    {
        ret = -1;
    }

    if(ret == 0)
        stat(reply, len);
    pthread_mutex_unlock(&mutex);
    return ret;
}
