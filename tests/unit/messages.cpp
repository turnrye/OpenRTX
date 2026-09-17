/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <errno.h>
#include <string>
#include "core/messages.h"
#include "rtx_packet_stub.h"

namespace
{

constexpr uint8_t MODE_A = 3;
constexpr uint8_t MODE_B = 4;
constexpr uint8_t MODE_NONE = 0;

/*
 * A fake protocol whose packets are the message body as plain text: receive
 * files the buffer as-is from a fixed sender, transmit copies the body.
 */
size_t rxCalls = 0;
size_t txCalls = 0;
int txResult = 0;
std::string lastRx;

int fakeProcessRx(const struct pktDesc *pkt)
{
    rxCalls++;
    lastRx.assign(static_cast<const char *>(pkt->buffer), pkt->size);

    struct message msg = {};
    msg.body = lastRx.c_str();
    msg.bodyLen = lastRx.size();
    msg.mode = MODE_A;
    msg.direction = MSG_DIR_RX;
    msg.status = MSG_STATUS_RECEIVED;
    msg.unread = 1;
    strcpy(msg.sender, "W1AW");

    return messages_store(&msg, nullptr);
}

int fakeFormatTx(const struct message *msg, struct pktDesc *pkt)
{
    txCalls++;
    if (txResult != 0)
        return txResult;

    if (msg->bodyLen > pkt->size)
        return -EMSGSIZE;

    memcpy(pkt->buffer, msg->body, msg->bodyLen);
    pkt->size = msg->bodyLen;
    return 0;
}

int failRx(const struct pktDesc *pkt)
{
    (void)pkt;
    return -EIO;
}

int failTx(const struct message *msg, struct pktDesc *pkt)
{
    (void)msg;
    (void)pkt;
    return -EIO;
}

const struct messageOps fakeOps = { fakeProcessRx, fakeFormatTx, "Fake",
                                    MODE_A };
const struct messageOps otherOps = { failRx, failTx, "Other", MODE_B };

void setup()
{
    rtxStub_reset();
    messages_init();
    rxCalls = 0;
    txCalls = 0;
    txResult = 0;
    lastRx.clear();
}

/* Register the fake source and run the task once in its mode, so that the
 * storage is allocated and one receive descriptor is submitted. */
void activate()
{
    setup();
    REQUIRE(messages_registerSource(&fakeOps) == 0);
    messages_task(MODE_A);
    REQUIRE(messages_canCompose(MODE_A));
}

/* Build a store() template for a received message. */
struct message rxMessage(const char *sender, const char *body)
{
    struct message msg = {};

    msg.body = body;
    msg.bodyLen = strlen(body);
    msg.mode = MODE_A;
    msg.direction = MSG_DIR_RX;
    msg.status = MSG_STATUS_RECEIVED;
    msg.unread = 1;
    strncpy(msg.sender, sender, MSG_ADDR_MAX_LEN - 1);

    return msg;
}

/* Build a send() template for an outgoing message. */
struct message txMessage(const char *recipient, const char *body,
                         uint8_t mode = MODE_A)
{
    struct message msg = {};

    msg.body = body;
    msg.bodyLen = strlen(body);
    msg.mode = mode;
    strncpy(msg.sender, "N0CALL", MSG_ADDR_MAX_LEN - 1);
    strncpy(msg.recipient, recipient, MSG_ADDR_MAX_LEN - 1);

    return msg;
}

/* Build a store() template for a message being sent. */
struct message sendingMessage(const char *recipient, const char *body)
{
    struct message msg = txMessage(recipient, body);

    msg.direction = MSG_DIR_TX;
    msg.status = MSG_STATUS_SENDING;

