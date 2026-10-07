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
    std::string format;
    std::string path;
    unsigned __int64 size{};
};

struct RemotePlaylist
{
    std::string id;
    std::wstring name;
    std::wstring description;
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
    bool EnsureCached(const RemoteTrack& track, std::wstring& cached_path, std::wstring& error_message) const;
    bool SyncLibrary(std::vector<RemotePlaylist>& playlists, bool& offline, std::wstring& error_message) const;
    static std::string HashString(const std::string& value);
    void SetCancellation(std::shared_ptr<std::atomic<bool>> cancel) { m_cancel = std::move(cancel); }

private:
    bool Request(const std::wstring& path, std::string& response, std::wstring& error_message) const;
    bool Download(const std::wstring& path, std::string& response, std::wstring& error_message, const std::wstring& destination = L"", unsigned __int64 maximum = 16 * 1024 * 1024) const;
    std::wstring CachePath(const RemoteTrack& track) const;

    std::wstring m_base_url;
    std::string m_api_key;
    std::wstring m_cache_directory;
    std::shared_ptr<std::atomic<bool>> m_cancel;
};
