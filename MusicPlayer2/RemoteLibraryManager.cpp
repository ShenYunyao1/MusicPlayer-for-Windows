#include "stdafx.h"
#include "RemoteLibraryManager.h"
#include "MusicPlayer2.h"
#include "Playlist.h"
#include "CRecentList.h"
#include "Common.h"
#include "nlohmann/json.hpp"
#include <filesystem>
#include <fstream>

namespace
{
    using json = nlohmann::json;
    std::string Utf8(const std::wstring& value) { return CCommon::UnicodeToStr(value, CodeType::UTF8_NO_BOM); }
    std::wstring Wide(const std::string& value) { return CCommon::StrToUnicode(value, CodeType::UTF8); }
    json TrackJson(const RemoteTrack& t)
    {
        return {{"id",t.id},{"title",Utf8(t.title)},{"artist",Utf8(t.artist)},{"album",Utf8(t.album)},
            {"genre",Utf8(t.genre)},{"comment",Utf8(t.comment)},{"lyrics",Utf8(t.lyrics)},
            {"content_hash",t.content_hash},{"format",t.format},{"size",t.size},{"path",t.path},
            {"cover_id",t.cover_id},{"cover_mime",t.cover_mime},{"cover_source",t.cover_source},
            {"duration_ms",t.duration_ms},{"bitrate_kbps",t.bitrate_kbps},{"sample_rate",t.sample_rate},
            {"channels",t.channels},{"bits_per_sample",t.bits_per_sample},{"missing",t.unavailable},{"version",t.version}};
    }
    RemoteTrack ReadTrack(const json& j)
    {
        RemoteTrack t;
        t.id=j.value("id",""); t.title=Wide(j.value("title","")); t.artist=Wide(j.value("artist",""));
        t.unavailable=j.value("missing",false) || j.value("removed",false); t.version=j.value("version",0ULL);
        t.album=Wide(j.value("album","")); t.genre=Wide(j.value("genre","")); t.comment=Wide(j.value("comment",""));
        t.lyrics=Wide(j.value("lyrics","")); t.content_hash=j.value("content_hash",""); t.format=j.value("format","");
        t.size=j.value("size",0ULL); t.path=j.value("path",""); t.cover_id=j.value("cover_id","");
        t.cover_mime=j.value("cover_mime",""); t.cover_source=j.value("cover_source","");
        t.duration_ms=j.value("duration_ms",0); t.bitrate_kbps=j.value("bitrate_kbps",0);
        t.sample_rate=j.value("sample_rate",0); t.channels=j.value("channels",0); t.bits_per_sample=j.value("bits_per_sample",0);
        return t;
    }
    void SaveJson(const std::filesystem::path& path, const json& value)
    {
        std::filesystem::create_directories(path.parent_path());
        const auto temporary=path.wstring()+L".tmp";
        std::ofstream out(temporary, std::ios::binary|std::ios::trunc);
        out << value.dump(2); out.close();
        if (!out || !MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("snapshot write failed");
    }
}

CRemoteLibraryManager& CRemoteLibraryManager::Instance() { static CRemoteLibraryManager instance; return instance; }
CRemoteLibraryManager::CRemoteLibraryManager() { m_worker=std::thread([this]{Worker();}); }
CRemoteLibraryManager::~CRemoteLibraryManager() { Shutdown(); }

void CRemoteLibraryManager::Enqueue(std::shared_ptr<RemoteLibraryTask> task, std::function<void()> work, bool priority)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stop) { task->status=L"播放器已退出"; task->done=true; return; }
    if (priority) m_jobs.emplace_front(task,std::move(work));
    else m_jobs.emplace_back(task,std::move(work));
    m_ready.notify_one();
}
void CRemoteLibraryManager::Worker()
{
    for (;;)
    {
        std::pair<std::shared_ptr<RemoteLibraryTask>,std::function<void()>> job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_ready.wait(lock,[this]{return m_stop || !m_jobs.empty();});
            if (m_stop) return;
            job=std::move(m_jobs.front()); m_jobs.pop_front(); m_active=job.first;
        }
        job.first->continuation=false;
        try { if (!*job.first->cancel) job.second(); }
        catch (const std::exception& e) { job.first->status=L"服务器任务失败："+Wide(e.what()); }
        if (*job.first->cancel) { job.first->success=false; job.first->status=L"任务已取消，已完成缓存保留。"; }
        if (!job.first->continuation || *job.first->cancel)
            job.first->done.store(true,std::memory_order_release);
        std::lock_guard<std::mutex> lock(m_mutex); m_active.reset();
    }
}
void CRemoteLibraryManager::Shutdown()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop=true;
        if (m_active) *m_active->cancel=true;
        for (auto& job:m_jobs) { *job.first->cancel=true; job.first->done=true; }
        m_jobs.clear(); m_ready.notify_one();
    }
    if (m_worker.joinable()) m_worker.join();
}

