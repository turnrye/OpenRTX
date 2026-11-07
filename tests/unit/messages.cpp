/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <errno.h>
#include <string>
#include <vector>
#include "core/messages.h"
#include "rtx_packet_stub.h"

constexpr uint8_t MODE_A = 3;
constexpr uint8_t MODE_B = 4;
constexpr uint8_t MODE_NONE = 0;

/*
 * A fake protocol whose packets are the message body as plain text: receive
 * files the buffer as-is from a fixed sender, transmit copies the body.
 */
static size_t rxCalls = 0;
static size_t txCalls = 0;
static int txResult = 0;
static std::string lastRx;

static int fakeProcessRx(const struct pktDesc *pkt)
{
    rxCalls++;
    lastRx.assign(static_cast<const char *>(pkt->buffer), pkt->res);

    struct message msg = {};
    msg.body = lastRx.c_str();
    msg.bodyLen = lastRx.size();
    msg.mode = MODE_A;
    msg.direction = MSG_DIR_RX;
    msg.status = MSG_STATUS_RECEIVED;
    msg.unread = 1;
    strcpy(msg.sender, "W1AW");

    int32_t seq = messages_store(&msg);
    return (seq < 0) ? seq : 0;
}

static int fakeFormatTx(const struct message *msg, struct pktDesc *pkt)
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

static int failRx(const struct pktDesc *pkt)
{
    (void)pkt;
    return -EIO;
}

static int failTx(const struct message *msg, struct pktDesc *pkt)
{
    (void)msg;
    (void)pkt;
    return -EIO;
}

static size_t taskCalls = 0;

static void countTask(void)
{
    taskCalls++;
}

static const struct messageOps fakeOps = { fakeProcessRx, fakeFormatTx, "Fake",
                                           MODE_A };
static const struct messageOps otherOps = { failRx, failTx, "Other", MODE_B };
static const struct messageOps taskOps = { failRx, failTx, "Tasked",
                                           MODE_B, NULL,   countTask };

static void setup()
{
    rtxStub_reset();
    messages_init();
    rxCalls = 0;
    txCalls = 0;
    txResult = 0;
    taskCalls = 0;
    lastRx.clear();
}

/* Register the fake source and run the task once in its mode, so that the
 * storage is allocated and one receive descriptor is submitted. */
static void activate()
{
    setup();
    REQUIRE(messages_registerSource(&fakeOps) == 0);
    messages_task(MODE_A);
    REQUIRE(messages_canCompose(MODE_A));
}

/* Build a store() template for a received message. */
static struct message rxMessage(const char *sender, const char *body)
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

/* Store a message expecting success and return the assigned sequence. */
static uint32_t storeOk(const struct message *msg)
{
    int32_t seq = messages_store(msg);
    REQUIRE(seq > 0);
    return (uint32_t)seq;
}

/* Build a send() template for an outgoing message. */
static struct message txMessage(const char *recipient, const char *body,
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
static struct message sendingMessage(const char *recipient, const char *body)
{
    struct message msg = txMessage(recipient, body);

    msg.direction = MSG_DIR_TX;
    msg.status = MSG_STATUS_SENDING;

    return msg;
}

/* Complete a submitted receive descriptor with the given packet bytes, as
 * an opmode does: the length goes in res, size is left as submitted. */
static void deliver(struct pktDesc *desc, const char *text)
{
    REQUIRE(desc != nullptr);
    REQUIRE(desc->status == PKT_STATUS_SUBMITTED);
    size_t len = strlen(text);
    REQUIRE(len <= desc->size);
    memcpy(desc->buffer, text, len);
    desc->res = len;
    desc->status = PKT_STATUS_DONE;
}

/* Hand a submitted descriptor back with an error, as an opmode does when it
 * is disabled. */
static void cancel(struct pktDesc *desc)
{
    REQUIRE(desc != nullptr);
    REQUIRE(desc->status == PKT_STATUS_SUBMITTED);
    desc->res = -ECANCELED;
    desc->status = PKT_STATUS_ERROR;
}

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
    CHECK(messages_store(&msg) == -ENODEV);

    /* Running the task in a mode without a source changes nothing. */
    CHECK(messages_task(MODE_A) == 0);
    CHECK(messages_store(&msg) == -ENODEV);
    CHECK(rtxStub_rxSubmissions() == 0);

    /* Registering a source is not enough either, the task must run. */
    REQUIRE(messages_registerSource(&fakeOps) == 0);
    CHECK_FALSE(messages_canCompose(MODE_A));
    CHECK(messages_store(&msg) == -ENODEV);

    CHECK(messages_task(MODE_B) == 0);
    CHECK_FALSE(messages_canCompose(MODE_A));

    CHECK(messages_task(MODE_A) == 0);
    CHECK(messages_canCompose(MODE_A));
    CHECK_FALSE(messages_canCompose(MODE_B));
    CHECK(messages_store(&msg) > 0);
    CHECK(messages_count() == 1);
}

