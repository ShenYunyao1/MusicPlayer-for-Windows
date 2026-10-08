#include "stdafx.h"
#include "ServerLibraryDlg.h"
#include "MusicPlayer2.h"
#include "Player.h"
#include "MusicPlayerDlg.h"
#include "MusicPlayerCmdHelper.h"
#include "CRecentList.h"
#include "Common.h"
#include "RemoteLibraryVersion.h"

BEGIN_MESSAGE_MAP(CServerLibraryDlg, CTabDlg)
    ON_BN_CLICKED(IDC_SERVER_REFRESH, &CServerLibraryDlg::OnRefresh)
    ON_BN_CLICKED(IDC_SERVER_DOWNLOAD, &CServerLibraryDlg::OnDownload)
    ON_BN_CLICKED(IDC_SERVER_PLAY, &CServerLibraryDlg::OnOK)
    ON_BN_CLICKED(IDC_SERVER_CONFIGURE, &CServerLibraryDlg::OnConfigure)
    ON_BN_CLICKED(IDC_SERVER_CANCEL, &CServerLibraryDlg::OnCancelTask)
    ON_LBN_SELCHANGE(IDC_SERVER_PLAYLISTS, &CServerLibraryDlg::OnPlaylist)
    ON_LBN_DBLCLK(IDC_SERVER_PLAYLISTS, &CServerLibraryDlg::OnOK)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_SERVER_TRACKS, &CServerLibraryDlg::OnTrackChanged)
    ON_WM_TIMER()
    ON_WM_DESTROY()
    ON_WM_SIZE()
END_MESSAGE_MAP()

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
    SetDlgItemTextW(IDC_SERVER_PLAY, L"播放歌单");
    SetDlgItemTextW(IDC_SERVER_CONFIGURE, L"服务器设置");
    SetDlgItemTextW(IDC_SERVER_CANCEL, L"取消任务");
    SetDlgItemTextW(IDC_SERVER_STATUS, (L"服务器适配 "+std::wstring(RemoteLibraryVersion::Version)+L"；播放时按需缓存当前曲目。").c_str());
    m_state=CRemoteLibraryManager::Instance().BrowseState();
    UpdateButtons();
    SetTimer(1,100,nullptr);
    CRect rect; GetClientRect(rect); Layout(rect.Width(), rect.Height());
    return TRUE;
}