    return msg;
}

/* Complete a submitted receive descriptor with the given packet bytes. */
void deliver(struct pktDesc *desc, const char *text)
{
    REQUIRE(desc != nullptr);
    REQUIRE(desc->status == PKT_STATUS_SUBMITTED);
    size_t len = strlen(text);
    REQUIRE(len <= desc->size);
    memcpy(desc->buffer, text, len);
    desc->size = len;
    desc->status = PKT_STATUS_DONE;
}

/* Hand a submitted descriptor back with an error, as an opmode does when it
 * is disabled. */
void cancel(struct pktDesc *desc)
{
    REQUIRE(desc != nullptr);
    REQUIRE(desc->status == PKT_STATUS_SUBMITTED);
    desc->res = -ECANCELED;
    desc->status = PKT_STATUS_ERROR;
}

} // namespace

/*
 * Storage lifecycle
 */

TEST_CASE("messages: inactive until the mode has a source", "[messages]")
{
    setup();

    CHECK(messages_count() == 0);
    CHECK(messages_countUnread() == 0);
    CHECK(messages_get(0) == nullptr);
    CHECK(messages_findBySequence(1) == SIZE_MAX);
    CHECK(messages_markRead(0, true) == -ENOENT);
    CHECK(messages_delete(0) == -ENOENT);
    CHECK(messages_setStatus(1, MSG_STATUS_SENT) == -ENOENT);
    CHECK_FALSE(messages_canCompose(MODE_A));

    struct message msg = rxMessage("W1AW", "a");
    CHECK(messages_store(&msg, nullptr) == -ENODEV);

    /* Running the task in a mode without a source changes nothing. */
    CHECK(messages_task(MODE_A) == 0);
    CHECK(messages_store(&msg, nullptr) == -ENODEV);
    CHECK(rtxStub_rxSubmissions() == 0);

    /* Registering a source is not enough either, the task must run. */
    REQUIRE(messages_registerSource(&fakeOps) == 0);
    CHECK_FALSE(messages_canCompose(MODE_A));
    CHECK(messages_store(&msg, nullptr) == -ENODEV);

    CHECK(messages_task(MODE_B) == 0);
    CHECK_FALSE(messages_canCompose(MODE_A));

    CHECK(messages_task(MODE_A) == 0);
    CHECK(messages_canCompose(MODE_A));
    CHECK_FALSE(messages_canCompose(MODE_B));
    CHECK(messages_store(&msg, nullptr) == 0);
    CHECK(messages_count() == 1);
}

TEST_CASE("messages: storage is released when leaving the mode", "[messages]")
{
    activate();

    struct message msg = rxMessage("W1AW", "a");
    REQUIRE(messages_store(&msg, nullptr) == 0);
    struct pktDesc *desc = rtxStub_rxDesc();
    REQUIRE(desc != nullptr);

    /* The rtx stage still holds the receive descriptor: nothing is freed. */
    CHECK(messages_task(MODE_NONE) == 0);
    CHECK(messages_count() == 1);
    CHECK_FALSE(messages_canCompose(MODE_NONE));
    CHECK(desc->status == PKT_STATUS_SUBMITTED);

    /* Once the descriptor is handed back the content is dropped. */
    cancel(desc);
    CHECK(messages_task(MODE_NONE) == 0);
    CHECK(messages_count() == 0);
    CHECK(messages_store(&msg, nullptr) == -ENODEV);
    CHECK(rtxStub_rxSubmissions() == 1);

    /* Coming back allocates fresh storage and rearms the receive slot. */
    CHECK(messages_task(MODE_A) == 0);
    CHECK(messages_canCompose(MODE_A));
    CHECK(messages_count() == 0);
    CHECK(rtxStub_rxSubmissions() == 2);
    CHECK(messages_store(&msg, nullptr) == 0);
}

TEST_CASE("messages: a pending transmission also delays the release",
          "[messages]")
{
    activate();

    struct message msg = txMessage("W1AW", "outgoing");
    REQUIRE(messages_send(&msg) == 0);
    cancel(rtxStub_rxDesc());

    CHECK(messages_task(MODE_NONE) == 0);
    CHECK(messages_count() == 1);

    cancel(rtxStub_txDesc());
    CHECK(messages_task(MODE_NONE) == 0);
    CHECK(messages_count() == 0);
}

