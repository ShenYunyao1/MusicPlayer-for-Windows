#include "stdafx.h"
#include "ServerLibraryDlg.h"
#include "MusicPlayer2.h"
#include "Player.h"
#include "Common.h"

namespace { constexpr UINT WM_SERVER_LIBRARY_DONE = WM_APP + 702; }

BEGIN_MESSAGE_MAP(CServerLibraryDlg, CTabDlg)
    ON_BN_CLICKED(IDC_SERVER_REFRESH, &CServerLibraryDlg::OnRefresh)
    ON_BN_CLICKED(IDC_SERVER_DOWNLOAD, &CServerLibraryDlg::OnDownload)
    ON_BN_CLICKED(IDC_SERVER_PLAY, &CServerLibraryDlg::OnOK)
    ON_BN_CLICKED(IDC_SERVER_CONFIGURE, &CServerLibraryDlg::OnConfigure)
    ON_BN_CLICKED(IDC_SERVER_CANCEL, &CServerLibraryDlg::OnCancelTask)
    ON_LBN_SELCHANGE(IDC_SERVER_PLAYLISTS, &CServerLibraryDlg::OnPlaylist)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_SERVER_TRACKS, &CServerLibraryDlg::OnTrackChanged)
    ON_NOTIFY(NM_DBLCLK, IDC_SERVER_TRACKS, &CServerLibraryDlg::OnDoubleClick)
    ON_MESSAGE(WM_SERVER_LIBRARY_DONE, &CServerLibraryDlg::OnFinished)
    ON_WM_DESTROY()
    ON_WM_SIZE()
END_MESSAGE_MAP()

CServerLibraryDlg::~CServerLibraryDlg()
{
    if (m_cancel) *m_cancel = true;
    if (m_worker.joinable()) m_worker.join();
}

BOOL CServerLibraryDlg::OnInitDialog()
{
    CTabDlg::OnInitDialog();
    m_playlist_list.SubclassDlgItem(IDC_SERVER_PLAYLISTS, this);
    m_track_list.SubclassDlgItem(IDC_SERVER_TRACKS, this);
    m_track_list.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    m_track_list.InsertColumn(0, L"曲目", LVCFMT_LEFT, theApp.DPI(210));
    m_track_list.InsertColumn(1, L"歌手", LVCFMT_LEFT, theApp.DPI(130));
    m_track_list.InsertColumn(2, L"专辑", LVCFMT_LEFT, theApp.DPI(140));
    m_track_list.InsertColumn(3, L"风格", LVCFMT_LEFT, theApp.DPI(80));
    SetDlgItemTextW(IDC_SERVER_REFRESH, L"刷新");
    SetDlgItemTextW(IDC_SERVER_DOWNLOAD, L"离线保存歌单");
    SetDlgItemTextW(IDC_SERVER_PLAY, L"播放选中");
    SetDlgItemTextW(IDC_SERVER_CONFIGURE, L"服务器设置");
    SetDlgItemTextW(IDC_SERVER_CANCEL, L"取消任务");
    SetDlgItemTextW(IDC_SERVER_STATUS, L"请先在设置中配置服务器，再刷新歌单。");
    GetDlgItem(IDC_SERVER_CANCEL)->EnableWindow(FALSE);
    CRect rect; GetClientRect(rect); Layout(rect.Width(), rect.Height());
    return TRUE;
}

void CServerLibraryDlg::OnTabEntered()
{
    const auto config = RemoteLibraryConfig::Load();
    const auto signature = CRemoteLibraryClient::HashString(CCommon::UnicodeToStr(config.url, CodeType::UTF8_NO_BOM) + "\n" + config.api_key);
    if (!m_worker.joinable() && (m_playlists.empty() || m_loaded_signature != signature)) OnRefresh();
    if (auto parent = GetParentWindow()) parent->SendMessage(WM_PLAY_SELECTED_BTN_ENABLE, !m_worker.joinable() && m_track_list.GetSelectedCount() != 0);
}

void CServerLibraryDlg::Busy(bool value)
{
    for (int id : {IDC_SERVER_REFRESH, IDC_SERVER_DOWNLOAD, IDC_SERVER_PLAY, IDC_SERVER_CONFIGURE, IDC_SERVER_PLAYLISTS, IDC_SERVER_TRACKS}) GetDlgItem(id)->EnableWindow(!value);
    GetDlgItem(IDC_SERVER_CANCEL)->EnableWindow(value);
    if (auto parent = GetParentWindow()) parent->SendMessage(WM_PLAY_SELECTED_BTN_ENABLE, !value && m_track_list.GetSelectedCount() != 0);
}