TEST_CASE("messages: storage is released when leaving the mode", "[messages]")
{
    activate();

    struct message msg = rxMessage("W1AW", "a");
    REQUIRE(messages_store(&msg) > 0);
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
    CHECK(messages_store(&msg) == -ENODEV);
    CHECK(rtxStub_rxSubmissions() == 1);

    /* Coming back allocates fresh storage and rearms the receive slot. */
    CHECK(messages_task(MODE_A) == 0);
    CHECK(messages_canCompose(MODE_A));
    CHECK(messages_count() == 0);
    CHECK(rtxStub_rxSubmissions() == 2);
    CHECK(messages_store(&msg) > 0);
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

    struct message msg = rxMessage("W1AW", "a");
    uint32_t first = storeOk(&msg);

    cancel(rtxStub_rxDesc());
    messages_task(MODE_NONE);
    messages_task(MODE_A);

    uint32_t second = storeOk(&msg);
    CHECK(second > first);
    CHECK(messages_findBySequence(first) == SIZE_MAX);
}

TEST_CASE("messages: init drops sources and stored entries", "[messages]")
{
    activate();

    struct message msg = rxMessage("W1AW", "a");
    REQUIRE(messages_store(&msg) > 0);
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
    uint32_t seq = storeOk(&msg);

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

    CHECK(messages_store(nullptr) == -EINVAL);

    struct message msg = rxMessage("W1AW", "");
    msg.body = nullptr;
    msg.bodyLen = 1;
    CHECK(messages_store(&msg) == -EINVAL);

    msg.body = "x";
    msg.bodyLen = MSG_BODY_MAX_LEN;
    CHECK(messages_store(&msg) == -EMSGSIZE);

    CHECK(messages_count() == 0);
}

TEST_CASE("messages: empty bodies are allowed", "[messages]")
{
    activate();

    struct message msg = rxMessage("W1AW", "");
    msg.body = nullptr;
    msg.bodyLen = 0;

    REQUIRE(messages_store(&msg) > 0);
    REQUIRE(messages_get(0) != nullptr);
    CHECK(messages_get(0)->body[0] == '\0');
}

TEST_CASE("messages: entries are listed newest first", "[messages]")
{
    activate();

    struct message msg = rxMessage("W1AW", "one");
    uint32_t first = storeOk(&msg);
    msg = rxMessage("W1AW", "two");
    uint32_t second = storeOk(&msg);
    msg = rxMessage("W1AW", "three");
    uint32_t third = storeOk(&msg);

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
        s = storeOk(&msg);
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
    REQUIRE(messages_store(&msg) > 0);
    REQUIRE(messages_store(&msg) > 0);

    /* Sent messages and pre-read ones are not arrivals. */
    struct message tx = sendingMessage("W1AW", "b");
    REQUIRE(messages_store(&tx) > 0);
    msg.unread = 0;
    REQUIRE(messages_store(&msg) > 0);

    CHECK(messages_task(MODE_A) == 2);
    CHECK(messages_task(MODE_A) == 0);
    CHECK(messages_countUnread() == 2);
}

