#include "stdafx.h"
#include "RemoteLibraryClient.h"

#include <filesystem>
#include <fstream>
#include <winhttp.h>
#include <wincrypt.h>
#include <array>
#include <algorithm>
#include <sstream>
#include <mutex>
#include <set>

#include "nlohmann/json.hpp"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "advapi32.lib")

namespace
{
    using json = nlohmann::json;

    std::string HashStream(std::istream& input)
    {
        HCRYPTPROV provider{};
        HCRYPTHASH hash{};
        if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return {};
        if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash))
        {
            CryptReleaseContext(provider, 0);
            return {};
        }
        std::array<char, 65536> buffer{};
        bool valid = true;
        while (input)
        {
            input.read(buffer.data(), buffer.size());
            if (!CryptHashData(hash, reinterpret_cast<const BYTE*>(buffer.data()), static_cast<DWORD>(input.gcount()), 0)) valid = false;
        }
        BYTE digest[32]{};
        DWORD size = sizeof(digest);
        valid = valid && !input.bad() && CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0);
        CryptDestroyHash(hash);
        CryptReleaseContext(provider, 0);
        std::string result;
        if (valid)
            for (BYTE byte : digest)
            {
                result += "0123456789abcdef"[byte >> 4];
                result += "0123456789abcdef"[byte & 15];
            }
        return result;
    }

    bool ValidId(const std::string& value)
    {
        return !value.empty() && value.size() <= 128 && std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') || character == '_' || character == '-';
        });
    }

    bool ValidHash(const std::string& value)
    {
        return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char character) {
            return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
        });
    }

    std::wstring CoverExtension(const std::string& mime)
    {
        // These are the server image formats supported by CImage and the
        // player's same-basename artwork lookup. WebP is not decoded by CImage.
        if (mime == "image/jpeg") return L".jpg";
        if (mime == "image/png") return L".png";
        if (mime == "image/gif") return L".gif";
        return {};
    }

    bool FileHasHash(const std::filesystem::path& path, const std::string& hash)
    {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) return false;
        std::ifstream input(path, std::ios::binary);
        return input && HashStream(input) == hash;
    }

    std::wstring Utf8ToWide(const std::string& value)
    {
        if (value.empty())
            return {};
        int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (length <= 0)
            return {};
        std::wstring result(static_cast<size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length);
        return result;
    }

    std::string WideToUtf8(const std::wstring& value)
    {
        if (value.empty())
            return {};
        int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (length <= 0)
            return {};
        std::string result(static_cast<size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
        return result;
    }

    std::wstring JsonString(const json& value, const char* name)
    {
        if (!value.contains(name) || !value[name].is_string())
            return {};
        return Utf8ToWide(value[name].get<std::string>());
    }

    void ParsePlaylistMetadata(const json& value, RemotePlaylist& playlist)
    {
        playlist.id = value.at("id").get<std::string>();
        playlist.name = JsonString(value, "name");
        playlist.description = JsonString(value, "description");
        playlist.read_only = value.value("read_only", false);
        playlist.visibility = value.value("visibility", "private");
        playlist.subscription_status = value.value("subscription_status", "");
        playlist.subscription_reason = value.value("subscription_reason", "");
        playlist.source_owner_name = JsonString(value, "source_owner_name");
    }

    RemoteTrack ParseTrack(const json& value)
    {
        RemoteTrack track;
        track.id = value.value("id", "");
        track.unavailable = value.value("removed", false) || value.value("missing", false);
        track.version = value.value("version", 0ULL);
        track.title = JsonString(value, "title");
        track.artist = JsonString(value, "artist");
        track.album = JsonString(value, "album");
        track.lyrics = JsonString(value, "lyrics");
        track.comment = JsonString(value, "comment");
        track.genre = JsonString(value, "genre");
        track.content_hash = value.value("content_hash", "");
        track.cover_id = value.value("cover_id", "");
        track.cover_mime = value.value("cover_mime", "");
        track.cover_source = value.value("cover_source", "");
        track.format = value.value("format", "mp3");
        track.path = value.value("path", "");
        track.size = value.value("size", static_cast<unsigned __int64>(0));
        track.duration_ms = value.value("duration_ms", 0);
        track.bitrate_kbps = value.value("bitrate_kbps", 0);
        track.sample_rate = value.value("sample_rate", 0);
        track.channels = value.value("channels", 0);
        track.bits_per_sample = value.value("bits_per_sample", 0);
        return track;
    }

    std::wstring WinHttpError(const wchar_t* operation, DWORD error_code)
    {
        return std::wstring(operation) + L" failed (" + std::to_wstring(error_code) + L")";
    }
}

