/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * Unit tests for SpscQueue, the lock-free single-producer single-consumer
 * queue.
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <thread>
#include "core/spsc_queue.hpp"

TEST_CASE("SpscQueue: elements come out in order until empty", "[core][spsc]")
{
    SpscQueue<int, 4> queue;
    int elem = 0;

    REQUIRE(queue.empty());
    REQUIRE(queue.pop(elem) == false);

    for (int i = 0; i < 4; i++)
        REQUIRE(queue.push(i));

    REQUIRE(queue.size() == 4);
    REQUIRE(queue.push(4) == false);

    for (int i = 0; i < 4; i++) {
        REQUIRE(queue.pop(elem));
        REQUIRE(elem == i);
    }

    REQUIRE(queue.pop(elem) == false);
}

TEST_CASE("SpscQueue: indices wrap around the storage", "[core][spsc]")
{
    SpscQueue<int, 4> queue;
    int elem = 0;

    for (int i = 0; i < 1000; i++) {
        REQUIRE(queue.push(i));
        REQUIRE(queue.push(i + 1));
        REQUIRE(queue.pop(elem));
        REQUIRE(elem == i);
        REQUIRE(queue.pop(elem));
        REQUIRE(elem == i + 1);
    }

    REQUIRE(queue.empty());
}

TEST_CASE("SpscQueue: clear drops every element", "[core][spsc]")
{
    SpscQueue<int, 8> queue;
    int elem = 0;

    for (int i = 0; i < 5; i++)
        REQUIRE(queue.push(i));

    queue.clear();
    REQUIRE(queue.empty());
    REQUIRE(queue.pop(elem) == false);

    REQUIRE(queue.push(42));
    REQUIRE(queue.pop(elem));
    REQUIRE(elem == 42);
}

TEST_CASE("SpscQueue: a producer and a consumer thread lose nothing",
          "[core][spsc]")
{
    static constexpr uint32_t COUNT = 200000;
    SpscQueue<uint32_t, 16> queue;
    bool inOrder = true;
    uint32_t received = 0;

    std::thread consumer([&]() {
        uint32_t elem;
        while (received < COUNT) {
            if (queue.pop(elem) == false) {
                std::this_thread::yield();
                continue;
            }

            if (elem != received)
                inOrder = false;

            received++;
        }
    });

    for (uint32_t i = 0; i < COUNT; i++) {
        while (queue.push(i) == false)
            std::this_thread::yield();
    }

    consumer.join();
    REQUIRE(received == COUNT);
    REQUIRE(inOrder);
    REQUIRE(queue.empty());
}