TEST_CASE("messages: markRead toggles the unread flag", "[messages]")
{
    activate();

    struct message msg = rxMessage("W1AW", "a");
    REQUIRE(messages_store(&msg) > 0);

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
    uint32_t seq = storeOk(&tx);
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
        REQUIRE(messages_store(&msg) > 0);
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

    struct message msg = rxMessage("W1AW", "first");
    uint32_t oldest = storeOk(&msg);

    for (size_t i = 1; i < CONFIG_MESSAGES_MAX_ENTRIES; i++) {
        msg = rxMessage("W1AW", "filler");
        REQUIRE(messages_store(&msg) > 0);
    }
    REQUIRE(messages_count() == CONFIG_MESSAGES_MAX_ENTRIES);
    CHECK(messages_findBySequence(oldest) != SIZE_MAX);

    msg = rxMessage("W1AW", "last");
    uint32_t newest = storeOk(&msg);

    CHECK(messages_count() == CONFIG_MESSAGES_MAX_ENTRIES);
    CHECK(messages_findBySequence(oldest) == SIZE_MAX);
    CHECK(messages_get(0)->sequence == newest);
    CHECK(std::string(messages_get(messages_count() - 1)->body) == "filler");
}

TEST_CASE("messages: a message still being sent is not evicted", "[messages]")
{
    activate();

    struct message msg = rxMessage("W1AW", "older");
    uint32_t older = storeOk(&msg);

    msg = rxMessage("W1AW", "outgoing");
    msg.direction = MSG_DIR_TX;
    msg.status = MSG_STATUS_SENDING;
    uint32_t sending = storeOk(&msg);

    for (size_t i = 2; i < CONFIG_MESSAGES_MAX_ENTRIES; i++) {
        msg = rxMessage("W1AW", "filler");
        REQUIRE(messages_store(&msg) > 0);
    }
    REQUIRE(messages_count() == CONFIG_MESSAGES_MAX_ENTRIES);

    /* The oldest entry goes first, as usual. */
    msg = rxMessage("W1AW", "one more");
    storeOk(&msg);
    CHECK(messages_findBySequence(older) == SIZE_MAX);
    CHECK(messages_findBySequence(sending) != SIZE_MAX);

    /* Now the in-flight message is the oldest, and is skipped over. */
    msg = rxMessage("W1AW", "and another");
    storeOk(&msg);
    CHECK(messages_findBySequence(sending) != SIZE_MAX);
    CHECK(messages_count() == CONFIG_MESSAGES_MAX_ENTRIES);
    CHECK(std::string(messages_get(messages_count() - 1)->body) == "outgoing");

    /* Once sent it is ordinary again. */
    messages_setStatus(sending, MSG_STATUS_SENT);
    msg = rxMessage("W1AW", "last");
    storeOk(&msg);
    CHECK(messages_findBySequence(sending) == SIZE_MAX);
}

TEST_CASE("messages: a long text splits into pieces on word boundaries",
          "[messages]")
{
    const size_t LIMIT = 67;
    auto pieces = [&](const std::string &text) {
        std::vector<std::string> out;
        const char *p = text.c_str();
        size_t left = text.size();
        size_t skip = 0;
        size_t n;
        while ((n = messages_split(p, left, LIMIT, &skip)) > 0) {
            out.emplace_back(p + skip, n);
            p += skip + n;
            left -= skip + n;
        }
        return out;
    };

    SECTION("short text is one piece")
    {
        auto out = pieces("hello world");
        REQUIRE(out.size() == 1);
        CHECK(out[0] == "hello world");
        CHECK(pieces("").empty());
    }

    SECTION("exactly the limit is one piece")
    {
        std::string t(LIMIT, 'x');
        auto out = pieces(t);
        REQUIRE(out.size() == 1);
        CHECK(out[0] == t);
    }

    SECTION("the cut lands on a space, which neither piece keeps")
    {
        std::string head(60, 'a');
        std::string t = head + " seventeenletters tail";
        auto out = pieces(t);
        REQUIRE(out.size() == 2);
        CHECK(out[0] == head);
        CHECK(out[1] == "seventeenletters tail");
    }

    SECTION("a word longer than the limit is cut at the limit")
    {
        std::string t(LIMIT + 10, 'z');
        auto out = pieces(t);
        REQUIRE(out.size() == 2);
        CHECK(out[0].size() == LIMIT);
        CHECK(out[1].size() == 10);
    }

    SECTION("many pieces in order, nothing lost but the cut spaces")
    {
        std::string t;
        for (int i = 0; i < 40; i++)
            t += "word" + std::to_string(i) + " ";
        auto out = pieces(t);
        REQUIRE(out.size() > 2);
        std::string joined;
        for (const auto &piece : out) {
            CHECK(piece.size() <= LIMIT);
            CHECK(piece.front() != ' ');
            joined += piece + " ";
        }
        CHECK(joined == t);
    }
}

