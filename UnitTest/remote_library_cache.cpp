#include <windows.h>
#include <atlimage.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include "../MusicPlayer2/RemoteLibraryClient.h"

namespace
{
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
    if (argc != 3) return 2;
    try
    {
        CRemoteLibraryClient client;
        client.Configure(argv[1], "fixture-key", argv[2]);
        RemoteTrack track;
        std::wstring path, error;
        auto Select = [&](const char* mode) {
            Require(client.GetTrack(std::string("fixture?mode=") + mode, track, error), "could not select fixture track");
            Require(track.id == "fixture", "fixture track parsing failed");
        };
        auto Cache = [&] { Require(client.EnsureCached(track, path, error), "optional artwork blocked audio cache"); };

        Select("png");
        Require(track.cover_mime == "image/png" && track.cover_source == "external", "cover metadata not parsed");
        Cache();
        VerifyCover(path, L".png", track.cover_id);
        auto lyrics = std::filesystem::path(path);
        lyrics.replace_extension(L".lrc");
        Require(Read(lyrics) == "\xef\xbb\xbf[00:01.00]fixture lyrics", "lyrics cache behavior changed");

        std::vector<RemotePlaylist> playlists;
        bool offline = false;
        Require(client.SyncLibrary(playlists, offline, error) && !offline, "online snapshot sync failed");
        Require(playlists.size() == 1 && playlists[0].tracks[0].cover_id == track.cover_id, "snapshot did not preserve cover metadata");

        Select("png2");
        Require(client.SyncLibrary(playlists, offline, error) && !offline, "updated snapshot sync failed");
        VerifyCover(path, L".png", track.cover_id);

        auto cover = std::filesystem::path(path);
        cover.replace_extension(L".png");
        std::ofstream(cover, std::ios::binary | std::ios::trunc) << "corrupt";
        Cache();
        VerifyCover(path, L".png", track.cover_id);

        Select("gif");
        Cache();
        Require(!Exists(path, L".png"), "old image extension was retained");
        VerifyCover(path, L".gif", track.cover_id);

        Select("corrupt");
        Cache();
        Require(!Exists(path, L".png") && !Exists(path, L".gif"), "hash mismatch retained stale artwork");
        Select("oversized");
        Cache();
        Require(!Exists(path, L".png"), "oversized artwork was accepted");
        Select("denied");
        Cache();
        Require(!Exists(path, L".png"), "denied artwork remained cached");

        Select("png");
        Cache();
        Select("none");
        Require(client.SyncLibrary(playlists, offline, error) && !offline, "cover removal sync failed");
        Require(!Exists(path, L".png") && !Exists(path, L".gif"), "cover removal left stale artwork");

        Select("png");
        Cache();
        Require(client.SyncLibrary(playlists, offline, error) && !offline, "snapshot before track removal failed");
        Select("removed");
        Require(client.SyncLibrary(playlists, offline, error) && !offline, "track removal sync failed");
        Require(!Exists(path, L".png") && std::filesystem::is_regular_file(path), "track removal retained artwork or removed audio");

        Select("png");
        Cache();
        Require(client.SyncLibrary(playlists, offline, error) && !offline, "snapshot for offline case failed");
        Select("stop");
        Sleep(250);
        Require(client.SyncLibrary(playlists, offline, error) && offline, "offline snapshot unavailable");
        Require(playlists.size() == 1 && playlists[0].tracks[0].cover_id == track.cover_id, "offline artwork metadata missing");
        track = playlists[0].tracks[0];
        Cache();
        VerifyCover(path, L".png", track.cover_id);
        Require(std::filesystem::is_regular_file(path), "audio cache was removed");
        std::wcout << L"Remote cache checks passed: authenticated artwork, hash verification, refresh, removal, limits, and offline reuse.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
