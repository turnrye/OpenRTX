/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>
#include "core/nvmem_access.h"
#include "core/data_slicer.h"
#include "core/cps.h"
#include "core/vfo.h"
#include "core/tuner.h"

/*
 * Tag of the stored VFO data. The top bits keep it different from
 * CPS_VERSION_NUMBER, which tagged the VFO when it was stored as a channel_t.
 */
#define VFO_DATA_TAG ((uint16_t)(0xF000 | TUNER_VERSION))

struct vfo_data {
    uint16_t version;
    struct tuner_core vfo;
} __attribute__((packed));

struct vfo_storage {
    uint32_t dev;
    uint32_t part;
    bool full_write_needed;
};

SLICED_STORAGE_DECLARE(slicedVfo, sizeof(struct vfo_data), 16)
static struct vfo_storage storage;

void vfo_init(const uint32_t nvm_dev, const uint32_t nvm_part)
{
    storage.dev = nvm_dev;
    storage.part = nvm_part;
    storage.full_write_needed = false;
}

int vfo_load(struct tuner_core *vfo)
{
    struct vfo_data saved_vfo;
    int err = 0;

    err = slicedStorage_nvmLoad(&slicedVfo, storage.dev, storage.part,
                                &saved_vfo);
    if (err)
        return err;

    if (saved_vfo.version != VFO_DATA_TAG) {
        storage.full_write_needed = true;
        return -1;
    }

    memcpy(vfo, &saved_vfo.vfo, sizeof(struct tuner_core));

    return 0;
}

int vfo_save(const struct tuner_core *vfo)
{
    const struct nvmInfo *ni = nvm_getDevInfo(storage.dev);
    struct vfo_data vfo_data;
    int ret = 0;

    vfo_data.version = VFO_DATA_TAG;
    vfo_data.vfo = *vfo;

    if (storage.full_write_needed && (ni->device_info & NVM_ERASE)) {
        ret = nvm_erasePartition(storage.dev, storage.part);
        if (ret < 0)
            return ret;
    }

    ret = slicedStorage_nvmStore(&slicedVfo, storage.dev, storage.part,
                                 &vfo_data, storage.full_write_needed);
    if (!ret)
        storage.full_write_needed = false;

    return ret;
}
