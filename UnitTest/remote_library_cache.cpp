#include <windows.h>
#include <atlimage.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <array>
#include "nlohmann/json.hpp"

#include "../MusicPlayer2/RemoteLibraryClient.h"

namespace
{
    std::string Utf8(const std::wstring& value)
    {
        const auto size=WideCharToMultiByte(CP_UTF8,0,value.data(),int(value.size()),nullptr,0,nullptr,nullptr);
        std::string result(size,'\0');
        if (size) WideCharToMultiByte(CP_UTF8,0,value.data(),int(value.size()),result.data(),size,nullptr,nullptr);
        return result;
    }
    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    bool Exists(const std::filesystem::path& audio, const wchar_t* extension)
    {
        auto path = audio;
        path.replace_extension(extension);
        return std::filesystem::is_regular_file(path);
    }

    std::string Read(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    void VerifyCover(const std::filesystem::path& audio, const wchar_t* extension, const std::string& hash)
    {
        auto cover = audio;
        cover.replace_extension(extension);
        Require(CRemoteLibraryClient::HashString(Read(cover)) == hash, "cover bytes did not match server hash");
        ATL::CImage image;
        Require(SUCCEEDED(image.Load(cover.c_str())) && !image.IsNull(), "player image decoder could not load cached cover");
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc==2 && (std::wstring(argv[1])==L"--server" || std::wstring(argv[1])==L"--check"))
    {
        using json=nlohmann::json;
        try
        {
            json request; std::cin >> request;
            const auto url=request.at("url").get<std::string>();
            CRemoteLibraryClient client;
            client.Configure(std::wstring(url.begin(),url.end()),request.at("key").get<std::string>(),
                std::filesystem::u8path(request.at("cache").get<std::string>()).wstring());
            if (std::wstring(argv[1])==L"--check")
            {
                std::wstring error;
                const bool success=client.CheckConnection(error);
                std::cout << json({{"success",success},{"error",Utf8(error)}}).dump() << '\n';
                return 0;
            }
            std::vector<RemotePlaylist> playlists;
            std::wstring error; bool offline=false;
            const bool ok=client.SyncLibrary(playlists,offline,error,request.value("local_only",false));
            json result={{"success",ok},{"offline",offline},{"blocked",client.AccessBlocked()},
                {"error",Utf8(error)},{"playlists",json::array()}};
            for (const auto& p:playlists)
            {
                json playlist={{"id",p.id},{"track_ids",p.track_ids},{"notes",json::object()},{"tracks",json::array()},
                    {"read_only",p.read_only},{"subscription_status",p.subscription_status},
                    {"subscription_reason",p.subscription_reason},{"allowed",client.PlaylistAllowed(p.id)},
                    {"visibility",p.visibility},{"reason_text",p.Available()?"":Utf8(p.UnavailableReason())}};
                for (const auto& note:p.track_notes) playlist["notes"][note.first]=Utf8(note.second);
                for (const auto& t:p.tracks)
                {
                    json track={{"id",t.id},{"unavailable",t.unavailable},{"cover",t.cover_id},{"lyrics",!t.lyrics.empty()}};
                    if (request.value("download",false))
                    {
                        std::wstring path;
                        track["cached"]=client.EnsureCached(t,path,error,!request.value("local_only",false));
                        if (track["cached"]==true)
                        {
                            std::ifstream input(std::filesystem::path(path),std::ios::binary);
                            track["hash"]=CRemoteLibraryClient::HashString(std::string(std::istreambuf_iterator<char>(input),{}));
                            auto cover=std::filesystem::path(path); cover.replace_extension(L".png");
                            track["artwork"]=std::filesystem::is_regular_file(cover);
                            auto lyrics=std::filesystem::path(path); lyrics.replace_extension(L".lrc");
                            track["lyric_file"]=std::filesystem::is_regular_file(lyrics);
                        }
                    }
                    playlist["tracks"].push_back(track);
                }
                result["playlists"].push_back(playlist);
            }
            std::cout << result.dump() << '\n';
            return 0;
        }
        catch(const std::exception& e) { std::cerr << e.what(); return 1; }
    }
    if (argc != 3) return 2;
    try
    {
        CRemoteLibraryClient client;
        client.Configure(argv[1], "fixture-key", argv[2]);
        std::array<bool, 4> connected{};
        std::array<std::thread, 4> requests;
        std::atomic<int> ready{0};
        std::atomic<bool> start{false};
        for (size_t i = 0; i < requests.size(); ++i)
            requests[i] = std::thread([&, i] {
                CRemoteLibraryClient connection;
                connection.Configure(argv[1], "fixture-key", argv[2]);
                ++ready;
                while (!start) std::this_thread::yield();
                std::wstring connection_error;
                connected[i] = connection.CheckConnection(connection_error);
            });
        while (ready != requests.size()) std::this_thread::yield();
        start = true;
        for (auto& request : requests) request.join();
        for (bool success : connected) Require(success, "connection check failed");
        RemoteTrack track;
        std::wstring path, error;
        auto Select = [&](const char* mode) {
            Require(client.GetTrack(std::string("fixture?mode=") + mode, track, error), "could not select fixture track");
            Require(track.id == "fixture", "fixture track parsing failed");
        };
        auto Cache = [&] { Require(client.EnsureCached(track, path, error), "optional artwork blocked audio cache"); };

        auto Requests = [&] {
            RemoteTrack stats;
            Require(client.GetTrack("fixture?mode=stats",stats,error),"could not read request counters");
            return std::stoull(stats.path);
        };
        auto RefreshCover = [&] { client.RefreshArtwork(track); Cache(); };

        Select("png");
        Require(track.duration_ms==12345 && track.bitrate_kbps==320 && track.sample_rate==44100
            && track.channels==2 && track.bits_per_sample==16,"technical metadata missing");
        auto before=Requests();
        Require(!client.EnsureCached(track,path,error,false),"uncached local-only track accepted");
        Require(Requests()==before,"local-only cache miss sent HTTP");
        Require(track.cover_mime == "image/png" && track.cover_source == "external", "cover metadata not parsed");
        Cache();
        VerifyCover(path, L".png", track.cover_id);
        auto lyrics = std::filesystem::path(path);
        lyrics.replace_extension(L".lrc");
        Require(Read(lyrics) == "\xef\xbb\xbf[00:01.00]fixture lyrics", "lyrics cache behavior changed");

        before=Requests();
        Cache();
        Require(client.EnsureCached(track,path,error,false),"local-only cache hit failed");
        Require(Requests()==before,"verified cache hit sent HTTP");
        auto cancel=std::make_shared<std::atomic<bool>>(true);
        auto other=track; other.content_hash=CRemoteLibraryClient::HashString("other");
        client.SetCancellation(cancel);
        Require(!client.EnsureCached(other,path,error),"cancelled miss succeeded");
        client.SetCancellation({});
        Require(Requests()==before,"cancelled cache miss sent HTTP");
        Cache();
        auto audio=std::filesystem::path(path);
        const auto original=Read(audio);
        std::ofstream(audio,std::ios::binary|std::ios::trunc)<<std::string(original.size(),'x');
        before=Requests();
        Require(!client.EnsureCached(track,path,error,false),"corrupt same-size audio accepted");
        Require(Requests()==before,"corrupt local-only cache sent HTTP");
        Cache();
        Require(Read(audio)==original && Requests()>before,"corrupt audio was not downloaded again");

        std::vector<RemotePlaylist> playlists;
        bool offline = false;
        Require(client.SyncLibrary(playlists, offline, error) && !offline, "online snapshot sync failed");
        Require(playlists.size() == 1 && playlists[0].tracks[0].cover_id == track.cover_id, "snapshot did not preserve cover metadata");

        before=Requests();
        Require(client.SyncLibrary(playlists,offline,error,true) && offline,"local snapshot failed");
        Require(Requests()==before,"local snapshot sent HTTP");

        auto original_cover=track.cover_id;
        Select("png2");
        Require(client.SyncLibrary(playlists, offline, error) && !offline, "updated snapshot sync failed");
        VerifyCover(path,L".png",original_cover);
        Cache();
        VerifyCover(path,L".png",track.cover_id);

        auto cover = std::filesystem::path(path);
        cover.replace_extension(L".png");
        std::ofstream(cover, std::ios::binary | std::ios::trunc) << "corrupt";
        Cache();
        VerifyCover(path, L".png", track.cover_id);

        Select("gif");
        RefreshCover();
        Require(!Exists(path, L".png"), "old image extension was retained");
        VerifyCover(path, L".gif", track.cover_id);

        Select("corrupt");
        RefreshCover();
        Require(!Exists(path, L".png") && !Exists(path, L".gif"), "hash mismatch retained stale artwork");
        Select("oversized");
        RefreshCover();
        Require(!Exists(path, L".png"), "oversized artwork was accepted");
        Select("denied");
        RefreshCover();
        Require(!Exists(path, L".png"), "denied artwork remained cached");

        Select("png");
        Cache();
        Select("none");
        Require(client.SyncLibrary(playlists, offline, error) && !offline, "cover removal sync failed");
        Cache();
        Require(!Exists(path, L".png") && !Exists(path, L".gif"), "cover removal left stale artwork");

        Select("png");
        Cache();
        Require(client.SyncLibrary(playlists, offline, error) && !offline, "snapshot before track removal failed");
        Select("removed");
        Require(client.SyncLibrary(playlists, offline, error) && !offline, "track removal sync failed");
        Require(Exists(path, L".png") && std::filesystem::is_regular_file(path), "track removal deleted an active snapshot sidecar");

        Select("png");
        Require(client.SyncLibrary(playlists, offline, error) && !offline, "snapshot for offline case failed");
        Cache();

        auto Sync = [&](const char* mode) {
            Select(mode);
            return client.SyncLibrary(playlists, offline, error, false, false);
        };
        auto snapshot = std::filesystem::path(client.CacheDirectory()) / L"library.json";
        Require(Sync("case-paged") && !offline, "paged reset snapshot failed");
        Require(playlists.size()==2 && playlists[0].id=="q" && playlists[1].id=="p", "playlist ordering lost");
        auto p=playlists[1];
        Require(p.track_ids==std::vector<std::string>({"b","a"}) && p.tracks.size()==2 &&
            p.track_notes.at("a")==L"keep note", "cross-page members, order or notes lost");
        Require(client.CachePath(p.tracks[0])!=client.CachePath(p.tracks[1]), "UUIDs collapsed by matching content hash");
        Require(Sync("case-delta") && playlists.size()==1 && playlists[0].name==L"Renamed", "delta playlist upsert/delete failed");
        Require(playlists[0].tracks[0].title==L"Updated" && playlists[0].tracks[1].unavailable &&
            playlists[0].track_notes.at("b")==L"retained", "delta tombstone lost member or note");
        std::wstring ignored;
        before=Requests();
        Require(!client.EnsureCached(playlists[0].tracks[1],ignored,error), "unavailable member downloaded");
        Require(Requests()==before, "unavailable member sent HTTP");
        Require(Sync("case-restore") && !playlists[0].tracks[1].unavailable, "delta restoration failed");
        const auto committed=Read(snapshot);
        for (const auto mode : {"case-bad-last","case-bad-json","case-cycle","case-restart-always","case-limited"})
        {
            Require(!Sync(mode) && !offline, "protocol or HTTP failure treated as offline success");
            Require(Read(snapshot)==committed, "failed pagination changed committed snapshot/cursor");
        }
        Require(Sync("case-network-last") && offline, "mid-page network outage did not use committed snapshot");
        Require(Read(snapshot)==committed && playlists[0].tracks[0].title==L"Updated", "partial page leaked into offline snapshot");
        Require(Sync("case-restart") && playlists.size()==2, "bounded snapshot restart failed");
        for (const auto mode : {"case-unauthorized","case-forbidden"})
        {
            Require(!Sync(mode) && !offline && client.AccessBlocked(), "authorization failure used offline data");
            Require(!client.SyncLibrary(playlists,offline,error,true), "known denied key loaded offline snapshot");
            CRemoteLibraryClient restarted;
            restarted.Configure(argv[1],"fixture-key",argv[2]);
            Require(restarted.AccessBlocked(), "authorization denial did not persist across restart");
            Require(Sync("case-paged") && !client.AccessBlocked(), "successful sync failed to revalidate key");
        }
        Require(Sync("case-empty") && playlists.empty(), "empty grants did not reset old library");
        Require(!client.TrackAllowed(p.tracks[0]) && !client.PlaylistAllowed("p"), "old granted entry remained usable after reset");
        cancel=std::make_shared<std::atomic<bool>>(true);
        before=Requests();
        client.SetCancellation(cancel);
        Require(!client.SyncLibrary(playlists,offline,error), "cancelled sync succeeded");
        client.SetCancellation({});
        Require(Requests()==before, "cancelled sync sent HTTP");
        CRemoteLibraryClient other_key;
        other_key.Configure(argv[1],"other-key",argv[2]);
        Require(other_key.CacheDirectory()!=client.CacheDirectory(), "key namespaces overlap");
        Select("png");
        Require(client.SyncLibrary(playlists,offline,error), "restore fixture snapshot after protocol tests failed");
        Cache();
        std::cout << "PASS v2 pagination, deltas, reset, UUID identity, notes, tombstones, retry bounds, atomicity, errors and authorization isolation\n";
        Select("stop");
        Sleep(250);
        Require(client.SyncLibrary(playlists, offline, error) && offline, "offline snapshot unavailable");
        Require(playlists.size() == 1 && playlists[0].tracks[0].cover_id == track.cover_id, "offline artwork metadata missing");
        track = playlists[0].tracks[0];
        Cache();
        VerifyCover(path, L".png", track.cover_id);
        Require(std::filesystem::is_regular_file(path), "audio cache was removed");
        std::wcout << L"Remote cache checks passed: zero-request hits/misses, cancellation, SHA-256 repair, technical metadata, artwork snapshots, limits, and offline reuse.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