std::string CRemoteLibraryClient::HashString(const std::string& value)
{
    std::istringstream input(value);
    return HashStream(input);
}

void CRemoteLibraryClient::Configure(const std::wstring& base_url, const std::string& api_key, const std::wstring& cache_directory)
{
    m_base_url = base_url;
    while (!m_base_url.empty() && m_base_url.back() == L'/')
        m_base_url.pop_back();
    m_api_key = api_key;
    m_cache_directory = (std::filesystem::path(cache_directory) / Utf8ToWide(HashString("api/v2\n" + WideToUtf8(m_base_url) + "\n" + api_key))).wstring();
}

bool CRemoteLibraryClient::IsConfigured() const
{
    return !m_base_url.empty() && (m_base_url.rfind(L"http://", 0) == 0 || m_base_url.rfind(L"https://", 0) == 0);
}

bool CRemoteLibraryClient::CheckConnection(std::wstring& error_message) const
{
    std::vector<RemotePlaylist> playlists;
    return GetPlaylists(playlists, error_message);
}

bool CRemoteLibraryClient::GetPlaylists(std::vector<RemotePlaylist>& playlists, std::wstring& error_message) const
{
    playlists.clear();
    std::string response;
    if (!Request(L"/api/v2/playlists", response, error_message))
        return false;
    try
    {
        const json values = json::parse(response);
        if (!values.is_array())
        {
            error_message = L"服务器返回的歌单数据格式不正确";
            return false;
        }
        for (const auto& value : values)
        {
            RemotePlaylist playlist;
            ParsePlaylistMetadata(value, playlist);
            if (playlist.Available() && value.contains("track_ids") && value["track_ids"].is_array())
                for (const auto& track_id : value["track_ids"])
                    if (track_id.is_string())
                        playlist.track_ids.push_back(track_id.get<std::string>());
            playlists.push_back(std::move(playlist));
        }
        return true;
    }
    catch (const json::exception& exception)
    {
        error_message = L"解析歌单响应失败：" + Utf8ToWide(exception.what());
        return false;
    }
}

bool CRemoteLibraryClient::GetPlaylist(const std::string& playlist_id, RemotePlaylist& playlist, std::wstring& error_message) const
{
    std::string response;
    if (!Request(L"/api/v2/playlists/" + Utf8ToWide(playlist_id), response, error_message))
        return false;
    try
    {
        const json value = json::parse(response);
        if (!value.contains("playlist") || !value["playlist"].is_object())
        {
            error_message = L"服务器返回的歌单数据格式不正确";
            return false;
        }
        playlist = RemotePlaylist{};
        ParsePlaylistMetadata(value["playlist"], playlist);
        if (!playlist.Available()) return true;
        playlist.track_notes.clear();
        if (value["playlist"].contains("track_notes") && value["playlist"]["track_notes"].is_object())
            for (auto note = value["playlist"]["track_notes"].begin(); note != value["playlist"]["track_notes"].end(); ++note)
                if (note.value().is_string()) playlist.track_notes[note.key()] = Utf8ToWide(note.value().get<std::string>());
        playlist.track_ids.clear();
        playlist.tracks.clear();
        if (value["playlist"].contains("track_ids") && value["playlist"]["track_ids"].is_array())
            for (const auto& track_id : value["playlist"]["track_ids"])
                if (track_id.is_string())
                    playlist.track_ids.push_back(track_id.get<std::string>());
        if (value.contains("tracks") && value["tracks"].is_array())
            for (const auto& track : value["tracks"])
                playlist.tracks.push_back(ParseTrack(track));
        return true;
    }
    catch (const json::exception& exception)
    {
        error_message = L"解析歌单详情失败：" + Utf8ToWide(exception.what());
        return false;
    }
}