TEST_CASE("messages: sending a long text queues one message per piece",
          "[messages]")
{
    activate();

    /* The fake source has no limit of its own: the store's applies. */
    CHECK(messages_pieceLimit(MODE_A) == MSG_BODY_MAX_LEN - 1);
    CHECK(messages_pieceLimit(MODE_B) == 0);

    std::string t;
    while (t.size() < 2 * MSG_BODY_MAX_LEN + 200)
        t += "lorem ipsum ";

    struct message msg = txMessage("W1AW", t.c_str());
    REQUIRE(messages_sendSplit(&msg) == 0);
    REQUIRE(messages_count() == 3);
    CHECK(std::string(messages_get(2)->body).rfind("lorem ipsum", 0) == 0);
    CHECK(messages_get(2)->bodyLen <= MSG_BODY_MAX_LEN - 1);
    CHECK(messages_get(0)->sequence > messages_get(2)->sequence);

    /* The cursor's piece follows the cuts. */
    size_t current = 0;
    CHECK(messages_pieces(MODE_A, t.c_str(), t.size(), 0, &current) == 3);
    CHECK(current == 1);
    messages_pieces(MODE_A, t.c_str(), t.size(), t.size() - 1, &current);
    CHECK(current == 3);
    CHECK(messages_pieces(MODE_A, "", 0, 0, &current) == 1);
    CHECK(current == 1);
}

TEST_CASE("messages: delete stays consistent after the table wraps",
          "[messages]")
{
    activate();

    /* Overfill the table so the ring head has moved past the start. */
    for (size_t i = 0; i < CONFIG_MESSAGES_MAX_ENTRIES + 2; i++) {
        std::string body = std::to_string(i);
        struct message msg = rxMessage("W1AW", body.c_str());
        storeOk(&msg);
    }
    REQUIRE(messages_count() == CONFIG_MESSAGES_MAX_ENTRIES);

    /* Deleting a middle entry shifts entries across the array boundary. */
    REQUIRE(messages_delete(1) == 0);
    REQUIRE(messages_count() == CONFIG_MESSAGES_MAX_ENTRIES - 1);
    CHECK(std::string(messages_get(0)->body)
          == std::to_string(CONFIG_MESSAGES_MAX_ENTRIES + 1));
    CHECK(std::string(messages_get(1)->body)
          == std::to_string(CONFIG_MESSAGES_MAX_ENTRIES - 1));

    /* The remaining entries are still ordered newest first. */
    for (size_t i = 1; i < messages_count(); i++)
        CHECK(messages_get(i)->sequence < messages_get(i - 1)->sequence);
}