void CServerLibraryDlg::OnRefresh()
{
    if (m_worker.joinable()) return;
    const auto config = RemoteLibraryConfig::Load();
    auto client = config.Client();
    if (!client.IsConfigured()) { SetDlgItemTextW(IDC_SERVER_STATUS, L"请先打开服务器设置，填写地址并应用。"); return; }
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    client.SetCancellation(m_cancel);
    m_sync_task = true;
    m_play_task = false;
    const auto signature = CRemoteLibraryClient::HashString(CCommon::UnicodeToStr(config.url, CodeType::UTF8_NO_BOM) + "\n" + config.api_key);
    if (m_loaded_signature != signature) { m_playlists.clear(); RenderPlaylists(); }
    m_loaded_signature = signature;
    Busy(true);
    SetDlgItemTextW(IDC_SERVER_STATUS, L"正在检查歌单更新…");
    const HWND window = GetSafeHwnd();
    m_worker = std::thread([this, client, window] {
        try
        {
            bool offline{};
            std::wstring error;
            m_success = client.SyncLibrary(m_result_playlists, offline, error);
            m_result_status = m_success ? (offline ? L"离线模式，使用已保存的歌单。" : L"歌单已同步。") : L"同步失败。";
            if (!error.empty()) m_result_status += L" " + error;
        }
        catch (const std::exception&) { m_success = false; m_result_status = L"同步发生异常"; }
        ::PostMessageW(window, WM_SERVER_LIBRARY_DONE, 0, 0);
    });
}

void CServerLibraryDlg::RenderPlaylists()
{
    m_playlist_list.ResetContent();
    for (const auto& playlist : m_playlists) m_playlist_list.AddString(playlist.name.c_str());
    if (!m_playlists.empty()) m_playlist_list.SetCurSel((std::max)(0, (std::min)(m_selected_playlist, static_cast<int>(m_playlists.size()) - 1)));
    OnPlaylist();
}

void CServerLibraryDlg::OnPlaylist()
{
    m_track_list.DeleteAllItems();
    SetDlgItemTextW(IDC_SERVER_NOTES, L"");
    m_selected_playlist = m_playlist_list.GetCurSel();
    if (m_selected_playlist < 0 || m_selected_playlist >= static_cast<int>(m_playlists.size())) return;
    const auto& playlist = m_playlists[m_selected_playlist];
    SetDlgItemTextW(IDC_SERVER_NOTES, playlist.description.c_str());
    int row = 0;
    for (const auto& track : playlist.tracks)
    {
        m_track_list.InsertItem(row, track.title.c_str());
        m_track_list.SetItemText(row, 1, track.artist.c_str());
        m_track_list.SetItemText(row, 2, track.album.c_str());
        m_track_list.SetItemText(row, 3, track.genre.c_str());
        ++row;
    }
}

void CServerLibraryDlg::OnTrackChanged(NMHDR*, LRESULT* result)
{
    *result = 0;
    if (auto parent = GetParentWindow()) parent->SendMessage(WM_PLAY_SELECTED_BTN_ENABLE, !m_worker.joinable() && m_track_list.GetSelectedCount() != 0);
    if (m_selected_playlist < 0 || m_selected_playlist >= static_cast<int>(m_playlists.size())) return;
    const auto& playlist = m_playlists[m_selected_playlist];
    int row = m_track_list.GetNextItem(-1, LVNI_SELECTED);
    std::wstring note = playlist.description;
    if (row >= 0 && row < static_cast<int>(playlist.tracks.size()))
    {
        const auto& track = playlist.tracks[row];
        note += L"  " + track.comment;
        auto found = playlist.track_notes.find(track.id);
        if (found != playlist.track_notes.end()) note += L"  " + found->second;
    }
    SetDlgItemTextW(IDC_SERVER_NOTES, note.c_str());
}