std::shared_ptr<RemoteLibraryTask> CRemoteLibraryManager::Library(bool refresh)
{
    auto client=RemoteLibraryConfig::Load().Client();
    std::shared_ptr<RemoteLibraryTask> task;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_library && m_library_namespace==client.CacheDirectory() && (!refresh || !m_library->done)) return m_library;
        if (m_library && !m_library->done) *m_library->cancel=true;
        m_library_namespace=client.CacheDirectory();
        task=m_library=std::make_shared<RemoteLibraryTask>();
        task->namespace_id=client.CacheDirectory();
        task->local_only=!refresh;
    }
    client.SetCancellation(task->cancel);
    if (refresh) RetryConnection();
    Enqueue(task,[this,task,client,refresh]() mutable {
        if (!client.IsConfigured()) { task->status=L"请先配置服务器地址。"; return; }
        task->success=client.SyncLibrary(task->playlists,task->offline,task->status,!refresh);
        task->access_blocked=client.AccessBlocked();
        if (refresh && task->offline && client.TransportFailed())
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_offline_until[client.CacheDirectory()]=std::chrono::steady_clock::now()+std::chrono::seconds(60);
        }
        if (task->success) task->status=(task->offline ? L"使用已保存的歌单；刷新可重新连接。 " : L"歌单已同步。 ")+task->status;
    });
    return task;
}

void CRemoteLibraryManager::RetryConnection()
{
    std::lock_guard<std::mutex> lock(m_mutex); m_offline_until.clear();
}

std::shared_ptr<RemoteLibraryTask> CRemoteLibraryManager::Prepare(const std::wstring& path, bool allow_network)
{
    Binding binding;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it=m_tracks.find(path); if (it==m_tracks.end()) return {};
        binding=it->second;
    }
    auto task=std::make_shared<RemoteLibraryTask>(); task->path=path; task->local_only=!allow_network;
    binding.client.SetCancellation(task->cancel);
    Enqueue(task,[this,task,binding,allow_network]() mutable {
        if (binding.client.CacheDirectory()!=RemoteLibraryConfig::Load().Client().CacheDirectory())
        { task->status=L"服务器或 API Key 已切换，请重新选择歌单。"; return; }
        task->success=binding.client.EnsureCached(binding.track,task->path,task->status,false);
        if (task->success || !allow_network || *task->cancel) return;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_offline_until[binding.client.CacheDirectory()]>std::chrono::steady_clock::now())
            { task->offline=true; task->status=L"服务器暂时离线，跳过未缓存曲目（刷新可重试）"; return; }
        }
        task->success=binding.client.EnsureCached(binding.track,task->path,task->status);
        if (!task->success && binding.client.TransportFailed() && !*task->cancel)
        {
            task->offline=true;
            std::lock_guard<std::mutex> lock(m_mutex);
            m_offline_until[binding.client.CacheDirectory()]=std::chrono::steady_clock::now()+std::chrono::seconds(60);
        }
    },true);
    return task;
}

std::shared_ptr<RemoteLibraryTask> CRemoteLibraryManager::SaveOffline(const RemotePlaylist& playlist)
{
    auto task=std::make_shared<RemoteLibraryTask>();
    auto client=RemoteLibraryConfig::Load().Client(); client.SetCancellation(task->cancel);
    auto available=std::make_shared<RemotePlaylist>(playlist);
    available->tracks.erase(std::remove_if(available->tracks.begin(),available->tracks.end(),
        [](const RemoteTrack& t){return t.unavailable;}),available->tracks.end());
    SaveOfflineStep(task,client,available,0);
    return task;
}

void CRemoteLibraryManager::SaveOfflineStep(std::shared_ptr<RemoteLibraryTask> task, CRemoteLibraryClient client, std::shared_ptr<RemotePlaylist> playlist, size_t index)
{
    Enqueue(task,[this,task,client,playlist,index] {
        if (client.CacheDirectory()!=RemoteLibraryConfig::Load().Client().CacheDirectory() || !client.PlaylistAllowed(playlist->id))
        { task->status=L"歌单授权或服务器配置已变化，请刷新。"; return; }
        if (index>=playlist->tracks.size())
        {
            task->success=true;
            task->status=L"离线保存完成，共 "+std::to_wstring(index)+L" 首；不可用曲目已跳过。";
            return;
        }
        if (!client.EnsureCached(playlist->tracks[index],task->path,task->status)) return;
        client.RefreshArtwork(playlist->tracks[index]);
        if (*task->cancel) return;
        // One track per job lets playback preparation run between downloads.
        task->continuation=true;
        SaveOfflineStep(task,client,playlist,index+1);
    });
}