TEST_CASE("messages: a full body pool evicts the oldest entries", "[messages]")
{
    activate();

    /* Bodies of maximum length fill the pool after a few stores. */
    std::string big(MSG_BODY_MAX_LEN - 1, 'a');
    const size_t perPool = CONFIG_MESSAGES_POOL_BYTES / MSG_BODY_MAX_LEN;
    REQUIRE(perPool >= 1);

    struct message msg = rxMessage("W1AW", big.c_str());
    uint32_t oldest = storeOk(&msg);

    for (size_t i = 1; i < perPool; i++) {
        REQUIRE(messages_store(&msg) > 0);
    }
    CHECK(messages_count() == perPool);
    CHECK(messages_findBySequence(oldest) != SIZE_MAX);

    /* One more wraps the pool and lands on the oldest body. */
    std::string different(MSG_BODY_MAX_LEN - 1, 'b');
    msg = rxMessage("W1AW", different.c_str());
    REQUIRE(messages_store(&msg) > 0);

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

TEST_CASE("messages: eviction stays oldest-first across a pool wrap",
          "[messages]")
{
    activate();

    /*
     * As many full bodies as fit, then a short one filling the slack at
     * the end of the pool. The next round of full bodies wraps and
     * overwrites the first round one by one; the short body, now the
     * oldest, survives at the end. One more full body wraps again onto the
     * first of the second round: strictly oldest-first eviction must drop
     * the short body before it, even though they do not overlap.
     */
    const size_t perPool = CONFIG_MESSAGES_POOL_BYTES / MSG_BODY_MAX_LEN;
    const size_t slack = CONFIG_MESSAGES_POOL_BYTES
                       - perPool * MSG_BODY_MAX_LEN;
    REQUIRE(perPool >= 1);
    REQUIRE(slack >= 2);
    REQUIRE(perPool + 1 <= CONFIG_MESSAGES_MAX_ENTRIES);

    std::string full(MSG_BODY_MAX_LEN - 1, 'a');
    std::string chunk(slack - 1, 'b');
    std::vector<uint32_t> seq;

    for (size_t i = 0; i < 2 * perPool + 2; i++) {
        struct message msg = rxMessage("W1AW", (i == perPool) ? chunk.c_str() :
                                                                full.c_str());
        seq.push_back(storeOk(&msg));
    }

    /* The chunk and the first body of the second round are gone. */
    REQUIRE(messages_count() == perPool);
    CHECK(messages_findBySequence(seq[perPool]) == SIZE_MAX);
    CHECK(messages_findBySequence(seq[perPool + 1]) == SIZE_MAX);

    /* What remains is the rest of the second round plus the last one. */
    for (size_t i = 0; i < messages_count(); i++) {
        CHECK(messages_get(i)->sequence == seq[seq.size() - 1 - i]);
        CHECK(std::string(messages_get(i)->body) == full);
    }
}

TEST_CASE("messages: terminate keeps storage the rtx stage still references",
          "[messages]")
{
    activate();

    struct message msg = rxMessage("W1AW", "a");
    REQUIRE(messages_store(&msg) > 0);
    struct pktDesc *desc = rtxStub_rxDesc();

    messages_terminate();
    CHECK(messages_count() == 1);
    CHECK(desc->status == PKT_STATUS_SUBMITTED);

    cancel(desc);
    messages_terminate();
    CHECK(messages_count() == 0);
}

TEST_CASE("messages: addresses are always NUL-terminated", "[messages]")
{
    activate();

    struct message msg = rxMessage("W1AW", "a");
    memset(msg.sender, 'A', sizeof(msg.sender));
    memset(msg.recipient, 'B', sizeof(msg.recipient));
    REQUIRE(messages_store(&msg) > 0);

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

TEST_CASE("messages: broadcast address comes from the source", "[messages]")
{
    setup();

    struct messageOps withBroadcast = fakeOps;
    withBroadcast.broadcastAddr = "EVERYONE";
    REQUIRE(messages_registerSource(&withBroadcast) == 0);
    REQUIRE(messages_registerSource(&otherOps) == 0);

    /* Answered from the registration alone, active inbox or not. */
    REQUIRE(messages_broadcastAddr(MODE_A) != nullptr);
    CHECK(std::string(messages_broadcastAddr(MODE_A)) == "EVERYONE");
    CHECK(messages_broadcastAddr(MODE_B) == nullptr);
    CHECK(messages_broadcastAddr(MODE_NONE) == nullptr);
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

    REQUIRE(messages_count() == 2);
    CHECK(messages_get(0)->status == MSG_STATUS_FAILED);
    CHECK(messages_get(1)->status == MSG_STATUS_FAILED);
}

TEST_CASE("messages: a message waits for a busy transmitter", "[messages]")
{
    activate();

    struct message msg = txMessage("W1AW", "first");
    REQUIRE(messages_send(&msg) == 0);
    struct pktDesc *desc = rtxStub_txDesc();

    /* The inbox slot is taken by the first message. */
    msg = txMessage("W1AW", "second");
    CHECK(messages_send(&msg) == 0);
    CHECK(rtxStub_txSubmissions() == 1);
    CHECK(messages_get(0)->status == MSG_STATUS_SENDING);

    desc->status = PKT_STATUS_DONE;
    messages_task(MODE_A);
    CHECK(messages_get(1)->status == MSG_STATUS_SENT);
    CHECK(rtxStub_txSubmissions() == 2);
    CHECK(memcmp(desc->buffer, "second", 6) == 0);

    /* The opmode is busy with something else, e.g. a beacon. */
    desc->status = PKT_STATUS_DONE;
    messages_task(MODE_A);
    rtxStub_setTxResult(-EBUSY);
    msg = txMessage("W1AW", "third");
    CHECK(messages_send(&msg) == 0);
    messages_task(MODE_A);
    CHECK(messages_get(0)->status == MSG_STATUS_SENDING);
    CHECK(rtxStub_txSubmissions() == 2);

    rtxStub_setTxResult(0);
    messages_task(MODE_A);
    CHECK(rtxStub_txSubmissions() == 3);
    CHECK(memcmp(desc->buffer, "third", 5) == 0);
}

TEST_CASE("messages: send is refused while the inbox is inactive", "[messages]")
{
    setup();
    REQUIRE(messages_registerSource(&fakeOps) == 0);

    struct message msg = txMessage("W1AW", "outgoing");
    CHECK(messages_send(&msg) == -ENODEV);
    CHECK(txCalls == 0);
}

/*
 * Source-driven transmission
 */

TEST_CASE("messages: a source task runs only in its mode", "[messages]")
{
    activate();
    REQUIRE(messages_registerSource(&taskOps) == 0);

    messages_task(MODE_A);
    CHECK(taskCalls == 0);

    messages_task(MODE_B);
    messages_task(MODE_B);
    CHECK(taskCalls == 2);
}

TEST_CASE("messages: a source can transmit a packet of its own", "[messages]")
{
    activate();

    CHECK(messages_sendPacket(MODE_A, nullptr, 3) == -EINVAL);
    CHECK(messages_sendPacket(MODE_A, "ack", 0) == -EINVAL);
    std::vector<uint8_t> big(MSG_PKT_MAX_SIZE + 1, 'x');
    CHECK(messages_sendPacket(MODE_A, big.data(), big.size()) == -EMSGSIZE);

    REQUIRE(messages_sendPacket(MODE_A, "ack", 3) == 0);
    struct pktDesc *desc = rtxStub_txDesc();
    CHECK(desc->size == 3);
    CHECK(memcmp(desc->buffer, "ack", 3) == 0);
    CHECK(messages_sendPacket(MODE_A, "ack", 3) == -EBUSY);

    /* Completion belongs to no entry and leaves the slot free. */
    desc->status = PKT_STATUS_DONE;
    messages_task(MODE_A);
    CHECK(messages_count() == 0);
    CHECK(messages_sendPacket(MODE_A, "ack", 3) == 0);
}

TEST_CASE("messages: resend leaves the entry status alone", "[messages]")
{
    activate();

    struct message msg = txMessage("W1AW", "again");
    REQUIRE(messages_send(&msg) == 0);
    struct pktDesc *desc = rtxStub_txDesc();
    desc->status = PKT_STATUS_DONE;
    messages_task(MODE_A);
    uint32_t seq = messages_get(0)->sequence;
    REQUIRE(messages_get(0)->status == MSG_STATUS_SENT);

    REQUIRE(messages_resend(seq) == 0);
    CHECK(rtxStub_txSubmissions() == 2);
    CHECK(memcmp(desc->buffer, "again", 5) == 0);
    CHECK(messages_resend(seq) == -EBUSY);

    desc->res = -EIO;
    desc->status = PKT_STATUS_ERROR;
    messages_task(MODE_A);
    CHECK(messages_get(0)->status == MSG_STATUS_SENT);

    CHECK(messages_resend(seq + 100) == -ENOENT);
    struct message rx = rxMessage("W1AW", "hello");
    uint32_t rxSeq = storeOk(&rx);
    CHECK(messages_resend(rxSeq) == -EINVAL);
}

TEST_CASE("messages: completion does not overwrite a settled status",
          "[messages]")
{
    activate();

    struct message msg = txMessage("W1AW", "quick");
    REQUIRE(messages_send(&msg) == 0);
    uint32_t seq = messages_get(0)->sequence;

    /* The source settles the entry before the transmission completes. */
    REQUIRE(messages_setStatus(seq, MSG_STATUS_FAILED) == 0);
    rtxStub_txDesc()->status = PKT_STATUS_DONE;
    messages_task(MODE_A);
    CHECK(messages_get(0)->status == MSG_STATUS_FAILED);
}

/*
 * Guests: a source taking another mode's packets
 */

static size_t guestRxCalls = 0;
static size_t guestTaskCalls = 0;
static uint8_t guestMode = 0;

/* MODE_B's own protocol: packets starting with 'B'. */
static int pickyRx(const struct pktDesc *pkt)
{
    const char *data = static_cast<const char *>(pkt->buffer);
    return (data[0] == 'B') ? 0 : -EPROTO;
}

/* The guest carries packets starting with 'G' in any mode. */
static int guestRx(uint8_t mode, const struct pktDesc *pkt)
{
    const char *data = static_cast<const char *>(pkt->buffer);
    if (data[0] != 'G')
        return -EPROTO;
    guestRxCalls++;
    guestMode = mode;
    return 0;
}

static void guestTick(uint8_t mode)
{
    guestTaskCalls++;
    guestMode = mode;
}

static const struct messageOps pickyOps = { pickyRx, failTx, "Picky", MODE_B };
static const struct messageOps guestOps = {
    fakeProcessRx, fakeFormatTx, "Guest", MODE_A, nullptr, nullptr, 0,
    guestRx,       guestTick,
};

TEST_CASE("messages: a packet its mode's source rejects is offered to guests",
          "[messages]")
{
    setup();
    guestRxCalls = 0;
    guestTaskCalls = 0;
    REQUIRE(messages_registerSource(&guestOps) == 0);
    REQUIRE(messages_registerSource(&pickyOps) == 0);

    /* The guest's task runs while MODE_B is current, not in its own mode */
    messages_task(MODE_B);
    CHECK(guestTaskCalls == 1);
    CHECK(guestMode == MODE_B);
    messages_task(MODE_A);
    CHECK(guestTaskCalls == 1);
    messages_task(MODE_B);

    struct pktDesc *desc = rtxStub_rxDesc();
    deliver(desc, "G from afar");
    messages_task(MODE_B);
    CHECK(guestRxCalls == 1);
    CHECK(guestMode == MODE_B);

    /* MODE_B's own packets stay with it */
    deliver(desc, "B native");
    messages_task(MODE_B);
    CHECK(guestRxCalls == 1);

    /* Nobody's: dropped, and the slot goes back for the next */
    deliver(desc, "X unknown");
    messages_task(MODE_B);
    CHECK(guestRxCalls == 1);
    CHECK(desc->status == PKT_STATUS_SUBMITTED);
}

TEST_CASE("messages: a waiting message only goes out in its own mode",
          "[messages]")
{
    setup();
    REQUIRE(messages_registerSource(&fakeOps) == 0);
    REQUIRE(messages_registerSource(&otherOps) == 0);
    messages_task(MODE_A);

    /* Queued behind a busy transmitter */
    rtxStub_setTxResult(-EBUSY);
    struct message msg = txMessage("W1AW", "later");
    REQUIRE(messages_send(&msg) == 0);
    rtxStub_setTxResult(0);
    size_t before = rtxStub_txSubmissions();

    /* Another mode's opmode must not be handed its packet */
    messages_task(MODE_B);
    CHECK(rtxStub_txSubmissions() == before);
    CHECK(messages_get(0)->status == MSG_STATUS_SENDING);

    messages_task(MODE_A);
    CHECK(rtxStub_txSubmissions() == before + 1);
}

TEST_CASE("messages: a message with a carrier goes out in the carrier's mode",
          "[messages]")
{
    setup();
    REQUIRE(messages_registerSource(&fakeOps) == 0);
    REQUIRE(messages_registerSource(&otherOps) == 0);
    messages_task(MODE_B);

    /* MODE_A's protocol, carried in MODE_B, queued behind a busy slot */
    rtxStub_setTxResult(-EBUSY);
    struct message msg = txMessage("W1AW", "carried");
    msg.carrier = MODE_B;
    REQUIRE(messages_send(&msg) == 0);
    CHECK(messages_travelMode(messages_get(0)) == MODE_B);
    rtxStub_setTxResult(0);
    size_t before = rtxStub_txSubmissions();

    messages_task(MODE_A);
    CHECK(rtxStub_txSubmissions() == before);

    messages_task(MODE_B);
    CHECK(rtxStub_txSubmissions() == before + 1);

    struct message plain = txMessage("W1AW", "own");
    CHECK(messages_travelMode(&plain) == MODE_A);
}