TEST_CASE("messages: sequence numbers keep increasing across releases",
          "[messages]")
{
    activate();

    uint32_t first = 0;
    struct message msg = rxMessage("W1AW", "a");
    REQUIRE(messages_store(&msg, &first) == 0);

    cancel(rtxStub_rxDesc());
    messages_task(MODE_NONE);
    messages_task(MODE_A);

    uint32_t second = 0;
    REQUIRE(messages_store(&msg, &second) == 0);
    CHECK(second > first);
    CHECK(messages_findBySequence(first) == SIZE_MAX);
}

TEST_CASE("messages: init drops sources and stored entries", "[messages]")
{
    activate();

    struct message msg = rxMessage("W1AW", "a");
    REQUIRE(messages_store(&msg, nullptr) == 0);
    REQUIRE(messages_count() == 1);

    messages_init();
    CHECK(messages_count() == 0);
    CHECK_FALSE(messages_canCompose(MODE_A));
    CHECK(messages_task(MODE_A) == 0);
    CHECK_FALSE(messages_canCompose(MODE_A));
}

/*
 * Entry storage
 */

TEST_CASE("messages: store copies the body and assigns a sequence",
          "[messages]")
{
    activate();

    char body[] = "hello";
    struct message msg = rxMessage("W1AW", body);
    uint32_t seq = 0;

    REQUIRE(messages_store(&msg, &seq) == 0);
    CHECK(seq != 0);

    /* The caller's buffer is no longer referenced. */
    memset(body, 'x', sizeof(body) - 1);

    const struct message *stored = messages_get(0);
    REQUIRE(stored != nullptr);
    CHECK(stored->sequence == seq);
    CHECK(std::string(stored->body) == "hello");
    CHECK(stored->bodyLen == 5);
    CHECK(std::string(stored->sender) == "W1AW");
    CHECK(stored->direction == MSG_DIR_RX);
    CHECK(stored->status == MSG_STATUS_RECEIVED);
    CHECK(stored->unread == 1);
    CHECK(messages_count() == 1);
    CHECK(messages_countUnread() == 1);
}

TEST_CASE("messages: store rejects bad arguments", "[messages]")
{
    activate();

    CHECK(messages_store(nullptr, nullptr) == -EINVAL);

    struct message msg = rxMessage("W1AW", "");
    msg.body = nullptr;
    msg.bodyLen = 1;
    CHECK(messages_store(&msg, nullptr) == -EINVAL);

    msg.body = "x";
    msg.bodyLen = MSG_BODY_MAX_LEN;
    CHECK(messages_store(&msg, nullptr) == -EMSGSIZE);

    CHECK(messages_count() == 0);
}

TEST_CASE("messages: empty bodies are allowed", "[messages]")
{
    activate();

    struct message msg = rxMessage("W1AW", "");
    msg.body = nullptr;
    msg.bodyLen = 0;

    REQUIRE(messages_store(&msg, nullptr) == 0);
    REQUIRE(messages_get(0) != nullptr);
    CHECK(messages_get(0)->body[0] == '\0');
}

TEST_CASE("messages: entries are listed newest first", "[messages]")
{
    activate();

    uint32_t first = 0;
    uint32_t second = 0;
    uint32_t third = 0;
    struct message msg = rxMessage("W1AW", "one");
    REQUIRE(messages_store(&msg, &first) == 0);
    msg = rxMessage("W1AW", "two");
    REQUIRE(messages_store(&msg, &second) == 0);
    msg = rxMessage("W1AW", "three");
    REQUIRE(messages_store(&msg, &third) == 0);

    CHECK(first < second);
    CHECK(second < third);
    REQUIRE(messages_count() == 3);
    CHECK(messages_get(0)->sequence == third);
    CHECK(messages_get(1)->sequence == second);
    CHECK(messages_get(2)->sequence == first);
    CHECK(std::string(messages_get(0)->body) == "three");
    CHECK(std::string(messages_get(2)->body) == "one");
}

