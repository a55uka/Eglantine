// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "qt_common/game_list/metadata.h"

namespace QtCommon::GameMetadata {
namespace {

// Community title database (contains both Switch and Switch 2 titles, keyed by eShop ID, with
// the program's title ID in each record).
constexpr auto DbUrlHost = "raw.githubusercontent.com";
constexpr auto DbUrlPath = "/blawar/titledb/master/US.en.json";
constexpr auto DbRefreshInterval = std::chrono::hours{24 * 30};

std::filesystem::path DatabasePath() {
    // Outside the game_list cache directory so resetting the metadata cache keeps the
    // downloaded database around.
    return Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "title_db.tsv";
}

std::string Sanitize(std::string value) {
    std::replace_if(value.begin(), value.end(),
                    [](char c) { return c == '\t' || c == '\n' || c == '\r'; }, ' ');
    return value;
}

u64 ParseTitleId(const std::string& hex) {
    if (hex.size() != 16) {
        return 0;
    }
    u64 title_id = 0;
    for (const char c : hex) {
        title_id <<= 4;
        if (c >= '0' && c <= '9') {
            title_id |= static_cast<u64>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            title_id |= static_cast<u64>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            title_id |= static_cast<u64>(c - 'A' + 10);
        } else {
            return 0;
        }
    }
    return title_id;
}

// SAX handler pulling the fields we need out of the full database: a DOM parse of the ~90 MB
// source costs far too much memory, but each record is flat and only three strings matter.
class TitleDbFilter final {
public:
    using json = nlohmann::json;
    using number_integer_t = json::number_integer_t;
    using number_unsigned_t = json::number_unsigned_t;
    using number_float_t = json::number_float_t;
    using string_t = json::string_t;
    using binary_t = json::binary_t;

    bool null() {
        field = Field::None;
        return true;
    }
    bool boolean(bool) {
        field = Field::None;
        return true;
    }
    bool number_integer(number_integer_t) {
        field = Field::None;
        return true;
    }
    bool number_unsigned(number_unsigned_t) {
        field = Field::None;
        return true;
    }
    bool number_float(number_float_t, const string_t&) {
        field = Field::None;
        return true;
    }
    bool string(string_t& value) {
        switch (field) {
        case Field::Id:
            current_id = std::move(value);
            break;
        case Field::Name:
            current_name = std::move(value);
            break;
        case Field::IconUrl:
            current_icon_url = std::move(value);
            break;
        case Field::None:
            break;
        }
        field = Field::None;
        return true;
    }
    bool binary(binary_t&) {
        return true;
    }
    bool start_object(std::size_t) {
        ++depth;
        if (depth == 2) {
            current_id.clear();
            current_name.clear();
            current_icon_url.clear();
        }
        field = Field::None;
        return true;
    }
    bool key(string_t& value) {
        if (depth == 2) {
            if (value == "id") {
                field = Field::Id;
            } else if (value == "name") {
                field = Field::Name;
            } else if (value == "iconUrl") {
                field = Field::IconUrl;
            } else {
                field = Field::None;
            }
        }
        return true;
    }
    bool end_object() {
        if (depth == 2) {
            if (const u64 title_id = ParseTitleId(current_id); title_id != 0 && !current_name.empty()) {
                db.insert_or_assign(title_id,
                                    GameInfo{Sanitize(current_name), Sanitize(current_icon_url)});
            }
        }
        --depth;
        field = Field::None;
        return true;
    }
    bool start_array(std::size_t) {
        ++depth;
        field = Field::None;
        return true;
    }
    bool end_array() {
        --depth;
        return true;
    }
    bool parse_error(std::size_t, const std::string&, const json::exception&) {
        return false;
    }

    std::unordered_map<u64, GameInfo> db;

private:
    enum class Field { None, Id, Name, IconUrl };