std::vector<std::wstring> CRemoteLibraryManager::MaterializedPaths(const RemotePlaylist& playlist) const
{
    std::vector<std::wstring> paths;
    const auto namespace_id = Utf8(RemoteLibraryConfig::Load().Client().CacheDirectory());
    std::error_code directory_error;
    for (const auto& entry : std::filesystem::directory_iterator(theApp.m_playlist_dir, directory_error))
    {
        const auto filename = entry.path().filename().wstring();
        if (filename.rfind(L"server-", 0) != 0 || filename.size() <= 21 ||
            filename.compare(filename.size() - 21, 21, L".playlist.remote.json") != 0)
            continue;
        try
        {
            std::ifstream input(entry.path(), std::ios::binary);
            const auto old_manifest = json::parse(input);
            const int version = old_manifest.value("version", 0);
            if (version == 3 && old_manifest.value("namespace", "") == namespace_id &&
                old_manifest.value("id", "") == playlist.id &&
                (!playlist.id.empty() || old_manifest.value("name", "") == Utf8(playlist.name)))
                paths.push_back(entry.path().wstring().substr(0, entry.path().wstring().size() - 12));
        }
        catch (const std::exception&) {}
    }
    return paths;
}

bool CRemoteLibraryManager::Materialize(const RemotePlaylist& playlist, const std::wstring& preferred_path,
    std::wstring& path, std::wstring& error)
{
    try
    {
        auto client=RemoteLibraryConfig::Load().Client();
        if (!client.PlaylistAllowed(playlist.id)) throw std::runtime_error("playlist no longer authorized; refresh required");
        const std::string name_or_id = playlist.id.empty() ? "name:" + Utf8(playlist.name) : "id:" + playlist.id;
        const std::string identity = Utf8(client.CacheDirectory()) + "\n" + name_or_id;
        const std::wstring stable_path = (std::filesystem::path(theApp.m_playlist_dir) /
            (L"server-" + Wide(CRemoteLibraryClient::HashString(identity)) + L".playlist")).wstring();
        const auto related_paths = MaterializedPaths(playlist);
        if (std::find(related_paths.begin(), related_paths.end(), preferred_path) != related_paths.end())
            path = preferred_path;
        else if (std::find(related_paths.begin(), related_paths.end(), stable_path) != related_paths.end())
            path = stable_path;
        else if (!related_paths.empty())
            path = related_paths.front();
        else
            path = stable_path;
        json manifest={{"version",3},{"namespace",Utf8(client.CacheDirectory())},{"name",Utf8(playlist.name)},{"id",playlist.id},{"tracks",json::array()}};
        std::vector<SongInfo> songs;
        for (const auto& track:playlist.tracks)
        {
            auto cache=client.CachePath(track);
            if (cache.empty()) throw std::runtime_error("invalid track cache identity");
            SongInfo song; song.file_path=cache; songs.push_back(song);
            manifest["tracks"].push_back(TrackJson(track));
        }
        SaveJson(path+L".remote.json",manifest);
        CPlaylistFile::SavePlaylistToFile(songs,path);
        if (!CCommon::FileExist(path)) throw std::runtime_error("playlist write failed");
        if (!LoadPlaylist(path)) throw std::runtime_error("playlist metadata load failed");
        for (const auto& old_path : related_paths)
        {
            if (old_path == path) continue;
            CRecentList::Instance().RemoveItem({ LT_PLAYLIST, old_path });
            DeleteFileW(old_path.c_str());
            DeleteFileW((old_path + L".remote.json").c_str());
        }
        return true;
    }
    catch (const std::exception& e) { error=L"保存播放歌单失败："+Wide(e.what()); return false; }
}

bool CRemoteLibraryManager::LoadPlaylist(const std::wstring& path)
{
    try
    {
        std::ifstream in(path+L".remote.json",std::ios::binary);
        if (!in) return false;
        auto manifest=json::parse(in);
        auto client=RemoteLibraryConfig::Load().Client();
        const int version = manifest.value("version", 0);
        if (version != 3 || manifest.value("namespace","")!=Utf8(client.CacheDirectory()) || !client.PlaylistAllowed(manifest.value("id",""))) return false;
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& value:manifest.at("tracks"))
        {
            auto track=ReadTrack(value); auto cache=client.CachePath(track);
            if (!cache.empty()) m_tracks[cache]={client,std::move(track)};
        }
        return true;
    }
    catch (const std::exception&) { return false; }
}

