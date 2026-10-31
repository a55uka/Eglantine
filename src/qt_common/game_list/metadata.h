// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace QtCommon::GameMetadata {

struct GameInfo {
    std::string name;
    std::string icon_url;
};

/**
 * Looks up a title's name and icon URL in the community title database. The database is
 * downloaded once and cached under the game list cache directory; failures leave the previous
 * cache in place and results are remembered until the next launch.
 *
 * @param title_id Title ID to look up
 *
 * @returns The game's info, or std::nullopt when it is unknown or the database is unavailable.
 */
std::optional<GameInfo> Lookup(u64 title_id);

/**
 * Downloads an icon image for a URL from the game database.
 *
 * @param url Image URL (HTTPS)
 *
 * @returns The image bytes, or an empty vector when the download failed.
 */
std::vector<u8> FetchIcon(const std::string& url);

} // namespace QtCommon::GameMetadata
