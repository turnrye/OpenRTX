/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * m17_sms_source_test.cpp — Unit tests for the M17 SMS message source.
 *
 * Exercises vtable callbacks (count, get, supported_actions, invoke_action,
 * start_compose) and the public compose helpers without requiring a live RTX
 * thread.  rtx_addPacketRx / rtx_addPacketTx are called but their return
 * values are gracefully handled by m17_sms_init() / m17_sms_send().
 */

#include <catch2/catch_test_macros.hpp>
#include <cstring>

extern "C" {
#include "hwconfig.h"
}
#include "core/m17_sms.h"
#include "core/messages.h"

#ifdef CONFIG_M17

/* Helper: remove all active entries between test cases. */
static void clean_sms(void)
{
    size_t n;
    while ((n = m17_sms_vtable.count(NULL)) > 0) {
        message_header_t *h = m17_sms_vtable.get(NULL, 0);
        if (h == nullptr)
            break;
        m17_sms_vtable.invoke_action(h, MSG_ACTION_DELETE);
    }
    m17_sms_compose_clear();
}

TEST_CASE("m17_sms: initial state after init", "[m17_sms]")
{
    m17_sms_init();
    REQUIRE(m17_sms_vtable.count(NULL) == 0);
    REQUIRE(m17_sms_compose_pending() == false);
    REQUIRE(m17_sms_compose_recipient()[0] == '\0');
}

TEST_CASE("m17_sms: send creates TX entry", "[m17_sms]")
{
    m17_sms_init();
    int ret = m17_sms_send("hello world", 11, "N0CALL");
    REQUIRE(ret == 0);
    REQUIRE(m17_sms_vtable.count(NULL) == 1);

    message_header_t *hdr = m17_sms_vtable.get(NULL, 0);
    REQUIRE(hdr != nullptr);
    REQUIRE(hdr->direction == MSG_DIR_TX);
    REQUIRE(hdr->unread == false);
    REQUIRE(hdr->body != nullptr);
    REQUIRE(hdr->body_len == 11);
    REQUIRE(strncmp(static_cast<const char *>(hdr->body), "hello world", 11)
            == 0);
}

TEST_CASE("m17_sms: send rejects null arguments", "[m17_sms]")
{
    m17_sms_init();
    REQUIRE(m17_sms_send(nullptr, 0, "N0CALL") == -EINVAL);
    REQUIRE(m17_sms_send("hello", 5, nullptr) == -EINVAL);
    REQUIRE(m17_sms_vtable.count(NULL) == 0);
}

TEST_CASE("m17_sms: send rejects empty recipient", "[m17_sms]")
{
    m17_sms_init();
    REQUIRE(m17_sms_send("hello", 5, "") == -EINVAL);
    REQUIRE(m17_sms_vtable.count(NULL) == 0);
}

TEST_CASE("m17_sms: MSG_ACTION_MARK_READ clears unread flag", "[m17_sms]")
{
    m17_sms_init();
    m17_sms_send("test msg", 8, "N0CALL");

    message_header_t *hdr = m17_sms_vtable.get(NULL, 0);
    REQUIRE(hdr != nullptr);
    hdr->unread = true; /* simulate unread */

    int ret = m17_sms_vtable.invoke_action(hdr, MSG_ACTION_MARK_READ);
    REQUIRE(ret == 0);
    REQUIRE(hdr->unread == false);
}

TEST_CASE("m17_sms: MSG_ACTION_MARK_UNREAD sets unread flag", "[m17_sms]")
{
    m17_sms_init();
    m17_sms_send("test msg", 8, "N0CALL");

    message_header_t *hdr = m17_sms_vtable.get(NULL, 0);
    REQUIRE(hdr != nullptr);
    hdr->unread = false;

    int ret = m17_sms_vtable.invoke_action(hdr, MSG_ACTION_MARK_UNREAD);
    REQUIRE(ret == 0);
    REQUIRE(hdr->unread == true);
}

TEST_CASE("m17_sms: MSG_ACTION_DELETE removes entry", "[m17_sms]")
{
    m17_sms_init();
    m17_sms_send("bye", 3, "N0CALL");
    REQUIRE(m17_sms_vtable.count(NULL) == 1);

    message_header_t *hdr = m17_sms_vtable.get(NULL, 0);
    REQUIRE(hdr != nullptr);

    int ret = m17_sms_vtable.invoke_action(hdr, MSG_ACTION_DELETE);
    REQUIRE(ret == 0);
    REQUIRE(m17_sms_vtable.count(NULL) == 0);
    REQUIRE(m17_sms_vtable.get(NULL, 0) == nullptr);
}

