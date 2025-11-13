/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/M17/Metadata.hpp"

using namespace M17;

Metadata *MetadataFactory::create(M17LinkSetupFrame &lsf, void *storage)
{
    streamType_t streamType = lsf.getType();

    M17::MetadataType type =
        M17::MetadataFactory::getTypeFromStream(streamType);
    if (!storage)
        return nullptr;

    switch (type) {
        case MetadataType::EXTENDED_CALLSIGN:
            return new (storage) ExtendedCallsignMetadata(lsf.metadata());
        case MetadataType::TEXT:
            return new (storage) TextBlockMetadata(lsf.metadata());
        default:
            return nullptr;
    }
}

MetadataType MetadataFactory::getTypeFromStream(const streamType_t &streamType)
{
    if (streamType.fields.encType != M17_ENCRYPTION_NONE) {
        return MetadataType::NONE;
    }

    switch (streamType.fields.encSubType) {
        case M17_META_EXTD_CALLSIGN:
            return MetadataType::EXTENDED_CALLSIGN;
        case M17_META_TEXT:
            return MetadataType::TEXT;
        case M17_META_GNSS:
            return MetadataType::GNSS;
        default:
            return MetadataType::NONE;
    }
}