bool CServerLibraryDlg::HasPlaylist() const
{
    return m_selected_playlist>=0 && m_selected_playlist<static_cast<int>(m_playlists.size()) && !m_playlists[m_selected_playlist].tracks.empty();
}
void CServerLibraryDlg::UpdateButtons()
{
    const bool busy=bool(m_library_task) || bool(m_offline_task);
    GetDlgItem(IDC_SERVER_REFRESH)->EnableWindow(!busy);
    GetDlgItem(IDC_SERVER_CONFIGURE)->EnableWindow(!busy);
    GetDlgItem(IDC_SERVER_DOWNLOAD)->EnableWindow(!busy && HasPlaylist());
    GetDlgItem(IDC_SERVER_PLAY)->EnableWindow(HasPlaylist());
    GetDlgItem(IDC_SERVER_CANCEL)->EnableWindow(busy);
    if (auto parent=GetParentWindow()) parent->SendMessage(WM_PLAY_SELECTED_BTN_ENABLE,HasPlaylist());
}
void CServerLibraryDlg::OnTabEntered()
{
    auto space=RemoteLibraryConfig::Load().Client().CacheDirectory();
    if (space!=m_loaded_namespace)
    {
        m_loaded_namespace=space;
        m_playlists.clear(); m_selected_playlist=-1;
        m_state=CRemoteLibraryManager::Instance().BrowseState();
        m_library_task=CRemoteLibraryManager::Instance().Library();
    }
    UpdateButtons();
}
void CServerLibraryDlg::OnRefresh()
{
    if (m_library_task) return;
    SaveState();
    m_library_task=CRemoteLibraryManager::Instance().Library(true);
    SetDlgItemTextW(IDC_SERVER_STATUS,L"正在检查歌单更新…正在播放的队列保持当前快照。");
    UpdateButtons();
}
void CServerLibraryDlg::SaveState()
{
    if (m_rendering || m_selected_playlist<0 || m_selected_playlist>=static_cast<int>(m_playlists.size())) return;
    const auto& playlist=m_playlists[m_selected_playlist];
    m_state.playlist_id=playlist.id;
    int row=m_track_list.GetNextItem(-1,LVNI_SELECTED);
    m_state.track_id=(row>=0 && row<static_cast<int>(playlist.tracks.size())) ? playlist.tracks[row].id : "";
    m_state.top_row=m_track_list.GetTopIndex();
    CRemoteLibraryManager::Instance().SaveBrowseState(m_state);
}
void CServerLibraryDlg::RenderPlaylists()
{
    m_rendering=true;
    m_playlist_list.ResetContent();
    m_selected_playlist=-1;
    for (size_t i=0;i<m_playlists.size();++i)
    {
        m_playlist_list.AddString(m_playlists[i].name.c_str());
        if (m_playlists[i].id==m_state.playlist_id) m_selected_playlist=static_cast<int>(i);
    }
    if (m_selected_playlist<0 && !m_playlists.empty()) m_selected_playlist=0;
    m_playlist_list.SetCurSel(m_selected_playlist);
    RenderTracks();
    m_rendering=false;
    UpdateButtons();
}
void CServerLibraryDlg::RenderTracks()
{
    m_track_list.DeleteAllItems();
    SetDlgItemTextW(IDC_SERVER_NOTES,L"");
    if (m_selected_playlist<0 || m_selected_playlist>=static_cast<int>(m_playlists.size())) return;
    const auto& playlist=m_playlists[m_selected_playlist];
    SetDlgItemTextW(IDC_SERVER_NOTES,playlist.description.c_str());
    int row=0;
    for (const auto& track:playlist.tracks)
    {
        m_track_list.InsertItem(row,track.title.c_str());
        m_track_list.SetItemText(row,1,track.artist.c_str());
        m_track_list.SetItemText(row,2,track.album.c_str());
        m_track_list.SetItemText(row,3,track.genre.c_str());
        if (track.id==m_state.track_id) m_track_list.SetItemState(row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
        ++row;
    }
    if (row>0 && m_state.top_row>0)
    {
        int top=(std::min)(row-1,m_state.top_row);
        m_track_list.EnsureVisible(top,FALSE);
        CRect item; m_track_list.GetItemRect(0,item,LVIR_BOUNDS);
        m_track_list.Scroll(CSize(0,(top-m_track_list.GetTopIndex())*item.Height()));
    }
}
void CServerLibraryDlg::OnPlaylist()
{
    if (m_rendering) return;
    SaveState();
    m_selected_playlist=m_playlist_list.GetCurSel();
    m_state.track_id.clear(); m_state.top_row=0;
    m_rendering=true; RenderTracks(); m_rendering=false;
    SaveState(); UpdateButtons();
}
void CServerLibraryDlg::OnTrackChanged(NMHDR*, LRESULT* result)
{
    *result=0;
    if (m_rendering || m_selected_playlist<0 || m_selected_playlist>=static_cast<int>(m_playlists.size())) return;
    const auto& playlist=m_playlists[m_selected_playlist];
    int row=m_track_list.GetNextItem(-1,LVNI_SELECTED);
    auto note=playlist.description;
    if (row>=0 && row<static_cast<int>(playlist.tracks.size()))
    {
        const auto& track=playlist.tracks[row]; note+=L"  "+track.comment;
        auto found=playlist.track_notes.find(track.id);
        if (found!=playlist.track_notes.end()) note+=L"  "+found->second;
    }
    SetDlgItemTextW(IDC_SERVER_NOTES,note.c_str());
    SaveState(); UpdateButtons();
}
void CServerLibraryDlg::OnOK()
{
    if (!HasPlaylist()) return;
    if (CPlayer::GetInstance().IsLoading()) { SetDlgItemTextW(IDC_SERVER_STATUS,L"播放器正在加载列表，请稍后再试。"); return; }
    SaveState();
    std::wstring error;
    if (!CPlayer::GetInstance().PlayRemotePlaylist(m_playlists[m_selected_playlist],error))
        SetDlgItemTextW(IDC_SERVER_STATUS,error.c_str());
    else SetDlgItemTextW(IDC_SERVER_STATUS,L"已载入完整歌单；播放到未缓存曲目时才下载，离线时跳过。");
}
void CServerLibraryDlg::OnDownload()
{
    if (!HasPlaylist() || m_offline_task) return;
    CRemoteLibraryManager::Instance().RetryConnection();
    m_offline_task=CRemoteLibraryManager::Instance().SaveOffline(m_playlists[m_selected_playlist]);
    SetDlgItemTextW(IDC_SERVER_STATUS,L"正在离线保存整个歌单；关闭媒体库后任务仍继续。");
    UpdateButtons();
}
void CServerLibraryDlg::OnCancelTask()
{
    if (m_library_task) *m_library_task->cancel=true;
    if (m_offline_task) *m_offline_task->cancel=true;
    SetDlgItemTextW(IDC_SERVER_STATUS,L"正在取消页面任务，播放任务继续运行。");
}
void CServerLibraryDlg::OnConfigure()
{
    SaveState();
    theApp.m_pMainWnd->SendMessage(WM_OPTION_SETTINGS,6,LPARAM(GetParentWindow()));
    if (IsWindow(GetSafeHwnd())) { OnTabEntered(); if (!m_library_task) OnRefresh(); }
}
void CServerLibraryDlg::OnTimer(UINT_PTR timer)
{
    if (m_library_task && m_library_task->done.load(std::memory_order_acquire))
    {
        auto task=m_library_task; m_library_task.reset();
        std::wstring refresh_error;
        bool updated_current = false, updated_lists = false;
        if (task->success && !task->offline && !task->local_only)
        {
            auto& library = CRemoteLibraryManager::Instance();
            auto& player = CPlayer::GetInstance();
            for (const auto& playlist : task->playlists)
            {
                const auto paths = library.MaterializedPaths(playlist);
                if (paths.empty()) continue;
                const auto current_path = player.GetPlaylistPath();
                if (player.IsPlaylistMode() && std::find(paths.begin(), paths.end(), current_path) != paths.end())
                {
                    if (!player.RefreshCurrentRemotePlaylist(playlist, refresh_error)) break;
                    updated_current = true;
                }
                else
                {
                    std::wstring path;
                    if (!library.Materialize(playlist, paths.front(), path, refresh_error)) break;
                    CRecentList::Instance().AddNewItem({ LT_PLAYLIST, path });
                    int duration = 0;
                    for (const auto& track : playlist.tracks) duration += track.duration_ms;
                    CRecentList::Instance().SetPlaylistTotalInfo({ LT_PLAYLIST, path },
                        static_cast<int>(playlist.tracks.size()), duration);
                }
                updated_lists = true;
            }
            if (updated_lists)
            {
                CRecentList::Instance().SaveData();
                CMusicPlayerCmdHelper::RefreshMediaTabData(CMusicPlayerCmdHelper::ML_PLAYLIST);
                if (updated_current) CMusicPlayerDlg::GetInstance()->ShowPlayList(false);
            }
        }
        if (task->success) { SaveState(); m_playlists=task->playlists; RenderPlaylists(); }
        SetDlgItemTextW(IDC_SERVER_STATUS,(refresh_error.empty() ? task->status : refresh_error).c_str());
        // First run has no offline snapshot: obtain the library from the server.
        if (!task->success && task->local_only && !*task->cancel) OnRefresh();
        UpdateButtons();
    }
    if (m_offline_task && m_offline_task->done.load(std::memory_order_acquire))
    {
        SetDlgItemTextW(IDC_SERVER_STATUS,m_offline_task->status.c_str());
        m_offline_task.reset(); UpdateButtons();
    }
    CTabDlg::OnTimer(timer);
}
void CServerLibraryDlg::OnDestroy()
{
    SaveState(); KillTimer(1);
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