TEST_CASE("messages: findBySequence tracks positions", "[messages]")
{
    activate();

    uint32_t seq[3];
    for (auto &s : seq) {
        struct message msg = rxMessage("W1AW", "x");
        REQUIRE(messages_store(&msg, &s) == 0);
    }

    CHECK(messages_findBySequence(seq[0]) == 2);
    CHECK(messages_findBySequence(seq[1]) == 1);
    CHECK(messages_findBySequence(seq[2]) == 0);
    CHECK(messages_findBySequence(0) == SIZE_MAX);
    CHECK(messages_findBySequence(seq[2] + 1) == SIZE_MAX);

    REQUIRE(messages_delete(1) == 0);
    CHECK(messages_findBySequence(seq[1]) == SIZE_MAX);
    CHECK(messages_findBySequence(seq[0]) == 1);
    CHECK(messages_findBySequence(seq[2]) == 0);
}

TEST_CASE("messages: task reports unread arrivals once", "[messages]")
{
    activate();

    struct message msg = rxMessage("W1AW", "a");
    REQUIRE(messages_store(&msg, nullptr) == 0);
    REQUIRE(messages_store(&msg, nullptr) == 0);

    /* Sent messages and pre-read ones are not arrivals. */
    struct message tx = sendingMessage("W1AW", "b");
    REQUIRE(messages_store(&tx, nullptr) == 0);
    msg.unread = 0;
    REQUIRE(messages_store(&msg, nullptr) == 0);

    CHECK(messages_task(MODE_A) == 2);
    CHECK(messages_task(MODE_A) == 0);
    CHECK(messages_countUnread() == 2);
}

TEST_CASE("messages: markRead toggles the unread flag", "[messages]")
{
    activate();

    struct message msg = rxMessage("W1AW", "a");
    REQUIRE(messages_store(&msg, nullptr) == 0);

    CHECK(messages_markRead(0, true) == 0);
    CHECK(messages_get(0)->unread == 0);
    CHECK(messages_countUnread() == 0);

    CHECK(messages_markRead(0, false) == 0);
    CHECK(messages_get(0)->unread == 1);
    CHECK(messages_countUnread() == 1);

    CHECK(messages_markRead(1, true) == -ENOENT);
}

TEST_CASE("messages: setStatus updates an entry by sequence", "[messages]")
{
    activate();

    struct message tx = sendingMessage("W1AW", "b");
    uint32_t seq = 0;
    REQUIRE(messages_store(&tx, &seq) == 0);
    CHECK(messages_get(0)->status == MSG_STATUS_SENDING);

    CHECK(messages_setStatus(seq, MSG_STATUS_SENT) == 0);
    CHECK(messages_get(0)->status == MSG_STATUS_SENT);

    CHECK(messages_setStatus(seq + 1, MSG_STATUS_FAILED) == -ENOENT);
}

TEST_CASE("messages: delete removes an entry and closes the gap", "[messages]")
{
    activate();

    for (const char *body : { "one", "two", "three" }) {
        struct message msg = rxMessage("W1AW", body);
        REQUIRE(messages_store(&msg, nullptr) == 0);
    }

    CHECK(messages_delete(3) == -ENOENT);
    REQUIRE(messages_delete(1) == 0);
    REQUIRE(messages_count() == 2);
    CHECK(std::string(messages_get(0)->body) == "three");
    CHECK(std::string(messages_get(1)->body) == "one");

    REQUIRE(messages_delete(0) == 0);
    REQUIRE(messages_delete(0) == 0);
    CHECK(messages_count() == 0);
    CHECK(messages_delete(0) == -ENOENT);
}