bool CRemoteLibraryClient::GetTrack(const std::string& track_id, RemoteTrack& track, std::wstring& error_message) const
{
    std::string response;
    if (!Request(L"/api/v2/tracks/" + Utf8ToWide(track_id), response, error_message))
        return false;
    try
    {
        track = ParseTrack(json::parse(response));
        return !track.id.empty();
    }
    catch (const json::exception& exception)
    {
        error_message = L"解析曲目详情失败：" + Utf8ToWide(exception.what());
        return false;
    }
}

bool CRemoteLibraryClient::SyncLibrary(std::vector<RemotePlaylist>& playlists, bool& offline, std::wstring& error_message, bool local_only, bool refresh_artwork) const
{
    playlists.clear();
    offline = false;
    m_transport_failed = false;
    error_message.clear();
    const auto snapshot_path = std::filesystem::path(m_cache_directory) / L"library.json";
    try
    {
        json saved;
        std::ifstream input(snapshot_path, std::ios::binary);
        if (input)
        {
            try { saved = json::parse(input); }
            catch (const json::exception&) { saved = nullptr; }
        }
        input.close();
        auto valid = [](const json& j) {
            return j.is_object() && j.value("cache_schema", 0) == 3 && j.contains("changed_tracks") &&
                j["changed_tracks"].is_array() && j.contains("playlists") && j["playlists"].is_array();
        };
        if (!valid(saved)) saved = nullptr;
        json snapshot = saved;
        bool received = false;
        if (local_only)
        {
            offline = true;
            if (AccessBlocked()) { error_message = L"授权已失效，请在线刷新验证当前 API Key。"; return false; }
        }
        else
        {
            // Candidate maps are private to this sync. Neither a failed page nor a
            // restarted snapshot can publish partial data or advance the saved cursor.
            bool completed = false;
            for (int attempt = 0; attempt < 3 && !completed; ++attempt)
            {
                std::map<std::string, json> tracks, lists;
                if (saved.is_object() && attempt == 0)
                {
                    for (const auto& t : saved["changed_tracks"]) tracks[t.at("id").get<std::string>()] = t;
                    for (const auto& p : saved["playlists"]) lists[p.at("id").get<std::string>()] = p;
                }
                std::string cursor = saved.is_object() && attempt == 0 ? saved.value("cursor", "") : "";
                std::set<std::string> seen;
                bool restart = false;
                for (int page = 0; page < 10000; ++page)
                {
                    if (m_cancel && *m_cancel) { error_message = L"任务已取消"; return false; }
                    // Signed server cursors use URL-safe base64; encode any other bytes.
                    std::wstring encoded;
                    for (unsigned char c : cursor)
                    {
                        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') encoded += wchar_t(c);
                        else { encoded += L'%'; encoded += L"0123456789ABCDEF"[c >> 4]; encoded += L"0123456789ABCDEF"[c & 15]; }
                    }
                    std::string response;
                    if (!Request(L"/api/v2/sync?limit=100" + (cursor.empty() ? L"" : L"&cursor=" + encoded), response, error_message))
                    {
                        if (m_http_status == 409 && m_error_code == "SYNC_RESTART") { restart = true; break; }
                        if (m_http_status == 401 || m_http_status == 403) BlockAccess();
                        if (m_transport_failed && valid(saved) && !AccessBlocked() && !(m_cancel && *m_cancel))
                        { offline = true; snapshot = saved; completed = true; break; }
                        return false;
                    }
                    const auto data = json::parse(response);
                    if (!data.is_object() || !data.contains("changed_tracks") || !data["changed_tracks"].is_array() ||
                        !data.contains("playlists") || !data["playlists"].is_array() ||
                        !data.contains("cursor") || !data["cursor"].is_string() || data["cursor"].get<std::string>().empty())
                        throw std::runtime_error("invalid v2 sync page");
                    if (page == 0 && (data.value("reset", false) || data.value("full_snapshot", false)))
                    { tracks.clear(); lists.clear(); }
                    if (page > 0 && data.value("reset", false)) throw std::runtime_error("reset inside pagination");
                    for (const auto& t : data["changed_tracks"])
                    {
                        const auto id = t.at("id").get<std::string>();
                        if (!ValidId(id)) throw std::runtime_error("invalid track UUID");
                        if (t.value("removed", false)) tracks.erase(id); else tracks[id] = t;
                    }
                    for (const auto& p : data["playlists"])
                    {
                        const auto id = p.at("id").get<std::string>();
                        if (!ValidId(id)) throw std::runtime_error("invalid playlist ID");
                        lists[id] = p;
                    }
                    for (const auto& id : data.value("deleted_tracks", json::array())) tracks.erase(id.get<std::string>());
                    for (const auto& id : data.value("deleted_playlists", json::array())) lists.erase(id.get<std::string>());
                    cursor = data.at("cursor").get<std::string>();
                    if (data.value("has_more", false))
                    {
                        if (!seen.insert(cursor).second) throw std::runtime_error("repeated sync cursor");
                        continue;
                    }
                    snapshot = {{"cache_schema",3},{"revision",data.at("revision")},{"cursor",cursor},
                        {"changed_tracks",json::array()},{"playlists",json::array()}};
                    for (const auto& t : tracks) snapshot["changed_tracks"].push_back(t.second);
                    for (const auto& p : lists) snapshot["playlists"].push_back(p.second);
                    std::sort(snapshot["playlists"].begin(), snapshot["playlists"].end(), [](const json& a, const json& b) {
                        const auto ao = a.value("sort_order", 0), bo = b.value("sort_order", 0);
                        if (ao != bo) return ao < bo;
                        const auto an = a.value("name", ""), bn = b.value("name", "");
                        return an != bn ? an < bn : a.at("id").get<std::string>() < b.at("id").get<std::string>();
                    });
                    completed = received = true;
                    break;
                }
                if (!completed && !restart) throw std::runtime_error("too many sync pages");
            }
            if (!completed) { error_message = L"同步期间曲库持续变化，请稍后刷新。"; return false; }
        }
        if (!valid(snapshot)) { error_message += L"；没有可用的 v2 离线快照"; return false; }
        std::map<std::string, RemoteTrack> tracks;
        for (const auto& value : snapshot["changed_tracks"])
        {
            auto track = ParseTrack(value);
            tracks[track.id] = std::move(track);
        }
        std::vector<RemotePlaylist> result;
        for (const auto& value : snapshot["playlists"])
        {
            RemotePlaylist playlist;
            ParsePlaylistMetadata(value, playlist);
            if (!playlist.Available()) { result.push_back(std::move(playlist)); continue; }
            const auto unavailable = value.value("unavailable_track_ids", std::vector<std::string>{});
            for (const auto& identity : value.value("track_ids", json::array()))
            {
                const auto id = identity.get<std::string>();
                if (!ValidId(id)) throw std::runtime_error("invalid member UUID");
                playlist.track_ids.push_back(id);
                auto found = tracks.find(id);
                RemoteTrack track;
                if (found != tracks.end()) track = found->second;
                else { track.id = id; track.title = L"不可用曲目"; track.unavailable = true; }
                if (std::find(unavailable.begin(), unavailable.end(), id) != unavailable.end()) track.unavailable = true;
                playlist.tracks.push_back(std::move(track));
            }
            if (value.contains("track_notes") && value["track_notes"].is_object())
                for (auto note = value["track_notes"].begin(); note != value["track_notes"].end(); ++note)
                    if (note.value().is_string()) playlist.track_notes[note.key()] = Utf8ToWide(note.value().get<std::string>());
            result.push_back(std::move(playlist));
        }
        if (m_cancel && *m_cancel) { error_message = L"任务已取消"; return false; }
        if (received)
        {
            std::filesystem::create_directories(snapshot_path.parent_path());
            const auto temporary = snapshot_path.wstring() + L".tmp";
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output << snapshot.dump();
            output.close();
            if (!output || !MoveFileExW(temporary.c_str(), snapshot_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            { error_message = L"同步快照保存失败，请检查缓存目录权限。"; return false; }
            // A successful complete sync revalidates this key, including re-enabled accounts.
            std::error_code ignored;
            std::filesystem::remove(snapshot_path.parent_path() / L"access-blocked", ignored);
            error_message.clear();
        }
        playlists = std::move(result);
        if (received && refresh_artwork)
            for (const auto& t : tracks) if (!t.second.unavailable) RefreshArtwork(t.second);
        return true;
    }
    catch (const std::exception& exception)
    {
        error_message = L"读取媒体库失败：" + Utf8ToWide(exception.what());
        return false;
    }
}

bool CRemoteLibraryClient::AccessBlocked() const
{
    std::error_code ignored;
    return std::filesystem::exists(std::filesystem::path(m_cache_directory) / L"access-blocked", ignored);
}

void CRemoteLibraryClient::BlockAccess() const
{
    std::error_code ignored;
    std::filesystem::create_directories(m_cache_directory, ignored);
    std::ofstream marker(std::filesystem::path(m_cache_directory) / L"access-blocked", std::ios::trunc);
    marker << "Online authorization required";
}

bool CRemoteLibraryClient::PlaylistAllowed(const std::string& id) const
{
    if (AccessBlocked()) return false;
    try
    {
        std::ifstream in(std::filesystem::path(m_cache_directory) / L"library.json");
        if (!in) return false;
        auto j = json::parse(in);
        for (const auto& p : j.at("playlists"))
            if (p.at("id") == id)
            {
                const auto status=p.value("subscription_status", "");
                return status.empty() || status == "active";
            }
    }
    catch (const std::exception&) {}
    return false;
}

bool CRemoteLibraryClient::TrackAllowed(const RemoteTrack& track) const
{
    if (track.unavailable || AccessBlocked()) return false;
    try
    {
        std::ifstream in(std::filesystem::path(m_cache_directory) / L"library.json");
        if (!in) return true; // Direct track requests before the first library sync.
        auto j = json::parse(in);
        for (const auto& t : j.at("changed_tracks"))
            if (t.at("id") == track.id)
                return !t.value("removed", false) && !t.value("missing", false) && t.value("content_hash", "") == track.content_hash;
    }
    catch (const std::exception&) {}
    return false;
}

bool CRemoteLibraryClient::EnsureCached(const RemoteTrack& track, std::wstring& cached_path, std::wstring& error_message, bool allow_network) const
{
    m_transport_failed = false;
    if (m_cancel && *m_cancel) { error_message = L"任务已取消"; return false; }
    if (!TrackAllowed(track)) { error_message = L"曲目不可用或授权已失效，请刷新歌单。"; return false; }
    cached_path = CachePath(track);
    if (cached_path.empty())
    {
        error_message = L"缓存目录或曲目哈希为空";
        return false;
    }
    std::error_code file_error;
    bool verified = false;
    if (std::filesystem::is_regular_file(cached_path, file_error) && std::filesystem::file_size(cached_path, file_error) == track.size)
    {
        std::ifstream input(cached_path, std::ios::binary);
        verified = HashStream(input) == track.content_hash;
    }
    const std::filesystem::path destination(cached_path);
    if (!verified && !allow_network)
    {
        error_message = L"曲目尚未缓存或缓存校验失败";
        return false;
    }
    std::filesystem::create_directories(destination.parent_path(), file_error);
    if (file_error)
    {
        error_message = L"无法创建缓存目录";
        return false;
    }
    if (!verified)
    {
        const std::wstring temporary_path = cached_path + L".download";
        std::string ignored;
        bool success = Download(L"/api/v2/media/" + Utf8ToWide(track.id), ignored, error_message, temporary_path, track.size);
        if (success)
        {
            std::ifstream input(temporary_path, std::ios::binary);
            success = std::filesystem::file_size(temporary_path, file_error) == track.size && HashStream(input) == track.content_hash;
            if (!success) error_message = L"下载文件大小或 SHA-256 校验失败，请刷新歌单";
        }
        if (success && !MoveFileExW(temporary_path.c_str(), cached_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            error_message = L"无法保存缓存文件";
            success = false;
        }
        if (!success)
        {
            std::filesystem::remove(temporary_path, file_error);
            return false;
        }
    }
    std::filesystem::path lyrics_path(destination);
    lyrics_path.replace_extension(L".lrc");
    if (!track.lyrics.empty())
    {
        std::ofstream lyrics(lyrics_path, std::ios::binary | std::ios::trunc);
        lyrics << "\xef\xbb\xbf" << WideToUtf8(track.lyrics);
        lyrics.close();
        if (!lyrics) { error_message = L"保存歌词失败"; return false; }
    }
    else std::filesystem::remove(lyrics_path, file_error);
    // A verified audio cache never requires a server request for playback.
    EnsureCoverCached(track, cached_path, !verified && allow_network, true);
    return true;
}

void CRemoteLibraryClient::RefreshArtwork(const RemoteTrack& track) const
{
    const auto path = CachePath(track);
    std::error_code ignored;
    if (!path.empty() && std::filesystem::is_regular_file(path, ignored) && TrackAllowed(track)) EnsureCoverCached(track, path, true, false);
}

void CRemoteLibraryClient::EnsureCoverCached(const RemoteTrack& track, const std::wstring& cached_path, bool allow_network, bool apply_to_audio) const
{
    const std::wstring extension = CoverExtension(track.cover_mime);
    const bool wanted = ValidHash(track.cover_id) && !extension.empty();
    std::filesystem::path sidecar(cached_path);
    if (!extension.empty()) sidecar.replace_extension(extension);
    std::filesystem::path asset = std::filesystem::path(m_cache_directory) / L"covers" / (Utf8ToWide(track.cover_id) + extension);
    std::error_code file_error;
    bool verified = wanted && FileHasHash(asset, track.cover_id);
    if (wanted && !verified && FileHasHash(sidecar, track.cover_id))
    {
        // Retain artwork downloaded by earlier client versions, without HTTP.
        std::filesystem::create_directories(asset.parent_path(), file_error);
        std::filesystem::copy_file(sidecar, asset, std::filesystem::copy_options::overwrite_existing, file_error);
        verified = !file_error;
    }
    if (wanted && !verified && allow_network && !(m_cancel && *m_cancel))
    {
        std::filesystem::create_directories(asset.parent_path(), file_error);
        const std::wstring temporary = asset.wstring() + L".download";
        std::string ignored;
        std::wstring error;
        // Artwork is optional: permission/network errors do not block audio.
        if (Download(L"/api/v2/covers/" + Utf8ToWide(track.cover_id), ignored, error, temporary, 8 * 1024 * 1024)
            && FileHasHash(temporary, track.cover_id))
            verified = !!MoveFileExW(temporary.c_str(), asset.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        std::filesystem::remove(temporary, file_error);
    }
    if (!apply_to_audio || (m_cancel && *m_cancel)) return;
    for (const wchar_t* candidate : {L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".webp"})
    {
        std::filesystem::path previous(cached_path);
        previous.replace_extension(candidate);
        if (!verified || previous != sidecar) std::filesystem::remove(previous, file_error);
    }
    if (verified && !FileHasHash(sidecar, track.cover_id))
        std::filesystem::copy_file(asset, sidecar, std::filesystem::copy_options::overwrite_existing, file_error);
}

bool CRemoteLibraryClient::Request(const std::wstring& path, std::string& response, std::wstring& error_message) const
{
    return Download(path, response, error_message);
}

bool CRemoteLibraryClient::Download(const std::wstring& path, std::string& response, std::wstring& error_message, const std::wstring& destination, unsigned __int64 maximum) const
{
    m_transport_failed = false;
    m_http_status = 0;
    m_error_code.clear();
    if (m_cancel && *m_cancel) { error_message = L"任务已取消"; return false; }
    // Settings connection checks use a separate client from the library worker.
    // Serialize every HTTP request to respect keys with a concurrency limit of 1.
    static std::mutex request_mutex;
    std::lock_guard<std::mutex> request_lock(request_mutex);
    if (m_cancel && *m_cancel) { error_message = L"任务已取消"; return false; }
    response.clear();
    if (!IsConfigured())
    {
        error_message = L"服务器地址未配置或不是 HTTP/HTTPS 地址";
        return false;
    }
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    wchar_t scheme[16]{};
    wchar_t host[256]{};
    wchar_t urlPath[2048]{};
    components.lpszScheme = scheme;
    components.dwSchemeLength = static_cast<DWORD>(std::size(scheme));
    components.lpszHostName = host;
    components.dwHostNameLength = static_cast<DWORD>(std::size(host));
    components.lpszUrlPath = urlPath;
    components.dwUrlPathLength = static_cast<DWORD>(std::size(urlPath));
    if (!WinHttpCrackUrl(m_base_url.c_str(), 0, 0, &components))
    {
        error_message = WinHttpError(L"解析服务器地址", GetLastError());
        return false;
    }
    const bool secure = _wcsicmp(scheme, L"https") == 0;
    if (m_api_key.find_first_of("\r\n") != std::string::npos) { error_message = L"API Key 含有非法换行"; return false; }
    HINTERNET session = WinHttpOpen(L"MusicPlayer2 Remote Library/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr)
    {
        error_message = WinHttpError(L"打开 HTTP 会话", GetLastError());
        return false;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 15000, 15000);
    HINTERNET connection = WinHttpConnect(session, host, components.nPort, 0);
    if (connection == nullptr)
    {
        m_transport_failed=true;
        error_message = WinHttpError(L"连接服务器", GetLastError());
        WinHttpCloseHandle(session);
        return false;
    }
    const std::wstring prefix(urlPath, components.dwUrlPathLength);
    const std::wstring requestPath = prefix + (path.empty() || path.front() == L'/' ? path : L"/" + path);
    HINTERNET request = WinHttpOpenRequest(connection, L"GET", requestPath.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (request == nullptr)
    {
        error_message = WinHttpError(L"创建 HTTP 请求", GetLastError());
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }
    std::wstring headers = L"Accept: application/json, audio/*, image/*\r\n";
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy, sizeof(redirect_policy));
    if (!m_api_key.empty())
        headers += L"X-API-Key: " + Utf8ToWide(m_api_key) + L"\r\n";
    bool success = WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(headers.size()), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) == TRUE;
    if (success)
        success = WinHttpReceiveResponse(request, nullptr) == TRUE;
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (success)
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (!success)
    {
        m_transport_failed = true;
        error_message = WinHttpError(L"HTTP 请求", GetLastError());
    }
    else if (status < 200 || status >= 300)
    {
        m_http_status = status;
        std::string body;
        std::array<char, 4096> data{};
        DWORD read = 0;
        while (body.size() < 65536 && WinHttpReadData(request, data.data(), DWORD(data.size()), &read) && read)
            body.append(data.data(), read);
        try { m_error_code = json::parse(body).at("error").value("code", ""); }
        catch (const json::exception&) {}
        error_message = L"服务器返回 HTTP 状态码 " + std::to_wstring(status);
        if (status == 401) error_message += L"：API Key 无效、已过期或账号不可用，请在管理网页检查。";
        else if (status == 403) error_message += L"：缺少读取权限，请检查 Key 的范围和歌单授权。";
        else if (status == 404) error_message += L"：曲目未授权、已下架或文件缺失，请刷新歌单。";
        else if (status == 429) error_message += L"：请求或下载并发达到上限，请稍后重试。";
        if (status == 401 || (status == 403 && path.rfind(L"/api/v2/covers/", 0) != 0)) BlockAccess();
        success = false;
    }
    std::ofstream output;
    if (success && !destination.empty())
    {
        output.open(destination, std::ios::binary | std::ios::trunc);
        if (!output) { success = false; error_message = L"无法写入缓存"; }
    }
    unsigned __int64 total = 0;
    std::array<char, 65536> buffer{};
    while (success)
    {
        if (m_cancel && *m_cancel) { error_message = L"任务已取消"; success = false; break; }
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available))
        {
            m_transport_failed = true;
            error_message = WinHttpError(L"读取 HTTP 响应", GetLastError());
            success = false;
            break;
        }
        if (available == 0)
            break;
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), (std::min)(available, static_cast<DWORD>(buffer.size())), &read))
        {
            m_transport_failed = true;
            error_message = WinHttpError(L"读取 HTTP 数据", GetLastError());
            success = false;
            break;
        }
        if (read == 0) break;
        total += read;
        if (total > maximum) { error_message = L"响应超过允许大小"; success = false; break; }
        if (destination.empty()) response.append(buffer.data(), read);
        else
        {
            output.write(buffer.data(), read);
            if (!output) { error_message = L"缓存写入失败"; success = false; }
        }
    }
    if (output.is_open())
    {
        output.close();
        if (!output) { error_message = L"缓存写入失败"; success = false; }
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return success;
}

std::wstring CRemoteLibraryClient::CachePath(const RemoteTrack& track) const
{
    if (!m_cache_directory.empty() && ValidId(track.id) && track.unavailable)
        return (std::filesystem::path(m_cache_directory) / L"unavailable" / (Utf8ToWide(track.id) + L".mp3")).wstring();
    if (m_cache_directory.empty() || !ValidHash(track.content_hash) || !ValidId(track.id) || !ValidId(track.format) || track.format.size() > 8)
        return {};
    return (std::filesystem::path(m_cache_directory) / (Utf8ToWide(track.id) + L"-" + Utf8ToWide(track.content_hash) + L"." + Utf8ToWide(track.format))).wstring();
}