void CServerLibraryDlg::Download(bool play)
{
    if (m_worker.joinable() || m_selected_playlist < 0 || m_selected_playlist >= static_cast<int>(m_playlists.size())) return;
    std::vector<RemoteTrack> tracks;
    const auto& playlist = m_playlists[m_selected_playlist];
    if (play)
    {
        for (int row = m_track_list.GetNextItem(-1, LVNI_SELECTED); row >= 0; row = m_track_list.GetNextItem(row, LVNI_SELECTED))
            if (row < static_cast<int>(playlist.tracks.size())) tracks.push_back(playlist.tracks[row]);
    }
    else tracks = playlist.tracks;
    if (tracks.empty()) { SetDlgItemTextW(IDC_SERVER_STATUS, L"请先选择曲目，或选择包含曲目的歌单。"); return; }
    auto client = RemoteLibraryConfig::Load().Client();
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    client.SetCancellation(m_cancel);
    m_sync_task = false;
    m_play_task = play;
    Busy(true);
    SetDlgItemTextW(IDC_SERVER_STATUS, L"正在下载并校验缓存，完成后即可播放…");
    const HWND window = GetSafeHwnd();
    m_worker = std::thread([this, client, tracks, window] {
        m_result_files.clear();
        m_success = true;
        try
        {
            for (const auto& track : tracks)
            {
                std::wstring path, error;
                if (*m_cancel || !client.EnsureCached(track, path, error))
                {
                    m_success = false;
                    m_result_status = *m_cancel ? L"任务已取消，已完成的缓存保留。" : L"下载失败：" + error;
                    break;
                }
                m_result_files.push_back(path);
            }
            if (m_success) m_result_status = L"缓存完成，共 " + std::to_wstring(m_result_files.size()) + L" 首曲目。";
        }
        catch (const std::exception&) { m_success = false; m_result_status = L"缓存任务发生异常"; }
        ::PostMessageW(window, WM_SERVER_LIBRARY_DONE, 0, 0);
    });
}

void CServerLibraryDlg::OnDownload() { Download(false); }
void CServerLibraryDlg::OnOK() { Download(true); }
void CServerLibraryDlg::OnDoubleClick(NMHDR*, LRESULT* result) { *result = 0; OnOK(); }
void CServerLibraryDlg::OnCancelTask() { if (m_cancel) *m_cancel = true; }
void CServerLibraryDlg::OnConfigure()
{
    theApp.m_pMainWnd->SendMessage(WM_OPTION_SETTINGS, 6, LPARAM(GetParentWindow()));
    if (IsWindow(GetSafeHwnd())) OnRefresh();
}

LRESULT CServerLibraryDlg::OnFinished(WPARAM, LPARAM)
{
    if (m_worker.joinable()) m_worker.join();
    if (m_success && m_sync_task) { m_playlists = std::move(m_result_playlists); RenderPlaylists(); }
    SetDlgItemTextW(IDC_SERVER_STATUS, m_result_status.c_str());
    Busy(false);
    if (m_success && m_play_task && !m_result_files.empty())
        if (!CPlayer::GetInstance().OpenFilesInDefaultPlaylist(m_result_files, true)) SetDlgItemTextW(IDC_SERVER_STATUS, L"缓存成功，但播放列表打开失败。");
    return 0;
}

void CServerLibraryDlg::OnDestroy()
{
    if (m_cancel) *m_cancel = true;
    if (m_worker.joinable()) m_worker.join();
    CTabDlg::OnDestroy();
}

void CServerLibraryDlg::OnSize(UINT type, int width, int height)
{
    CTabDlg::OnSize(type, width, height);
    Layout(width, height);
}

void CServerLibraryDlg::Layout(int width, int height)
{
    if (!m_track_list.GetSafeHwnd()) return;
    const int margin = theApp.DPI(10), button_height = theApp.DPI(30), top = margin * 2 + button_height, left = theApp.DPI(172);
    const int button_width = (std::max)(theApp.DPI(60), (width - margin * 6) / 5);
    int button_left = margin;
    for (int id : {IDC_SERVER_REFRESH, IDC_SERVER_DOWNLOAD, IDC_SERVER_PLAY, IDC_SERVER_CONFIGURE, IDC_SERVER_CANCEL})
    {
        GetDlgItem(id)->MoveWindow(button_left, margin, button_width, button_height);
        button_left += button_width + margin;
    }
    const int bottom = (std::max)(top + theApp.DPI(80), height - theApp.DPI(90));
    m_playlist_list.MoveWindow(margin, top, left - margin * 2, bottom - top);
    m_track_list.MoveWindow(left, top, (std::max)(100, width - left - margin), bottom - top);
    GetDlgItem(IDC_SERVER_NOTES)->MoveWindow(margin, bottom + margin, (std::max)(100, width - margin * 2), theApp.DPI(36));
    GetDlgItem(IDC_SERVER_STATUS)->MoveWindow(margin, bottom + theApp.DPI(50), (std::max)(100, width - margin * 2), theApp.DPI(35));
}