TEST_CASE("messages: a full entry table evicts the oldest entry", "[messages]")
{
    activate();

    uint32_t oldest = 0;
    struct message msg = rxMessage("W1AW", "first");
    REQUIRE(messages_store(&msg, &oldest) == 0);

    for (size_t i = 1; i < CONFIG_MESSAGES_MAX_ENTRIES; i++) {
        msg = rxMessage("W1AW", "filler");
        REQUIRE(messages_store(&msg, nullptr) == 0);
    }
    REQUIRE(messages_count() == CONFIG_MESSAGES_MAX_ENTRIES);
    CHECK(messages_findBySequence(oldest) != SIZE_MAX);

    uint32_t newest = 0;
    msg = rxMessage("W1AW", "last");
    REQUIRE(messages_store(&msg, &newest) == 0);

    CHECK(messages_count() == CONFIG_MESSAGES_MAX_ENTRIES);
    CHECK(messages_findBySequence(oldest) == SIZE_MAX);
    CHECK(messages_get(0)->sequence == newest);
    CHECK(std::string(messages_get(messages_count() - 1)->body) == "filler");
}

TEST_CASE("messages: a full body pool evicts the oldest entries", "[messages]")
{
    activate();

    /* Bodies of maximum length fill the pool after a few stores. */
    std::string big(MSG_BODY_MAX_LEN - 1, 'a');
    const size_t perPool = CONFIG_MESSAGES_POOL_BYTES / MSG_BODY_MAX_LEN;
    REQUIRE(perPool >= 1);

    uint32_t oldest = 0;
    struct message msg = rxMessage("W1AW", big.c_str());
    REQUIRE(messages_store(&msg, &oldest) == 0);

    for (size_t i = 1; i < perPool; i++) {
        REQUIRE(messages_store(&msg, nullptr) == 0);
    }
    CHECK(messages_count() == perPool);
    CHECK(messages_findBySequence(oldest) != SIZE_MAX);

    /* One more wraps the pool and lands on the oldest body. */
    std::string different(MSG_BODY_MAX_LEN - 1, 'b');
    msg = rxMessage("W1AW", different.c_str());
    REQUIRE(messages_store(&msg, nullptr) == 0);

    CHECK(messages_findBySequence(oldest) == SIZE_MAX);
    CHECK(messages_count() == perPool);

    /* Every surviving body is intact. */
    for (size_t i = 0; i < messages_count(); i++) {
        const struct message *m = messages_get(i);
        REQUIRE(m != nullptr);
        CHECK(strlen(m->body) == m->bodyLen);
        CHECK(m->bodyLen == MSG_BODY_MAX_LEN - 1);
        CHECK(std::string(m->body) == ((i == 0) ? different : big));
    }
}

TEST_CASE("messages: addresses are always NUL-terminated", "[messages]")
{
    activate();

    struct message msg = rxMessage("W1AW", "a");
    memset(msg.sender, 'A', sizeof(msg.sender));
    memset(msg.recipient, 'B', sizeof(msg.recipient));
    REQUIRE(messages_store(&msg, nullptr) == 0);

    const struct message *stored = messages_get(0);
    REQUIRE(stored != nullptr);
    CHECK(strlen(stored->sender) == MSG_ADDR_MAX_LEN - 1);
    CHECK(strlen(stored->recipient) == MSG_ADDR_MAX_LEN - 1);
}

/*
 * Sources and packet handling
 */