TEST_CASE("m17_sms: MSG_ACTION_REPLY sets compose recipient", "[m17_sms]")
{
    m17_sms_init();
    m17_sms_send("test", 4, "N0CALL");

    message_header_t *hdr = m17_sms_vtable.get(NULL, 0);
    REQUIRE(hdr != nullptr);

    /* Simulate an RX entry so REPLY action makes sense. */
    hdr->direction = MSG_DIR_RX;
    strncpy(hdr->sender, "W1AW", sizeof(hdr->sender) - 1);
    hdr->sender[sizeof(hdr->sender) - 1] = '\0';

    int ret = m17_sms_vtable.invoke_action(hdr, MSG_ACTION_REPLY);
    REQUIRE(ret == 0);
    REQUIRE(m17_sms_compose_pending() == true);
    REQUIRE(strcmp(m17_sms_compose_recipient(), "W1AW") == 0);
}

TEST_CASE("m17_sms: start_compose sets pending with no recipient", "[m17_sms]")
{
    m17_sms_init();
    REQUIRE(m17_sms_compose_pending() == false);

    m17_sms_vtable.start_compose(NULL);
    REQUIRE(m17_sms_compose_pending() == true);
    REQUIRE(m17_sms_compose_recipient()[0] == '\0');
}

TEST_CASE("m17_sms: compose_clear resets pending state", "[m17_sms]")
{
    m17_sms_init();
    m17_sms_vtable.start_compose(NULL);
    REQUIRE(m17_sms_compose_pending() == true);

    m17_sms_compose_clear();
    REQUIRE(m17_sms_compose_pending() == false);
    REQUIRE(m17_sms_compose_recipient()[0] == '\0');
}

TEST_CASE("m17_sms: multiple entries indexed correctly", "[m17_sms]")
{
    m17_sms_init();
    m17_sms_send("first", 5, "N0CALL");
    m17_sms_send("second", 6, "N0CALL");
    m17_sms_send("third", 5, "N0CALL");

    REQUIRE(m17_sms_vtable.count(NULL) == 3);
    REQUIRE(m17_sms_vtable.get(NULL, 0) != nullptr);
    REQUIRE(m17_sms_vtable.get(NULL, 1) != nullptr);
    REQUIRE(m17_sms_vtable.get(NULL, 2) != nullptr);
    REQUIRE(m17_sms_vtable.get(NULL, 3) == nullptr);

    /* Delete the middle entry and verify index compaction. */
    message_header_t *mid = m17_sms_vtable.get(NULL, 1);
    REQUIRE(strncmp(static_cast<const char *>(mid->body), "second", 6) == 0);
    m17_sms_vtable.invoke_action(mid, MSG_ACTION_DELETE);

    REQUIRE(m17_sms_vtable.count(NULL) == 2);
    REQUIRE(m17_sms_vtable.get(NULL, 2) == nullptr);
}

TEST_CASE("m17_sms: supported_actions includes REPLY only for RX", "[m17_sms]")
{
    m17_sms_init();
    m17_sms_send("tx msg", 6, "N0CALL");

    message_header_t *hdr = m17_sms_vtable.get(NULL, 0);
    REQUIRE(hdr != nullptr);

    /* TX entry: no REPLY */
    hdr->direction = MSG_DIR_TX;
    uint32_t acts_tx = m17_sms_vtable.supported_actions(hdr);
    REQUIRE((acts_tx & MSG_ACTION_VIEW) != 0);
    REQUIRE((acts_tx & MSG_ACTION_DELETE) != 0);
    REQUIRE((acts_tx & MSG_ACTION_REPLY) == 0);

    /* RX entry: REPLY offered */
    hdr->direction = MSG_DIR_RX;
    uint32_t acts_rx = m17_sms_vtable.supported_actions(hdr);
    REQUIRE((acts_rx & MSG_ACTION_REPLY) != 0);
}

TEST_CASE("m17_sms: vtable metadata is correct", "[m17_sms]")
{
    REQUIRE(m17_sms_vtable.name != nullptr);
    REQUIRE(strcmp(m17_sms_vtable.name, "M17 SMS") == 0);
    REQUIRE(m17_sms_vtable.mode_id == 3); /* OPMODE_M17 */
    REQUIRE(m17_sms_vtable.count != nullptr);
    REQUIRE(m17_sms_vtable.get != nullptr);
    REQUIRE(m17_sms_vtable.supported_actions != nullptr);
    REQUIRE(m17_sms_vtable.invoke_action != nullptr);
    REQUIRE(m17_sms_vtable.start_compose != nullptr);
}

#else  /* !CONFIG_M17 */

TEST_CASE("m17_sms: skipped — CONFIG_M17 not defined", "[m17_sms]")
{
    /* Nothing to test when M17 is disabled. */
    SUCCEED();
}

#endif /* CONFIG_M17 */
