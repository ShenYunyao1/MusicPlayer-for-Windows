#pragma once
#include "RemoteLibraryConfig.h"
#include "SongInfo.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <functional>
#include <chrono>

// Results are published by done (release/acquire). Workers never access windows
// or the player; their lifetime is independent of the media library dialog.
struct RemoteLibraryTask
{
    std::atomic<bool> done{false};
    std::shared_ptr<std::atomic<bool>> cancel{std::make_shared<std::atomic<bool>>(false)};
    bool success{}, offline{}, local_only{}, access_blocked{};
    bool continuation{}; // Worker-only: another chunk is queued before publishing done.
    std::wstring path, status, namespace_id;
    std::vector<RemotePlaylist> playlists;
    RemoteTransferState transfer;
};

struct RemoteBrowseState
{
    std::string playlist_id, track_id;
    int top_row{};
};

class CRemoteLibraryManager
{
public:
    static CRemoteLibraryManager& Instance();
    ~CRemoteLibraryManager();
    std::shared_ptr<RemoteLibraryTask> Library(bool refresh = false);
    std::shared_ptr<RemoteLibraryTask> Prepare(const std::wstring& path, bool allow_network);
    std::shared_ptr<RemoteLibraryTask> SaveOffline(const RemotePlaylist& playlist);
    bool Materialize(const RemotePlaylist& playlist, const std::wstring& preferred_path,
        std::wstring& path, std::wstring& error, bool force = false);
    std::vector<std::wstring> MaterializedPaths(const RemotePlaylist& playlist) const;
    bool LoadPlaylist(const std::wstring& path);
    bool PlaylistUsable(const std::wstring& path) const;
    void RemoveUnavailableRecentPlaylists();
    bool ApplyMetadata(SongInfo& song);
    bool IsRemote(const std::wstring& path);
    std::wstring TrackName(const std::wstring& path);
    std::wstring PlaylistName(const std::wstring& path);
    RemoteBrowseState BrowseState();
    void SaveBrowseState(const RemoteBrowseState& state);
    void RetryConnection();
    void SetBrowserActive(bool active) { m_browser_active = active; }
    std::shared_ptr<RemoteLibraryTask> PollSynchronization(bool remote_playlist);
    std::shared_ptr<RemoteLibraryTask> CurrentLibrary();
    void ConfigurationChanged();
    RemoteTransferProgress TransferProgress(const std::wstring& path, bool foreground) const;
    std::string TrackIdentity(const std::wstring& path);
    bool NeedsMaterialize(const RemotePlaylist& playlist, const std::wstring& path) const;
    void Shutdown();
private:
    CRemoteLibraryManager();
    struct Binding { CRemoteLibraryClient client; RemoteTrack track; };
    void Enqueue(std::shared_ptr<RemoteLibraryTask> task, std::function<void()> work, bool priority = false);
    void Worker();
    void SaveOfflineStep(std::shared_ptr<RemoteLibraryTask> task, CRemoteLibraryClient client, std::shared_ptr<RemotePlaylist> playlist, size_t index);
    mutable std::mutex m_mutex;
    std::condition_variable m_ready;
    std::deque<std::pair<std::shared_ptr<RemoteLibraryTask>, std::function<void()>>> m_jobs;
    std::thread m_worker;
    bool m_stop{};
    std::shared_ptr<RemoteLibraryTask> m_active, m_library;
    std::shared_ptr<RemoteLibraryTask> m_foreground, m_bulk, m_observed;
    RemoteRefreshSchedule m_refresh;
    bool m_browser_active{};
    std::wstring m_library_namespace;
    std::map<std::wstring, Binding> m_tracks;
    std::map<std::wstring, std::chrono::steady_clock::time_point> m_offline_until;
};