bool CRemoteLibraryManager::PlaylistUsable(const std::wstring& path) const
{
    if (!CCommon::FileExist(path+L".remote.json")) return true;
    try
    {
        std::ifstream in(path+L".remote.json",std::ios::binary);
        const auto manifest=json::parse(in);
        auto client=RemoteLibraryConfig::Load().Client();
        return manifest.value("version",0)==3 && manifest.value("namespace","")==Utf8(client.CacheDirectory()) &&
            client.PlaylistAllowed(manifest.value("id",""));
    }
    catch (const std::exception&) { return false; }
}

void CRemoteLibraryManager::RemoveUnavailableRecentPlaylists()
{
    std::error_code ignored;
    for (const auto& entry : std::filesystem::directory_iterator(theApp.m_playlist_dir,ignored))
    {
        const auto filename=entry.path().wstring();
        const std::wstring suffix=L".playlist.remote.json";
        if (filename.size()<suffix.size() || filename.compare(filename.size()-suffix.size(),suffix.size(),suffix)!=0) continue;
        const auto path=filename.substr(0,filename.size()-12);
        if (!PlaylistUsable(path)) CRecentList::Instance().RemoveItem({LT_PLAYLIST,path});
    }
    CRecentList::Instance().SaveData();
}

bool CRemoteLibraryManager::ApplyMetadata(SongInfo& song)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it=m_tracks.find(song.file_path); if (it==m_tracks.end()) return false;
    const auto& t=it->second.track;
    song.title=t.title; song.artist=t.artist; song.album=t.album; song.genre=t.genre; song.comment=t.comment;
    if (t.duration_ms>0) song.end_pos.fromInt(t.duration_ms);
    if (t.bitrate_kbps>0) song.bitrate=static_cast<short>(t.bitrate_kbps);
    if (t.sample_rate>0) song.freq=t.sample_rate;
    if (t.channels>0) song.channels=static_cast<BYTE>(t.channels);
    if (t.bits_per_sample>0) song.bits=static_cast<BYTE>(t.bits_per_sample);
    song.lyric_file.clear();
    if (!t.lyrics.empty()) { auto lrc=std::filesystem::path(song.file_path); lrc.replace_extension(L".lrc"); song.lyric_file=lrc.wstring(); }
    return true;
}
bool CRemoteLibraryManager::IsRemote(const std::wstring& path)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_tracks.find(path)!=m_tracks.end();
}
std::wstring CRemoteLibraryManager::TrackName(const std::wstring& path)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it=m_tracks.find(path); if (it==m_tracks.end()) return {};
    const auto& t=it->second.track;
    auto title=t.title.empty() ? std::filesystem::path(Wide(t.path)).stem().wstring() : t.title;
    if (title.empty()) title=L"未命名曲目";
    return (t.artist.empty() ? title : t.artist+L" - "+title)+L"."+Wide(t.format);
}
std::wstring CRemoteLibraryManager::PlaylistName(const std::wstring& path)
{
    // Recent-list rendering must not rebind current playback metadata to an old snapshot.
    try
    {
        std::ifstream in(path+L".remote.json",std::ios::binary);
        if (!in) return {};
        auto manifest=json::parse(in);
        const int version = manifest.value("version",0);
        auto client=RemoteLibraryConfig::Load().Client();
        if (version != 3 || manifest.value("namespace","")!=Utf8(client.CacheDirectory()) || !client.PlaylistAllowed(manifest.value("id",""))) return L"不可用的服务器歌单";
        auto name=Wide(manifest.value("name",""));
        return name;
    }
    catch (const std::exception&) { return {}; }
}
RemoteBrowseState CRemoteLibraryManager::BrowseState()
{
    RemoteBrowseState state;
    try
    {
        auto client=RemoteLibraryConfig::Load().Client();
        std::ifstream in(std::filesystem::path(client.CacheDirectory())/L"browse.json");
        if (in) { auto j=json::parse(in); state.playlist_id=j.value("playlist_id",""); state.track_id=j.value("track_id",""); state.top_row=j.value("top_row",0); }
    }
    catch (const std::exception&) {}
    return state;
}
void CRemoteLibraryManager::SaveBrowseState(const RemoteBrowseState& state)
{
    try { SaveJson(std::filesystem::path(RemoteLibraryConfig::Load().Client().CacheDirectory())/L"browse.json",
        {{"playlist_id",state.playlist_id},{"track_id",state.track_id},{"top_row",state.top_row}}); }
    catch (const std::exception&) { /* Browsing state is optional; playback remains unaffected. */ }
}
