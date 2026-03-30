/*
 * Copyright (c) 2026, OpenAI
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebView/StorageSnapshot.h>

namespace IPC {

ErrorOr<void> encode(Encoder& encoder, WebView::StorageSnapshotEntry const& entry)
{
    TRY(encoder.encode(entry.key));
    TRY(encoder.encode(entry.value));
    return {};
}

ErrorOr<WebView::StorageSnapshotEntry> decode(Decoder& decoder)
{
    WebView::StorageSnapshotEntry entry;
    entry.key = TRY(decoder.decode<String>());
    entry.value = TRY(decoder.decode<String>());
    return entry;
}

}
