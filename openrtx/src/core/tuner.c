/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/tuner.h"
#include "core/cps.h"

#include "interfaces/cps_io.h"
#include "rtx/rtx.h"
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

struct tuner tuner_getDefault(void)
{
    struct tuner t;
    const channel_t def = cps_getDefaultChannel();

    memset(&t, 0, sizeof(t));
    t.core.tuner_mode    = TUNER_VFO;
    t.core.bank_index    = 0;
    t.core.channel_index = 0;
    tuner_set_vfo_from_channel(&t, &def);

    return t;
}

enum tuner_mode tuner_get_mode(const struct tuner *t)
{
    if(t == NULL)
        return TUNER_VFO;
    return t->core.tuner_mode;
}

uint32_t tuner_get_tx_frequency(const struct tuner *t)
{
    if(t == NULL)
        return 0;

    if(t->core.tuner_mode == TUNER_VFO)
        return t->core.vfo.tx_frequency;

    return t->cache.channel.tx_frequency;
}

uint32_t tuner_get_rx_frequency(const struct tuner *t)
{
    if(t == NULL)
        return 0;

    if(t->core.tuner_mode == TUNER_VFO)
        return t->core.vfo.rx_frequency;

    return t->cache.channel.rx_frequency;
}

uint32_t tuner_get_power(const struct tuner *t)
{
    if(t == NULL)
        return 0;

    if(t->core.tuner_mode == TUNER_VFO)
        return t->core.power;

    return t->cache.channel.power;
}

uint8_t tuner_get_opmode(const struct tuner *t)
{
    if(t == NULL)
        return 0;

    if(t->core.tuner_mode == TUNER_VFO)
        return t->core.vfo.op_mode;

    return t->cache.channel.mode;
}

int tuner_get_M17_params(const struct tuner *t, m17Info_t *params)
{
    if( (t == NULL) || (params == NULL))
        return -EINVAL;

    if(t->core.tuner_mode == TUNER_VFO)
        *params = t->core.vfo.m17;
    else
    {
        if(t->cache.channel.mode == OPMODE_M17)
            *params = t->cache.channel.m17;
        else
            return -EINVAL;
    }

    return 0;
}

int tuner_get_FM_params(const struct tuner *t, fmInfo_t *params)
{
    if( (t == NULL) || (params == NULL))
        return -EINVAL;

    if(t->core.tuner_mode == TUNER_VFO)
        *params = t->core.vfo.fm;
    else
    {
        if(t->cache.channel.mode == OPMODE_FM)
            *params = t->cache.channel.fm;
        else
            return -EINVAL;
    }

    return 0;
}

int tuner_get_DMR_params(const struct tuner *t, dmrInfo_t *params)
{
    if( (t == NULL) || (params == NULL))
        return -EINVAL;

    if(t->core.tuner_mode == TUNER_VFO)
        *params = t->core.vfo.dmr;
    else
    {
        if(t->cache.channel.mode == OPMODE_DMR)
            *params = t->cache.channel.dmr;
        else
            return -EINVAL;
    }

    return 0;
}

void tuner_get_channel_name(const struct tuner *t, char *s, const size_t n)
{
    if(n == 0 || t == NULL || s == NULL)
        return;

    if(t->core.tuner_mode == TUNER_VFO)
        s[0] = '\0';
    else
        strncpy(s, t->cache.channel.name, n);
}

const channel_t *tuner_get_channel(const struct tuner *t)
{
    if(t == NULL || t->core.tuner_mode == TUNER_VFO)
        return NULL;

    return &t->cache.channel;
}

enum bandwidth tuner_get_bandwidth(const struct tuner *t)
{
    if(t == NULL)
        return BW_12_5;

    if(tuner_get_mode(t) == TUNER_VFO)
        return t->core.vfo.bandwidth;

    return t->cache.channel.bandwidth;
}

int tuner_switch_mode(struct tuner *t, enum tuner_mode mode)
{
    switch(mode)
    {
        case TUNER_VFO:
            t->core.tuner_mode = TUNER_VFO;
            break;
        case TUNER_CHAN:
            return tuner_set_mode_channel(t, t->core.channel_index);
            break;
        case TUNER_BANK:
            return tuner_set_mode_bank(t, t->core.bank_index, t->core.channel_index);
            break;
    }
    return 0;
}

int tuner_set_mode_bank(struct tuner *t, const uint16_t bank, const uint16_t channel)
{
    if(t == NULL)
        return -EINVAL;

    t->core.tuner_mode = TUNER_BANK;
    t->core.bank_index = bank;
    t->core.channel_index = channel;

    if(cps_readBankHeader(&(t->cache.bank), t->core.bank_index) < 0)
        return -EINVAL;
    if(channel >= t->cache.bank.ch_count)
        return -EINVAL;
    int ch = cps_readBankData(bank, channel);
    if(ch < 0)
        return -EINVAL;

    return cps_readChannel(&(t->cache.channel), ch);
}

int tuner_set_mode_channel(struct tuner *t, const uint16_t channel)
{
    if(t == NULL)
        return -EINVAL;

    t->core.tuner_mode = TUNER_CHAN;
    t->core.channel_index = channel;

    return cps_readChannel(&(t->cache.channel), channel);
}

void tuner_set_power(struct tuner *t, const uint32_t power)
{
    if(t == NULL)
        return;

    if(t->core.tuner_mode == TUNER_VFO)
        t->core.power = power;
    else
        t->cache.channel.power = power;
}

