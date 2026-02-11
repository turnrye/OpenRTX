/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <cstdio>
#include <cstdint>
#include <random>
#include "protocols/M17/M17FrameEncoder.hpp"
#include "protocols/M17/M17FrameDecoder.hpp"
#include "protocols/M17/M17PacketFrame.hpp"

using namespace std;
using namespace M17;

default_random_engine rng;

/**
 * Test encoding and decoding of packet frames
 */
int main()
{
    printf("M17 Packet Frame Encoder/Decoder Test\n");
    printf("======================================\n\n");

    M17FrameEncoder encoder;
    M17FrameDecoder decoder;

    uniform_int_distribution<uint8_t> rndByte(0, 255);
    uniform_int_distribution<uint8_t> rndCounter(0, 31);
    uniform_int_distribution<uint8_t> rndByteCount(1, 25);

    int passed = 0;
    int failed = 0;

    // Test 1: Encode and decode non-final packet frames
    printf("Test 1: Non-final packet frames (EOF=0)\n");
    for(int i = 0; i < 100; i++)
    {
        payload_t payload;
        frame_t encoded;
        
        // Generate random payload
        for(size_t j = 0; j < payload.size(); j++)
        {
            payload[j] = rndByte(rng);
        }
        
        uint8_t frameCounter = rndCounter(rng);
        
        // Encode packet frame
        encoder.reset();
        encoder.encodePacketFrame(payload, encoded, frameCounter, false);
        
        // Decode packet frame
        decoder.reset();
        M17FrameType type = decoder.decodeFrame(encoded);
        
        // Verify frame type
        if(type != M17FrameType::PACKET)
        {
            printf("  Test %d FAILED: Wrong frame type detected (%d)\n", i, (int)type);
            failed++;
            continue;
        }
        
        const M17PacketFrame& decoded_frame = decoder.getPacketFrame();
        
        // Verify EOF bit is not set
        if(decoded_frame.getEofBit())
        {
            printf("  Test %d FAILED: EOF bit should be 0\n", i);
            failed++;
            continue;
        }
        
        // Verify frame counter
        if(decoded_frame.getCounter() != frameCounter)
        {
            printf("  Test %d FAILED: Counter mismatch (expected %d, got %d)\n", 
                   i, frameCounter, decoded_frame.getCounter());
            failed++;
            continue;
        }
        
        // Verify payload
        bool payload_match = true;
        for(size_t j = 0; j < payload.size(); j++)
        {
            if(decoded_frame.payload()[j] != payload[j])
            {
                payload_match = false;
                break;
            }
        }
        
        if(!payload_match)
        {
            printf("  Test %d FAILED: Payload mismatch\n", i);
            failed++;
            continue;
        }
        
        passed++;
    }
    printf("  Passed: %d, Failed: %d\n\n", passed, failed);

    // Test 2: Encode and decode final packet frames
    printf("Test 2: Final packet frames (EOF=1)\n");
    int test2_passed = 0;
    int test2_failed = 0;
    
    for(int i = 0; i < 100; i++)
    {
        payload_t payload;
        frame_t encoded;
        
        // Generate random payload
        for(size_t j = 0; j < payload.size(); j++)
        {
            payload[j] = rndByte(rng);
        }
        
        uint8_t byteCount = rndByteCount(rng);
        
        // Encode packet frame
        encoder.reset();
        encoder.encodePacketFrame(payload, encoded, byteCount, true);
        
        // Decode packet frame
        decoder.reset();
        M17FrameType type = decoder.decodeFrame(encoded);
        
        // Verify frame type
        if(type != M17FrameType::PACKET)
        {
            printf("  Test %d FAILED: Wrong frame type detected (%d)\n", i, (int)type);
            test2_failed++;
            continue;
        }
        
        const M17PacketFrame& decoded_frame = decoder.getPacketFrame();
        
        // Verify EOF bit is set
        if(!decoded_frame.getEofBit())
        {
            printf("  Test %d FAILED: EOF bit should be 1\n", i);
            test2_failed++;
            continue;
        }
        
        // Verify byte counter
        if(decoded_frame.getCounter() != byteCount)
        {
            printf("  Test %d FAILED: Byte count mismatch (expected %d, got %d)\n", 
                   i, byteCount, decoded_frame.getCounter());
            test2_failed++;
            continue;
        }
        
        // Verify payload (only first byteCount bytes are valid)
        bool payload_match = true;
        for(size_t j = 0; j < byteCount; j++)
        {
            if(decoded_frame.payload()[j] != payload[j])
            {
                payload_match = false;
                break;
            }
        }
        
        if(!payload_match)
        {
            printf("  Test %d FAILED: Payload mismatch\n", i);
            test2_failed++;
            continue;
        }
        
        test2_passed++;
    }
    printf("  Passed: %d, Failed: %d\n\n", test2_passed, test2_failed);

    passed += test2_passed;
    failed += test2_failed;

    // Test 3: Verify sync word detection
    printf("Test 3: Packet sync word detection\n");
    {
        payload_t payload = {0};
        frame_t encoded;
        
        encoder.reset();
        encoder.encodePacketFrame(payload, encoded, 0, false);
        
        // Check that first two bytes match PACKET_SYNC_WORD
        if(encoded[0] == PACKET_SYNC_WORD[0] && encoded[1] == PACKET_SYNC_WORD[1])
        {
            printf("  Sync word verification PASSED\n");
            passed++;
        }
        else
        {
            printf("  Sync word verification FAILED (got %02x%02x, expected %02x%02x)\n",
                   encoded[0], encoded[1], PACKET_SYNC_WORD[0], PACKET_SYNC_WORD[1]);
            failed++;
        }
    }
    printf("\n");

    // Summary
    printf("======================================\n");
    printf("Total Tests Passed: %d\n", passed);
    printf("Total Tests Failed: %d\n", failed);
    printf("======================================\n");

    return (failed == 0) ? 0 : -1;
}
