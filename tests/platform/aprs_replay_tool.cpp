/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * APRS baseband replay tool: counts the frames the receive chain decodes
 * with a good checksum from a recording.
 *
 * Build:   meson compile -C build_linux aprs_replay_tool
 * Run:     ./build_linux/aprs_replay_tool [-v] recording.raw
 *
 * The recording is signed 16-bit little endian, mono, 9600 samples/s. The
 * usual benchmark is the WA8LMF TNC Test CD, http://www.wa8lmf.net/TNCtest/
 * (track 1 is flat discriminator audio, track 2 the same de-emphasised).
 * The CD is not part of this repository and must stay off the air: fetch
 * TNC_Test_2.iso, extract the FLAC tracks and convert one with sox, where
 * "vol" sets the level reaching the demodulator:
 *
 *     sox track.flac -r 9600 -c 1 -b 16 -e signed -t raw track.raw \
 *         remix 1 vol 0.05
 *
 * For a reference count run Dire Wolf's "atest" on the same track.
 */

#include <cstdio>
#include <cstring>

#include "core/audio_stream.h"
#include "protocols/APRS/constants.h"
#include "protocols/APRS/Demodulator.hpp"

/**
 * Print an AX.25 address as CALL[-SSID].
 *
 * @param field: seven byte address field.
 */
static void printAddress(const uint8_t *field)
{
    for (size_t i = 0; i < 6; i++) {
        char c = field[i] >> 1;
        if (c != ' ')
            putchar(c);
    }

    uint8_t ssid = (field[6] >> 1) & 0x0F;
    if (ssid != 0)
        printf("-%u", ssid);
}

/**
 * Print a frame as a TNC2 monitor line: SOURCE>DEST,PATH:info.
 *
 * @param frame: AX.25 frame followed by its checksum.
 * @param len: frame length in bytes.
 */
static void printFrame(const uint8_t *frame, size_t len)
{
    if (len < 18)
        return;

    printAddress(&frame[7]);
    putchar('>');
    printAddress(&frame[0]);

    // Find the last digipeater that repeated the frame, it gets the '*'
    size_t end = 14;
    size_t heard = 0;
    while (((frame[end - 1] & 1) == 0) && ((end + 7) <= len)) {
        if (frame[end + 6] & 0x80)
            heard = end;
        end += 7;
    }

    for (size_t pos = 14; pos < end; pos += 7) {
        putchar(',');
        printAddress(&frame[pos]);
        if (pos == heard)
            putchar('*');
    }

    size_t pos = end;

    // Skip the control and protocol fields, leave out the checksum
    putchar(':');
    if ((pos + 4) <= len)
        fwrite(&frame[pos + 2], 1, len - pos - 4, stdout);
    putchar('\n');
}

int main(int argc, char *argv[])
{
    bool verbose = (argc == 3) && (strcmp(argv[1], "-v") == 0);
    if ((argc != 2) && !verbose) {
        puts("Usage: aprs_replay_tool [-v] recording.raw");
        puts("-v\t Print each decoded frame");
        return 1;
    }

    FILE *file = fopen(argv[argc - 1], "rb");
    if (file == NULL) {
        perror(argv[argc - 1]);
        return 1;
    }

    APRS::Demodulator demodulator;
    demodulator.init();

    int16_t samples[APRS_BUF_SIZE];
    dataBlock_t baseband = { samples, APRS_BUF_SIZE };
    size_t frames = 0;
    size_t oversize = 0;

    while (fread(samples, sizeof(int16_t), APRS_BUF_SIZE, file)
           == APRS_BUF_SIZE) {
        if (!demodulator.update(baseband))
            continue;

        uint8_t frame[APRS_PACLEN];
        ssize_t len = demodulator.getFrame(frame, sizeof(frame));
        if (len < 0) {
            oversize++;
            continue;
        }

        frames++;
        if (verbose)
            printFrame(frame, static_cast<size_t>(len));
    }

    demodulator.terminate();
    fclose(file);

    printf("frames %zu, oversize %zu\n", frames, oversize);
    return 0;
}
