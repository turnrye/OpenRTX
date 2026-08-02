/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPMODE_DMR_H
#define OPMODE_DMR_H

#include "rtx/OpMode.hpp"

/**
 * Operating mode for DMR.
 *
 * At the moment this only brings the baseband up as a receiver and surfaces
 * the link control of the calls it decodes, which is what the user interface
 * shows as "last heard". There is no voice path yet.
 */
class OpMode_DMR : public OpMode
{
public:

    OpMode_DMR() { }

    ~OpMode_DMR() { }

    virtual void enable() override;

    virtual void disable() override;

    virtual void update(rtxStatus_t *const status, const bool newCfg) override;

    virtual opmode getID() override
    {
        return OPMODE_DMR;
    }

    virtual bool rxSquelchOpen() override
    {
        return false;
    }

private:

    bool enabled = false;
};

#endif /* OPMODE_DMR_H */
