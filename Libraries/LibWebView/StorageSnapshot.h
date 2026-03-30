/*
 * Copyright (c) 2026, OpenAI
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibCore/Forward.h>
#include <LibIPC/Forward.h>

namespace WebView {

struct StorageSnapshotEntry {
    String key;
    String value;
};

}

namespace IPC {

ErrorOr<void> encode(Encoder&, WebView::StorageSnapshotEntry const&);
ErrorOr<WebView::StorageSnapshotEntry> decode(Decoder&);

}
