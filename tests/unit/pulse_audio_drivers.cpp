/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <cstdio>

extern "C" {
#include "interfaces/audio.h"
#include "drivers/audio/discard_sink.h"
#include "drivers/audio/file_sink.h"
}

TEST_CASE("Discard sink starts and stops cleanly", "[audio][linux]")
{
    stream_sample_t buf[256];
    struct streamCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.buffer     = buf;
    ctx.bufSize    = 256;
    ctx.bufMode    = BUF_CIRC_DOUBLE;
    ctx.sampleRate = 8000;

    REQUIRE(discard_sink_audio_driver.start(0, NULL, &ctx) == 0);
    REQUIRE(ctx.running == 1);

    stream_sample_t *idle = nullptr;
    int ret = discard_sink_audio_driver.data(&ctx, &idle);
    REQUIRE(ret > 0);
    REQUIRE(idle != nullptr);

    discard_sink_audio_driver.terminate(&ctx);
    REQUIRE(ctx.running == 0);
}

TEST_CASE("Discard sink data returns correct half-buffer size", "[audio][linux]")
{
    stream_sample_t buf[512];
    struct streamCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.buffer     = buf;
    ctx.bufSize    = 512;
    ctx.bufMode    = BUF_CIRC_DOUBLE;
    ctx.sampleRate = 8000;

    REQUIRE(discard_sink_audio_driver.start(0, NULL, &ctx) == 0);

    stream_sample_t *idle = nullptr;
    int ret = discard_sink_audio_driver.data(&ctx, &idle);
    REQUIRE(ret == 256);
    REQUIRE(idle == buf);

    discard_sink_audio_driver.terminate(&ctx);
}

TEST_CASE("Discard sink data returns full buffer in linear mode", "[audio][linux]")
{
    stream_sample_t buf[512];
    struct streamCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.buffer     = buf;
    ctx.bufSize    = 512;
    ctx.bufMode    = BUF_LINEAR;
    ctx.sampleRate = 8000;

    REQUIRE(discard_sink_audio_driver.start(0, NULL, &ctx) == 0);

    stream_sample_t *idle = nullptr;
    int ret = discard_sink_audio_driver.data(&ctx, &idle);
    REQUIRE(ret == 512);
    REQUIRE(idle == buf);

    discard_sink_audio_driver.terminate(&ctx);
}

TEST_CASE("Discard sink sync does not crash", "[audio][linux]")
{
    stream_sample_t buf[256];
    struct streamCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.buffer     = buf;
    ctx.bufSize    = 256;
    ctx.bufMode    = BUF_CIRC_DOUBLE;
    ctx.sampleRate = 48000;  /* High sample rate for fast sync */

    REQUIRE(discard_sink_audio_driver.start(0, NULL, &ctx) == 0);

    int ret = discard_sink_audio_driver.sync(&ctx, 1);
    REQUIRE(ret == 0);

    discard_sink_audio_driver.terminate(&ctx);
}

TEST_CASE("Discard sink rejects double start", "[audio][linux]")
{
    stream_sample_t buf[256];
    struct streamCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.buffer     = buf;
    ctx.bufSize    = 256;
    ctx.bufMode    = BUF_CIRC_DOUBLE;
    ctx.sampleRate = 8000;

    REQUIRE(discard_sink_audio_driver.start(0, NULL, &ctx) == 0);
    REQUIRE(discard_sink_audio_driver.start(0, NULL, &ctx) < 0);

    discard_sink_audio_driver.terminate(&ctx);
}

TEST_CASE("Discard sink rejects NULL context", "[audio][linux]")
{
    REQUIRE(discard_sink_audio_driver.start(0, NULL, NULL) < 0);
}

TEST_CASE("File sink starts, writes data, and stops", "[audio][linux]")
{
    const char *path = "/tmp/file_sink_test.raw";
    remove(path);  // Ensure clean state for append-mode file
    stream_sample_t buf[256];
    struct streamCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.buffer     = buf;
    ctx.bufSize    = 256;
    ctx.bufMode    = BUF_CIRC_DOUBLE;
    ctx.sampleRate = 48000;

    REQUIRE(file_sink_audio_driver.start(0, path, &ctx) == 0);
    REQUIRE(ctx.running == 1);

    // First data() call: phase=0, so idle half is buffer+128
    stream_sample_t *idle = nullptr;
    int ret = file_sink_audio_driver.data(&ctx, &idle);
    REQUIRE(ret == 128);   // half-buffer for BUF_CIRC_DOUBLE
    REQUIRE(idle == buf + 128);

    for(int i = 0; i < ret; i++)
        idle[i] = (stream_sample_t)(i * 100);

    // Sync with dirty=1 to flush data to file
    REQUIRE(file_sink_audio_driver.sync(&ctx, 1) == 0);

    file_sink_audio_driver.stop(&ctx);
    REQUIRE(ctx.running == 0);

    // Verify the file contains the expected data
    FILE *fp = fopen(path, "rb");
    REQUIRE(fp != nullptr);

    stream_sample_t readback[128];
    size_t n = fread(readback, sizeof(stream_sample_t), 128, fp);
    fclose(fp);
    remove(path);

    REQUIRE(n == 128);
    for(int i = 0; i < 128; i++)
        REQUIRE(readback[i] == (stream_sample_t)(i * 100));
}

