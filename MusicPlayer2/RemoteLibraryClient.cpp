#include "stdafx.h"
#include "RemoteLibraryClient.h"

#include <filesystem>
#include <fstream>
#include <winhttp.h>
#include <wincrypt.h>
#include <array>
#include <algorithm>
#include <sstream>

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

    RemoteTrack ParseTrack(const json& value)
    {
        RemoteTrack track;
        track.id = value.value("id", "");
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
    m_cache_directory = (std::filesystem::path(cache_directory) / Utf8ToWide(HashString(WideToUtf8(m_base_url) + "\n" + api_key))).wstring();
}

bool CRemoteLibraryClient::IsConfigured() const
{
    return !m_base_url.empty() && (m_base_url.rfind(L"http://", 0) == 0 || m_base_url.rfind(L"https://", 0) == 0);
}

bool CRemoteLibraryClient::CheckConnection(std::wstring& error_message) const
{
    std::string response;
    return Request(L"/api/v1/playlists", response, error_message);
}

bool CRemoteLibraryClient::GetPlaylists(std::vector<RemotePlaylist>& playlists, std::wstring& error_message) const
{
    playlists.clear();
    std::string response;
    if (!Request(L"/api/v1/playlists", response, error_message))
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
            playlist.id = value.value("id", "");
            playlist.name = JsonString(value, "name");
            playlist.description = JsonString(value, "description");
            if (value.contains("track_ids") && value["track_ids"].is_array())
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
    if (!Request(L"/api/v1/playlists/" + Utf8ToWide(playlist_id), response, error_message))
        return false;
    try
    {
        const json value = json::parse(response);
        if (!value.contains("playlist") || !value["playlist"].is_object())
        {
            error_message = L"服务器返回的歌单数据格式不正确";
            return false;
        }
        playlist.id = value["playlist"].value("id", playlist_id);
        playlist.name = JsonString(value["playlist"], "name");
        playlist.description = JsonString(value["playlist"], "description");
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
    if (!Request(L"/api/v1/tracks/" + Utf8ToWide(track_id), response, error_message))
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

bool CRemoteLibraryClient::SyncLibrary(std::vector<RemotePlaylist>& playlists, bool& offline, std::wstring& error_message) const
{
    playlists.clear();
    offline = false;
    const std::filesystem::path snapshot_path = std::filesystem::path(m_cache_directory) / L"library.json";
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
        std::string response;
        const auto revision = saved.is_object() ? saved.value("revision", -1LL) : -1LL;
        const std::wstring endpoint = revision >= 0 ? L"/api/v1/sync?since=" + std::to_wstring(revision) : L"/api/v1/sync";
        bool received = Request(endpoint, response, error_message);
        json snapshot;
        if (received)
        {
            snapshot = json::parse(response);
            if (!snapshot.value("full_snapshot", false)) snapshot = saved;
        }
        else
        {
            offline = true;
            snapshot = saved;
        }
        if (!snapshot.is_object() || !snapshot.contains("changed_tracks") || !snapshot["changed_tracks"].is_array() || !snapshot.contains("playlists") || !snapshot["playlists"].is_array())
        {
            if (received) error_message = L"服务器快照格式错误";
            else error_message += L"；没有可用离线快照";
            return false;
        }
        std::map<std::string, RemoteTrack> tracks;
        for (const auto& value : snapshot["changed_tracks"])
        {
            RemoteTrack track = ParseTrack(value);
            tracks.emplace(track.id, std::move(track));
        }
        if (received)
        {
            // Refresh artwork for audio already cached, including removal in
            // a new snapshot. Offline snapshots retain their verified artwork.
            std::error_code file_error;
            for (const auto& entry : tracks)
            {
                const std::wstring cached_path = CachePath(entry.second);
                if (!cached_path.empty() && std::filesystem::is_regular_file(cached_path, file_error))
                    EnsureCoverCached(entry.second, cached_path);
            }
            if (saved.is_object() && saved.contains("changed_tracks") && saved["changed_tracks"].is_array())
                for (const auto& value : saved["changed_tracks"])
                {
                    RemoteTrack previous = ParseTrack(value);
                    if (tracks.find(previous.id) != tracks.end()) continue;
                    const std::wstring cached_path = CachePath(previous);
                    previous.cover_id.clear();
                    if (!cached_path.empty()) EnsureCoverCached(previous, cached_path);
                }
        }
        for (const auto& value : snapshot["playlists"])
        {
            RemotePlaylist playlist;
            playlist.id = value.value("id", "");
            playlist.name = JsonString(value, "name");
            playlist.description = JsonString(value, "description");
            if (value.contains("track_ids") && value["track_ids"].is_array())
                for (const auto& identity : value["track_ids"])
                {
                    const std::string id = identity.get<std::string>();
                    playlist.track_ids.push_back(id);
                    auto found = tracks.find(id);
                    if (found != tracks.end()) playlist.tracks.push_back(found->second);
                }
            if (value.contains("track_notes") && value["track_notes"].is_object())
                for (auto note = value["track_notes"].begin(); note != value["track_notes"].end(); ++note)
                    if (note.value().is_string()) playlist.track_notes[note.key()] = Utf8ToWide(note.value().get<std::string>());
            playlists.push_back(std::move(playlist));
        }
        if (received)
        {
            std::error_code file_error;
            std::filesystem::create_directories(snapshot_path.parent_path(), file_error);
            const std::wstring temporary = snapshot_path.wstring() + L".tmp";
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output << snapshot.dump();
            output.close();
            if (!output || !MoveFileExW(temporary.c_str(), snapshot_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                error_message = L"在线同步成功，但离线快照保存失败";
            else error_message.clear();
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        error_message = L"读取媒体库失败：" + Utf8ToWide(exception.what());
        return false;
    }
}

bool CRemoteLibraryClient::EnsureCached(const RemoteTrack& track, std::wstring& cached_path, std::wstring& error_message) const
{
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
        bool success = Download(L"/api/v1/media/" + Utf8ToWide(track.id), ignored, error_message, temporary_path, track.size);
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
    EnsureCoverCached(track, cached_path);
    return true;
}

void CRemoteLibraryClient::EnsureCoverCached(const RemoteTrack& track, const std::wstring& cached_path) const
{
    const std::wstring extension = CoverExtension(track.cover_mime);
    std::filesystem::path target(cached_path);
    if (!extension.empty()) target.replace_extension(extension);
    const bool wanted = ValidHash(track.cover_id) && !extension.empty();
    std::error_code file_error;
    bool verified = wanted && FileHasHash(target, track.cover_id);
    // Never leave stale art selected after a server-side change or removal.
    for (const wchar_t* candidate : {L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".webp"})
    {
        std::filesystem::path previous(cached_path);
        previous.replace_extension(candidate);
        if (!verified || previous != target)
            std::filesystem::remove(previous, file_error);
    }
    if (!wanted || verified || (m_cancel && *m_cancel)) return;
    const std::wstring temporary = target.wstring() + L".download";
    std::string ignored;
    std::wstring error;
    // Artwork is optional: a network/permission failure never blocks playback.
    bool downloaded = Download(L"/api/v1/covers/" + Utf8ToWide(track.cover_id), ignored, error, temporary, 8 * 1024 * 1024);
    if (downloaded && FileHasHash(temporary, track.cover_id))
        MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    std::filesystem::remove(temporary, file_error);
}

bool CRemoteLibraryClient::Request(const std::wstring& path, std::string& response, std::wstring& error_message) const
{
    return Download(path, response, error_message);
}

bool CRemoteLibraryClient::Download(const std::wstring& path, std::string& response, std::wstring& error_message, const std::wstring& destination, unsigned __int64 maximum) const
{
    response.clear();
    if (!IsConfigured())
    {
        error_message = L"服务器地址未配置或不是 HTTP/HTTPS 地址";
        return false;
    }
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    wchar_t host[256]{};
    wchar_t urlPath[2048]{};
    components.lpszHostName = host;
    components.dwHostNameLength = static_cast<DWORD>(std::size(host));
    components.lpszUrlPath = urlPath;
    components.dwUrlPathLength = static_cast<DWORD>(std::size(urlPath));
    if (!WinHttpCrackUrl(m_base_url.c_str(), 0, 0, &components))
    {
        error_message = WinHttpError(L"解析服务器地址", GetLastError());
        return false;
    }
    const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;
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
        error_message = WinHttpError(L"HTTP 请求", GetLastError());
    }
    else if (status < 200 || status >= 300)
    {
        error_message = L"服务器返回 HTTP 状态码 " + std::to_wstring(status);
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
            error_message = WinHttpError(L"读取 HTTP 响应", GetLastError());
            success = false;
            break;
        }
        if (available == 0)
            break;
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), (std::min)(available, static_cast<DWORD>(buffer.size())), &read))
        {
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
    if (m_cache_directory.empty() || !ValidHash(track.content_hash) || !ValidId(track.id) || !ValidId(track.format) || track.format.size() > 8)
        return {};
    return (std::filesystem::path(m_cache_directory) / (Utf8ToWide(track.content_hash) + L"." + Utf8ToWide(track.format))).wstring();
}
