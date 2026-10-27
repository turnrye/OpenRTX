/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SPSC_QUEUE_H
#define SPSC_QUEUE_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include <atomic>
#include <cstddef>

/**
 * Lock-free queue between exactly one producer thread and one consumer
 * thread. Each side only writes its own index, so neither ever waits for,
 * or fails because of, the other.
 *
 * @tparam T: element type.
 * @tparam N: capacity, a power of two.
 */
template <typename T, size_t N>
class SpscQueue
{
public:
    /**
     * Constructor.
     */
    SpscQueue() : head(0), tail(0)
    {
    }

    /**
     * Destructor.
     */
    ~SpscQueue()
    {
    }

    /**
     * Add an element. To be called by the producer only.
     *
     * @param elem: element to add.
     * @return true on success, false if the queue is full.
     */
    bool push(const T &elem)
    {
        size_t h = head.load(std::memory_order_relaxed);
        if ((h - tail.load(std::memory_order_acquire)) == N)
            return false;

        data[h % N] = elem;
        head.store(h + 1, std::memory_order_release);
        return true;
    }

    /**
     * Remove the oldest element. To be called by the consumer only.
     *
     * @param elem: where to store the element.
     * @return true on success, false if the queue is empty.
     */
    bool pop(T &elem)
    {
        size_t t = tail.load(std::memory_order_relaxed);
        if (t == head.load(std::memory_order_acquire))
            return false;

        elem = data[t % N];
        tail.store(t + 1, std::memory_order_release);
        return true;
    }

    /**
     * Drop every element. To be called by the consumer only.
     */
    void clear()
    {
        tail.store(head.load(std::memory_order_acquire),
                   std::memory_order_release);
    }

    /**
     * Get the number of elements. Exact for the producer and the consumer;
     * any other caller gets a snapshot that may be momentarily high.
     *
     * @return number of elements in the queue.
     */
    size_t size() const
    {
        // Tail first: it never overtakes a head read afterwards.
        size_t t = tail.load(std::memory_order_acquire);
        size_t h = head.load(std::memory_order_acquire);
        return h - t;
    }

    /**
     * Check if the queue is empty, with the same caveat as size().
     *
     * @return true if the queue holds no element.
     */
    bool empty() const
    {
        return size() == 0;
    }

private:
    static_assert((N != 0) && ((N & (N - 1)) == 0),
                  "SpscQueue capacity must be a power of two");

    std::atomic<size_t> head; ///< Next slot to write, owned by the producer.
    std::atomic<size_t> tail; ///< Next slot to read, owned by the consumer.
    T data[N];                ///< Element storage.
};

#endif /* SPSC_QUEUE_H */
