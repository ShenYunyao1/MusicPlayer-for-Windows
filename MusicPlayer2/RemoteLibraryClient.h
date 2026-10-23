#pragma once

#include <string>
#include <vector>
#include <map>
#include <atomic>
#include <memory>

struct RemoteTrack
{
    std::string id;
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    std::wstring lyrics;
    std::wstring comment;
    std::wstring genre;
    std::string content_hash;
    std::string cover_id;
    std::string cover_mime;
    std::string cover_source;
    std::string format;
    std::string path;
    unsigned __int64 size{};
    bool unavailable{};
    unsigned __int64 version{};
    int duration_ms{}, bitrate_kbps{}, sample_rate{}, channels{}, bits_per_sample{};
};

struct RemotePlaylist
{
    std::string id;
    std::wstring name;
    std::wstring description;
    bool read_only{};
    std::string visibility;
    std::string subscription_status;
    std::string subscription_reason;
    std::wstring source_owner_name;
    bool Available() const { return subscription_status.empty() || subscription_status == "active"; }
    std::wstring UnavailableReason() const
    {
        if (subscription_reason == "private") return L"作者已设为私有";
        if (subscription_reason == "owner_disabled") return L"作者账户已停用";
        if (subscription_reason == "deleted") return L"源歌单已删除";
        if (subscription_reason == "share_revoked") return L"分享尚未接受或已被撤回";
        return L"订阅暂不可用";
    }
    std::vector<std::string> track_ids;
    std::vector<RemoteTrack> tracks;
    std::map<std::string, std::wstring> track_notes;
};

class CRemoteLibraryClient
{
public:
    void Configure(const std::wstring& base_url, const std::string& api_key, const std::wstring& cache_directory);
    bool IsConfigured() const;
    bool CheckConnection(std::wstring& error_message) const;
    bool GetPlaylists(std::vector<RemotePlaylist>& playlists, std::wstring& error_message) const;
    bool GetPlaylist(const std::string& playlist_id, RemotePlaylist& playlist, std::wstring& error_message) const;
    bool GetTrack(const std::string& track_id, RemoteTrack& track, std::wstring& error_message) const;
    bool EnsureCached(const RemoteTrack& track, std::wstring& cached_path, std::wstring& error_message, bool allow_network = true) const;
    bool SyncLibrary(std::vector<RemotePlaylist>& playlists, bool& offline, std::wstring& error_message, bool local_only = false, bool refresh_artwork = true) const;
    // Explicit refresh stages artwork by content ID without changing active playback files.
    void RefreshArtwork(const RemoteTrack& track) const;
    std::wstring CachePath(const RemoteTrack& track) const;
    const std::wstring& CacheDirectory() const { return m_cache_directory; }
    bool TransportFailed() const { return m_transport_failed; }
    bool AccessBlocked() const;
    bool PlaylistAllowed(const std::string& id) const;
    bool TrackAllowed(const RemoteTrack& track) const;
    static std::string HashString(const std::string& value);
    void SetCancellation(std::shared_ptr<std::atomic<bool>> cancel) { m_cancel = std::move(cancel); }

private:
    bool Request(const std::wstring& path, std::string& response, std::wstring& error_message) const;
    bool Download(const std::wstring& path, std::string& response, std::wstring& error_message, const std::wstring& destination = L"", unsigned __int64 maximum = 16 * 1024 * 1024) const;
    void EnsureCoverCached(const RemoteTrack& track, const std::wstring& cached_path, bool allow_network, bool apply_to_audio) const;
    void BlockAccess() const;

    std::wstring m_base_url;
    std::string m_api_key;
    std::wstring m_cache_directory;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    mutable bool m_transport_failed{};
    mutable unsigned long m_http_status{};
    mutable std::string m_error_code;
};