void tuner_set_vfo_rx_frequency(struct tuner *t, const uint32_t freq)
{
    if(t == NULL)
        return;

    t->core.vfo.rx_frequency = freq;
}

void tuner_set_vfo_tx_frequency(struct tuner *t, const uint32_t freq)
{
    if(t == NULL)
        return;

    t->core.vfo.tx_frequency = freq;
}

void tuner_set_vfo_opmode(struct tuner *t, const uint8_t opmode, const void *params)
{
    if( (t == NULL) || (params == NULL) )
        return;

    switch(opmode)
    {
        case OPMODE_M17:
            t->core.vfo.op_mode = opmode;
            m17Info_t *m17 = (m17Info_t *)params;
            t->core.vfo.m17 = *m17;
            break;
        case OPMODE_FM:
            t->core.vfo.op_mode = opmode;
            fmInfo_t *fm = (fmInfo_t *)params;
            t->core.vfo.fm = *fm;
            break;
        default:
            return;
    }

}

void tuner_set_vfo_from_channel(struct tuner *t, const channel_t *c)
{
    if((t == NULL) || (c == NULL))
        return;

    struct vfo *v = &t->core.vfo;

    v->op_mode      = c->mode;
    v->rx_frequency = c->rx_frequency;
    v->tx_frequency = c->tx_frequency;
    v->bandwidth    = c->bandwidth;
    v->sqlLevel     = c->sqlLevel;
    v->m17_can      = c->m17_can;
    memcpy(v->m17_dest, c->m17_dest, sizeof(v->m17_dest));
    memcpy(v->M17_meta_text, c->M17_meta_text, sizeof(v->M17_meta_text));

    switch(c->mode)
    {
        case OPMODE_FM:
            v->fm = c->fm;
            break;
        case OPMODE_M17:
            v->m17 = c->m17;
            break;
        case OPMODE_DMR:
            v->dmr = c->dmr;
            break;
    }

    t->core.power = c->power;
}

uint8_t tuner_get_sql_level(const struct tuner *t)
{
    if(t->core.tuner_mode == TUNER_VFO)
        return t->core.vfo.sqlLevel;

    return t->cache.channel.sqlLevel;
}

uint8_t tuner_get_m17_can(const struct tuner *t)
{
    if(t->core.tuner_mode == TUNER_VFO)
        return t->core.vfo.m17_can;

    return t->cache.channel.m17_can;
}

const char *tuner_get_m17_dest(const struct tuner *t)
{
    if(t->core.tuner_mode == TUNER_VFO)
        return t->core.vfo.m17_dest;

    return t->cache.channel.m17_dest;
}

const char *tuner_get_m17_meta_text(const struct tuner *t)
{
    if(t->core.tuner_mode == TUNER_VFO)
        return t->core.vfo.M17_meta_text;

    return t->cache.channel.M17_meta_text;
}

bool tuner_is_rx_only(const struct tuner *t)
{
    if(t->core.tuner_mode == TUNER_VFO)
        return false;

    return t->cache.channel.rx_only;
}

void tuner_set_rx_frequency(struct tuner *t, const uint32_t freq)
{
    if(t->core.tuner_mode == TUNER_VFO)
        t->core.vfo.rx_frequency = freq;
    else
        t->cache.channel.rx_frequency = freq;
}

void tuner_set_tx_frequency(struct tuner *t, const uint32_t freq)
{
    if(t->core.tuner_mode == TUNER_VFO)
        t->core.vfo.tx_frequency = freq;
    else
        t->cache.channel.tx_frequency = freq;
}

void tuner_set_opmode(struct tuner *t, const uint8_t opmode)
{
    if(t->core.tuner_mode == TUNER_VFO)
        t->core.vfo.op_mode = opmode;
    else
        t->cache.channel.mode = opmode;
}

void tuner_set_bandwidth(struct tuner *t, const enum bandwidth bw)
{
    if(t->core.tuner_mode == TUNER_VFO)
        t->core.vfo.bandwidth = bw;
    else
        t->cache.channel.bandwidth = bw;
}

void tuner_set_FM_params(struct tuner *t, const fmInfo_t *fm)
{
    if(t->core.tuner_mode == TUNER_VFO)
        t->core.vfo.fm = *fm;
    else
        t->cache.channel.fm = *fm;
}

void tuner_set_sql_level(struct tuner *t, const uint8_t level)
{
    if(t->core.tuner_mode == TUNER_VFO)
        t->core.vfo.sqlLevel = level;
    else
        t->cache.channel.sqlLevel = level;
}

void tuner_set_m17_can(struct tuner *t, const uint8_t can)
{
    if(t->core.tuner_mode == TUNER_VFO)
        t->core.vfo.m17_can = can & 0x0F;
    else
        t->cache.channel.m17_can = can & 0x0F;
}

void tuner_set_m17_dest(struct tuner *t, const char *dest)
{
    char *d = (t->core.tuner_mode == TUNER_VFO) ? t->core.vfo.m17_dest
                                                : t->cache.channel.m17_dest;
    const size_t n = sizeof(t->core.vfo.m17_dest);

    strncpy(d, dest, n - 1);
    d[n - 1] = '\0';
}

void tuner_set_m17_meta_text(struct tuner *t, const char *text)
{
    char *d = (t->core.tuner_mode == TUNER_VFO) ? t->core.vfo.M17_meta_text
                                                : t->cache.channel.M17_meta_text;
    const size_t n = sizeof(t->core.vfo.M17_meta_text);

    strncpy(d, text, n - 1);
    d[n - 1] = '\0';
}
