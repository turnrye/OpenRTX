/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ORTX_UI_VFOVIEW_HPP
#define ORTX_UI_VFOVIEW_HPP

#include "core/View.hpp"
#include "layout/Flex.hpp"
#include "widgets/Widgets.hpp"
#include "widgets/TopBar.hpp"
#include "widgets/FreqHero.hpp"
#include "widgets/DotMeter.hpp"
#include "widgets/TextInput.hpp"
#include "core/state.h"

namespace ortxui
{

/**
 * The VFO home screen (mockups 5-6): a left-weighted instrument readout.
 *
 * A Flex column of shared top bar / frequency hero (big freq + sub-digits +
 * mode-tone-power stack) / channel line (index + name in blue) / signal meter /
 * open space. Widgets are direct members in static storage. build() wires the
 * tree and lays it out once; syncFromState() pulls the shown radio fields from
 * the state snapshot change-gated, and drives the meter from the RX/TX status
 * (green RX with an S-scale, orange TX as a solid power bar).
 *
 * As the navigation root, ENTER drills into the main menu (wired via setMenu()).
 * The screen is also interactive: UP/DOWN (or the knob) tune the frequency in
 * VFO mode or step through channels in memory mode; ESC toggles between VFO and
 * memory mode; a digit key opens an in-place frequency keypad entry (RX then TX
 * set). Radio-affecting edits raise requestSyncRtx(). Mirrors the classic VFO.
 */
class VfoView : public View
{
public:
    void build();

    void setMenu(View *menu)
    {
        menu_ = menu;
    }

    void syncFromState(const state_t &s) override;
    NavIntent onEvent(const Event &e) override;
    void announce() override;

    Screen &screen() override
    {
        return screen_;
    }

private:
    void syncMode(const channel_t &ch);
    void syncMeter(const state_t &s);
    void announceVfoState(); //< speak channel summary (MEM) or freq+mode (VFO)

    /* --- interaction --- */
    NavIntent
    onInputEvent(const Event &e);        //< key handling while entering a freq
    void stepFreq(int dir);              //< VFO: tune RX+TX by one freq step
    void stepChannel(int dir);           //< MEM: load the next/prev channel
    void toggleVfoMem();                 //< switch between VFO and memory mode
    bool loadChannel(int16_t index);     //< bank-aware channel load into state
    void beginInput(uint8_t firstDigit); //< open keypad freq entry with a digit
    void inputDigit(uint8_t digit); //< accumulate a digit into the active set
    void confirmInput();            //< ENTER: advance RX->TX, or apply TX
    void applyInput();              //< commit the entered RX/TX if in band
    void exitInput();               //< leave keypad entry, restore the readout
    void refreshInput();            //< redraw the hero/label from the entry
    void announceFreq();            //< speak the current channel frequencies
    void clearFmTone();             //< drop the momentary FM tone if latched

    /* --- M17 destination (shown in the channel line, edited via #) --- */
    void composeChanLine(const state_t &s, char *idxOut, char *nameOut,
                         uint16_t nameSz); //< build the index + name slot text
    void m17DstLabel(char *out, uint16_t sz); //< "#<dest>" (or "#BROADCAST")
    NavIntent
    onDstEditEvent(const Event &e); //< key handling while editing dest
    void beginDstEdit();            //< open the destination editor
    void endDstEdit(bool commit);   //< commit/cancel the destination
    void dstCycle(int dir);         //< cycle the char under the cursor
    void dstMove(int dir);          //< move the cursor (extends w/ space)
    void
    renderDstEdit(); //< write the bracket-cursor dest into the channel line
    void announceDstChar(); //< speak the char under the cursor

    Screen screen_;

    Flex root_;
    TopBar topBar_;
    FreqHero hero_;
    Flex chanRow_;
    Label chanIdx_;
    Label chanName_;
    DotMeter meter_;
    Flex spacer_;

    /* Channel-line slot: composed index + name text, change-gated by string
     * compare (the name slot depends on tuner mode, channel name, radio mode
     * and, in M17, the destination — a plain string diff covers them all). */
    char idxCache_[8] = { 1, 0 };   //< sentinel forces the first paint
    char nameCache_[40] = { 1, 0 }; //< composed name, or bracket-cursor dest
    char readoutBuf_[12] = { 0 };
    char toneBuf_[8] = { 0 };       //< PL/CTCSS tone shown in the mode stack

    uint32_t lastFreq_ = 0xFFFFFFFFu;
    uint8_t lastMode_ = 0xFFu;
    uint8_t lastBandwidth_ = 0xFFu;
    uint8_t lastToneEn_ = 0xFFu;
    uint32_t lastPower_ = 0xFFFFFFFFu;
    uint8_t lastStatus_ = 0xFFu;
    int32_t lastRssi_ = INT32_MIN;

    /* Frequency keypad entry (mirrors the classic MAIN_VFO_INPUT screen). */
    bool inputActive_ = false; //< keypad entry in progress
    bool inputTxSet_ = false;  //< false = editing RX, true = TX
    uint8_t inputPos_ = 0;     //< digits entered in the active set
    uint32_t newRx_ = 0;       //< frequency accumulated so far, RX
    uint32_t newTx_ = 0;       //< frequency accumulated so far, TX

    /* M17 destination editor (classic MAIN_VFO # -> dst input): edited in place
     * through the shared TextInput widget (inline bracket rendering). */
    bool dstEditing_ = false; //< destination editor active
    char dstBuf_[10] = { 0 }; //< settings.m17_dest is char[10]
    TextInput dst_;

    View *menu_ = nullptr;
};

} // namespace ortxui

#endif /* ORTX_UI_VFOVIEW_HPP */