TEST_CASE("File sink sync with dirty=0 does not write", "[audio][linux]")
{
    const char *path = "/tmp/file_sink_nodirty_test.raw";
    remove(path);  // Ensure clean state for append-mode file
    stream_sample_t buf[256];
    struct streamCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.buffer     = buf;
    ctx.bufSize    = 256;
    ctx.bufMode    = BUF_CIRC_DOUBLE;
    ctx.sampleRate = 48000;

    REQUIRE(file_sink_audio_driver.start(0, path, &ctx) == 0);

    // Sync without dirty — nothing should be written
    REQUIRE(file_sink_audio_driver.sync(&ctx, 0) == 0);

    file_sink_audio_driver.stop(&ctx);

    // Verify the file is empty
    FILE *fp = fopen(path, "rb");
    REQUIRE(fp != nullptr);
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fclose(fp);
    remove(path);

    REQUIRE(size == 0);
}

TEST_CASE("File sink rejects NULL context", "[audio][linux]")
{
    REQUIRE(file_sink_audio_driver.start(0, "/tmp/null_ctx.raw", NULL) < 0);
}

TEST_CASE("File sink rejects double start", "[audio][linux]")
{
    const char *path = "/tmp/file_sink_double_start.raw";
    remove(path);  // Ensure clean state for append-mode file
    stream_sample_t buf[256];
    struct streamCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.buffer     = buf;
    ctx.bufSize    = 256;
    ctx.bufMode    = BUF_CIRC_DOUBLE;
    ctx.sampleRate = 8000;

    REQUIRE(file_sink_audio_driver.start(0, path, &ctx) == 0);
    REQUIRE(file_sink_audio_driver.start(0, path, &ctx) < 0);

    file_sink_audio_driver.terminate(&ctx);
    remove(path);
}

TEST_CASE("File sink double-buffer alternates halves", "[audio][linux]")
{
    const char *path = "/tmp/file_sink_alternate.raw";
    remove(path);  // Ensure clean state for append-mode file
    stream_sample_t buf[256];
    struct streamCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.buffer     = buf;
    ctx.bufSize    = 256;
    ctx.bufMode    = BUF_CIRC_DOUBLE;
    ctx.sampleRate = 48000;

    REQUIRE(file_sink_audio_driver.start(0, path, &ctx) == 0);

    // First data(): phase=0, idle half is buf+128
    stream_sample_t *idle1 = nullptr;
    int ret1 = file_sink_audio_driver.data(&ctx, &idle1);
    REQUIRE(ret1 == 128);
    REQUIRE(idle1 == buf + 128);
    for(int i = 0; i < ret1; i++)
        idle1[i] = (stream_sample_t)(1000 + i);

    // Sync toggles phase
    REQUIRE(file_sink_audio_driver.sync(&ctx, 1) == 0);

    // Second data(): phase=1, idle half is buf+0
    stream_sample_t *idle2 = nullptr;
    int ret2 = file_sink_audio_driver.data(&ctx, &idle2);
    REQUIRE(ret2 == 128);
    REQUIRE(idle2 == buf);
    for(int i = 0; i < ret2; i++)
        idle2[i] = (stream_sample_t)(2000 + i);

    REQUIRE(file_sink_audio_driver.sync(&ctx, 1) == 0);

    // Third data(): phase=0 again, idle half is buf+128
    stream_sample_t *idle3 = nullptr;
    int ret3 = file_sink_audio_driver.data(&ctx, &idle3);
    REQUIRE(ret3 == 128);
    REQUIRE(idle3 == buf + 128);

    file_sink_audio_driver.stop(&ctx);

    // Verify the file contains both frames in order
    FILE *fp = fopen(path, "rb");
    REQUIRE(fp != nullptr);

    stream_sample_t readback[256];
    size_t n = fread(readback, sizeof(stream_sample_t), 256, fp);
    fclose(fp);
    remove(path);

    REQUIRE(n == 256);
    for(int i = 0; i < 128; i++)
        REQUIRE(readback[i] == (stream_sample_t)(1000 + i));
    for(int i = 0; i < 128; i++)
        REQUIRE(readback[128 + i] == (stream_sample_t)(2000 + i));
}

#ifdef LINUX_AUDIO_PULSE
extern "C" {
#include "drivers/audio/pulse_sink.h"
#include "drivers/audio/pulse_source.h"
}

TEST_CASE("PulseAudio sink opens and closes", "[audio][linux][pulse][!mayfail]")
{
    stream_sample_t buf[1024];
    struct streamCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.buffer     = buf;
    ctx.bufSize    = 1024;
    ctx.bufMode    = BUF_CIRC_DOUBLE;
    ctx.sampleRate = 8000;

    int ret = pulse_sink_audio_driver.start(0, NULL, &ctx);
    if(ret == 0)
    {
        REQUIRE(ctx.running == 1);
        pulse_sink_audio_driver.terminate(&ctx);
        REQUIRE(ctx.running == 0);
    }
    else
    {
        WARN("PulseAudio not available, skipping sink test");
    }
}

TEST_CASE("PulseAudio source opens and closes", "[audio][linux][pulse][!mayfail]")
{
    stream_sample_t buf[1024];
    struct streamCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.buffer     = buf;
    ctx.bufSize    = 1024;
    ctx.bufMode    = BUF_CIRC_DOUBLE;
    ctx.sampleRate = 8000;

    int ret = pulse_source_audio_driver.start(0, NULL, &ctx);
    if(ret == 0)
    {
        REQUIRE(ctx.running == 1);
        pulse_source_audio_driver.terminate(&ctx);
        REQUIRE(ctx.running == 0);
    }
    else
    {
        WARN("PulseAudio not available, skipping source test");
    }
}
#endif
