#include "stdafx.h"
#include "RemoteLibraryManager.h"
#include "MusicPlayer2.h"
#include "Playlist.h"
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
            {"channels",t.channels},{"bits_per_sample",t.bits_per_sample}};
    }
    RemoteTrack ReadTrack(const json& j)
    {
        RemoteTrack t;
        t.id=j.value("id",""); t.title=Wide(j.value("title","")); t.artist=Wide(j.value("artist",""));
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
        task->local_only=!refresh;
    }
    client.SetCancellation(task->cancel);
    if (refresh) RetryConnection();
    Enqueue(task,[this,task,client,refresh]() mutable {
        if (!client.IsConfigured()) { task->status=L"请先配置服务器地址。"; return; }
        task->success=client.SyncLibrary(task->playlists,task->offline,task->status,!refresh);
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
    SaveOfflineStep(task,client,std::make_shared<RemotePlaylist>(playlist),0);
    return task;
}

void CRemoteLibraryManager::SaveOfflineStep(std::shared_ptr<RemoteLibraryTask> task, CRemoteLibraryClient client, std::shared_ptr<RemotePlaylist> playlist, size_t index)
{
    Enqueue(task,[this,task,client,playlist,index] {
        if (index>=playlist->tracks.size())
        {
            task->success=true;
            task->status=L"离线保存完成，共 "+std::to_wstring(index)+L" 首。";
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

bool CRemoteLibraryManager::Materialize(const RemotePlaylist& playlist, std::wstring& path, std::wstring& error)
{
    try
    {
        auto client=RemoteLibraryConfig::Load().Client();
        json manifest={{"version",1},{"namespace",Utf8(client.CacheDirectory())},{"name",Utf8(playlist.name)},{"id",playlist.id},{"tracks",json::array()}};
        std::vector<SongInfo> songs;
        for (const auto& track:playlist.tracks)
        {
            auto cache=client.CachePath(track);
            if (cache.empty()) throw std::runtime_error("invalid track cache identity");
            SongInfo song; song.file_path=cache; songs.push_back(song);
            manifest["tracks"].push_back(TrackJson(track));
        }
        // Each snapshot has its own file, so updates preserve the active queue.
        // The player saves the old queue before materializing even the same snapshot.
        auto name=CRemoteLibraryClient::HashString(manifest.dump());
        path=(std::filesystem::path(theApp.m_playlist_dir)/(L"server-"+Wide(name)+L".playlist")).wstring();
        SaveJson(path+L".remote.json",manifest);
        CPlaylistFile::SavePlaylistToFile(songs,path);
        if (!CCommon::FileExist(path)) throw std::runtime_error("playlist write failed");
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
        if (manifest.value("version",0)!=1 || manifest.value("namespace","")!=Utf8(client.CacheDirectory())) return false;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_names[path]=Wide(manifest.value("name",""));
        for (const auto& value:manifest.at("tracks"))
        {
            auto track=ReadTrack(value); auto cache=client.CachePath(track);
            if (!cache.empty()) m_tracks[cache]={client,std::move(track)};
        }
        return true;
    }
    catch (const std::exception&) { return false; }
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
    { std::lock_guard<std::mutex> lock(m_mutex); auto it=m_names.find(path); if (it!=m_names.end()) return it->second; }
    // Recent-list rendering must not rebind current playback metadata to an old snapshot.
    try
    {
        std::ifstream in(path+L".remote.json",std::ios::binary);
        if (!in) return {};
        auto manifest=json::parse(in);
        if (manifest.value("version",0)!=1) return {};
        auto name=Wide(manifest.value("name",""));
        std::lock_guard<std::mutex> lock(m_mutex); m_names[path]=name;
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