TEST_CASE("messages: source registration", "[messages]")
{
    setup();

    CHECK(messages_registerSource(nullptr) == -EINVAL);

    struct messageOps incomplete = fakeOps;
    incomplete.formatTx = nullptr;
    CHECK(messages_registerSource(&incomplete) == -EINVAL);

    REQUIRE(messages_registerSource(&fakeOps) == 0);
    CHECK(messages_registerSource(&fakeOps) == -EEXIST);
    REQUIRE(messages_registerSource(&otherOps) == 0);

    /* Table full: CONFIG_MESSAGES_MAX_SOURCES defaults to two. */
    struct messageOps third = fakeOps;
    third.mode = 5;
    CHECK(messages_registerSource(&third) == -ENOSPC);

    messages_task(MODE_A);
    CHECK(messages_canCompose(MODE_A));
    messages_task(MODE_B);
    CHECK(messages_canCompose(MODE_B));
    CHECK_FALSE(messages_canCompose(5));
}

TEST_CASE("messages: no receive without a source for the mode", "[messages]")
{
    setup();
    REQUIRE(messages_registerSource(&fakeOps) == 0);

    messages_task(MODE_B);
    CHECK(rtxStub_rxSubmissions() == 0);

    messages_task(MODE_A);
    CHECK(rtxStub_rxSubmissions() == 1);
}

TEST_CASE("messages: received packets reach their source", "[messages]")
{
    activate();

    struct pktDesc *desc = rtxStub_rxDesc();
    REQUIRE(desc != nullptr);
    CHECK(desc->buffer != nullptr);
    CHECK(desc->size == MSG_PKT_MAX_SIZE);

    /* Nothing happens while the descriptor is pending. */
    CHECK(messages_task(MODE_A) == 0);
    CHECK(rtxStub_rxSubmissions() == 1);
    CHECK(rxCalls == 0);

    deliver(desc, "hello");
    CHECK(messages_task(MODE_A) == 1);

    CHECK(rxCalls == 1);
    CHECK(lastRx == "hello");
    REQUIRE(messages_count() == 1);
    CHECK(std::string(messages_get(0)->body) == "hello");
    CHECK(messages_task(MODE_A) == 0);

    /* The slot is resubmitted right away with its full buffer. */
    CHECK(rtxStub_rxSubmissions() == 2);
    CHECK(desc->status == PKT_STATUS_SUBMITTED);
    CHECK(desc->size == MSG_PKT_MAX_SIZE);
}

TEST_CASE("messages: receive errors are dropped and rearmed", "[messages]")
{
    activate();

    struct pktDesc *desc = rtxStub_rxDesc();
    REQUIRE(desc != nullptr);

    desc->res = -EIO;
    desc->status = PKT_STATUS_ERROR;
    messages_task(MODE_A);

    CHECK(rxCalls == 0);
    CHECK(messages_count() == 0);
    CHECK(rtxStub_rxSubmissions() == 2);
    CHECK(desc->status == PKT_STATUS_SUBMITTED);
}

TEST_CASE("messages: a refused receive is retried", "[messages]")
{
    setup();
    REQUIRE(messages_registerSource(&fakeOps) == 0);

    rtxStub_setRxResult(-EAGAIN);
    messages_task(MODE_A);
    CHECK(rtxStub_rxSubmissions() == 0);

    rtxStub_setRxResult(0);
    messages_task(MODE_A);
    CHECK(rtxStub_rxSubmissions() == 1);
}

TEST_CASE("messages: a packet is handled by the mode it was submitted in",
          "[messages]")
{
    setup();
    REQUIRE(messages_registerSource(&fakeOps) == 0);
    REQUIRE(messages_registerSource(&otherOps) == 0);

    messages_task(MODE_A);
    struct pktDesc *desc = rtxStub_rxDesc();
    deliver(desc, "late");

    /* The mode changed to another one with a source before completion. */
    CHECK(messages_task(MODE_B) == 1);

    CHECK(rxCalls == 1);
    CHECK(lastRx == "late");
    REQUIRE(messages_count() == 1);
    CHECK(messages_get(0)->mode == MODE_A);

    /* The slot is rearmed for the new mode. */
    CHECK(desc->status == PKT_STATUS_SUBMITTED);
    CHECK(rtxStub_rxSubmissions() == 2);
}