    Field field = Field::None;
    int depth = 0;
    std::string current_id;
    std::string current_name;
    std::string current_icon_url;
};

bool IsFresh(const std::filesystem::path& db_path) {
    std::error_code ec;
    const auto modified = std::filesystem::last_write_time(db_path, ec);
    if (ec) {
        return false;
    }
    const auto age = std::filesystem::file_time_type::clock::now() - modified;
    return age < std::chrono::duration_cast<std::filesystem::file_time_type::duration>(
                     DbRefreshInterval);
}

bool RefreshDatabase(const std::filesystem::path& db_path) {
    LOG_INFO(Frontend, "Downloading the game title database (~90 MB)...");
    httplib::SSLClient cli{DbUrlHost, 443};
    cli.set_connection_timeout(std::chrono::seconds{15});
    cli.set_read_timeout(std::chrono::seconds{60});
    cli.set_follow_location(true);

    auto res = cli.Get(DbUrlPath, {{"User-Agent", "Eden-Emulator (game list metadata)"}});
    if (!res || res->status != 200) {
        LOG_WARNING(Frontend, "Title database download failed ({})",
                    res ? static_cast<int>(res->status) : 0);
        return false;
    }

    TitleDbFilter filter;
    if (!nlohmann::json::sax_parse(res->body, &filter) || filter.db.empty()) {
        LOG_WARNING(Frontend, "Title database could not be parsed");
        return false;
    }
    res->body.clear();
    res->body.shrink_to_fit();

    if (!Common::FS::CreateParentDirs(Common::FS::PathToUTF8String(db_path))) {
        return false;
    }
    std::ofstream out{db_path, std::ios::binary | std::ios::trunc};
    if (!out) {
        LOG_WARNING(Frontend, "Could not write the title database cache");
        return false;
    }
    for (const auto& [title_id, info] : filter.db) {
        out << fmt::format("{:016X}\t{}\t{}\n", title_id, info.name, info.icon_url);
    }
    LOG_INFO(Frontend, "Cached {} titles from the title database", filter.db.size());
    return true;
}

std::unordered_map<u64, GameInfo> ReadDatabaseFile(const std::filesystem::path& db_path) {
    std::unordered_map<u64, GameInfo> db;
    std::ifstream file{db_path};
    if (!file) {
        return db;
    }

    std::string line;
    while (std::getline(file, line)) {
        const auto name_split = line.find('\t');
        if (name_split == std::string::npos) {
            continue;
        }
        const auto url_split = line.find('\t', name_split + 1);
        const u64 title_id = ParseTitleId(line.substr(0, name_split));
        if (title_id == 0) {
            continue;
        }
        const std::string name = url_split == std::string::npos
                                     ? line.substr(name_split + 1)
                                     : line.substr(name_split + 1, url_split - name_split - 1);
        const std::string icon_url =
            url_split == std::string::npos ? std::string{} : line.substr(url_split + 1);
        db.insert_or_assign(title_id, GameInfo{name, icon_url});
    }
    return db;
}

std::unordered_map<u64, GameInfo> LoadDatabase() {
    const auto db_path = DatabasePath();
    if (!IsFresh(db_path) && !RefreshDatabase(db_path)) {
        LOG_INFO(Frontend, "Keeping the cached title database as-is");
    }
    return ReadDatabaseFile(db_path);
}

} // namespace

std::optional<GameInfo> Lookup(u64 title_id) {
    static std::mutex mutex;
    static std::unordered_map<u64, GameInfo> db;
    static bool load_attempted = false;

    std::scoped_lock lock{mutex};
    if (!load_attempted) {
        load_attempted = true;
        db = LoadDatabase();
    }

    const auto it = db.find(title_id);
    if (it == db.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<u8> FetchIcon(const std::string& url) {
    constexpr std::string_view Scheme = "https://";

    std::string_view rest = url;
    if (!rest.starts_with(Scheme)) {
        return {};
    }
    rest.remove_prefix(Scheme.size());
    const auto path_split = rest.find('/');
    if (path_split == std::string_view::npos) {
        return {};
    }

    httplib::SSLClient cli{std::string{rest.substr(0, path_split)}, 443};
    cli.set_connection_timeout(std::chrono::seconds{15});
    cli.set_read_timeout(std::chrono::seconds{60});

    const auto res = cli.Get(std::string{rest.substr(path_split)},
                             {{"User-Agent", "Eden-Emulator (game list metadata)"}});
    if (!res || res->status != 200 || res->body.size() < 8) {
        return {};
    }
    // The icons are JPEGs, but validate the content instead of trusting the extension.
    const auto& body = res->body;
    const bool is_jpeg = body[0] == '\xFF' && body[1] == '\xD8';
    const bool is_png = body[0] == '\x89' && body[1] == 'P' && body[2] == 'N' && body[3] == 'G';
    if (!is_jpeg && !is_png) {
        return {};
    }
    return {body.begin(), body.end()};
}

} // namespace QtCommon::GameMetadata