TEST_CASE("messages: send formats and submits a packet", "[messages]")
{
    activate();

    struct message msg = txMessage("W1AW", "outgoing");
    REQUIRE(messages_send(&msg) == 0);
    CHECK(txCalls == 1);
    CHECK(rtxStub_txSubmissions() == 1);

    struct pktDesc *desc = rtxStub_txDesc();
    REQUIRE(desc != nullptr);
    CHECK(desc->status == PKT_STATUS_SUBMITTED);
    CHECK(desc->size == 8);
    CHECK(memcmp(desc->buffer, "outgoing", 8) == 0);

    REQUIRE(messages_count() == 1);
    const struct message *stored = messages_get(0);
    CHECK(stored->direction == MSG_DIR_TX);
    CHECK(stored->status == MSG_STATUS_SENDING);
    CHECK(stored->unread == 0);
    CHECK(std::string(stored->sender) == "N0CALL");
    CHECK(std::string(stored->recipient) == "W1AW");
    CHECK(std::string(stored->body) == "outgoing");

    messages_task(MODE_A);
    CHECK(messages_get(0)->status == MSG_STATUS_SENDING);

    desc->status = PKT_STATUS_DONE;
    CHECK(messages_task(MODE_A) == 0);
    CHECK(messages_get(0)->status == MSG_STATUS_SENT);
    CHECK(desc->status == PKT_STATUS_IDLE);

    /* The slot is free again. */
    CHECK(messages_send(&msg) == 0);
    CHECK(rtxStub_txSubmissions() == 2);
}

TEST_CASE("messages: a failed transmission is reported", "[messages]")
{
    activate();

    struct message msg = txMessage("W1AW", "outgoing");
    REQUIRE(messages_send(&msg) == 0);

    struct pktDesc *desc = rtxStub_txDesc();
    desc->res = -EIO;
    desc->status = PKT_STATUS_ERROR;
    messages_task(MODE_A);

    CHECK(messages_get(0)->status == MSG_STATUS_FAILED);
    CHECK(desc->status == PKT_STATUS_IDLE);
}

TEST_CASE("messages: send failures leave a failed entry", "[messages]")
{
    activate();

    CHECK(messages_send(nullptr) == -EINVAL);

    struct message msg = txMessage("W1AW", "nowhere", MODE_B);
    CHECK(messages_send(&msg) == -ENOENT);
    CHECK(messages_count() == 0);

    /* Refused by the source. */
    msg = txMessage("W1AW", "first");
    txResult = -EMSGSIZE;
    CHECK(messages_send(&msg) == -EMSGSIZE);
    CHECK(rtxStub_txSubmissions() == 0);
    txResult = 0;

    /* Refused by the rtx stage. */
    rtxStub_setTxResult(-ENOTSUP);
    CHECK(messages_send(&msg) == -ENOTSUP);
    rtxStub_setTxResult(0);

    /* Accepted, then a second one while the first is in flight. */
    REQUIRE(messages_send(&msg) == 0);
    msg = txMessage("W1AW", "second");
    CHECK(messages_send(&msg) == -EBUSY);

    REQUIRE(messages_count() == 4);
    CHECK(std::string(messages_get(0)->body) == "second");
    CHECK(messages_get(0)->status == MSG_STATUS_FAILED);
    CHECK(messages_get(1)->status == MSG_STATUS_SENDING);
    CHECK(messages_get(2)->status == MSG_STATUS_FAILED);
    CHECK(messages_get(3)->status == MSG_STATUS_FAILED);
}

TEST_CASE("messages: send is refused while the inbox is inactive", "[messages]")
{
    setup();
    REQUIRE(messages_registerSource(&fakeOps) == 0);

    struct message msg = txMessage("W1AW", "outgoing");
    CHECK(messages_send(&msg) == -ENODEV);
    CHECK(txCalls == 0);
}
