#pragma once
#include "TabDlg.h"
#include "RemoteLibraryManager.h"
#include "RemoteLibraryResource.h"
#include <thread>
#include <atomic>
#include <memory>

class CServerLibraryDlg : public CTabDlg
{
public:
    CServerLibraryDlg() : CTabDlg(IDD_SERVER_LIBRARY) {}
    BOOL OnInitDialog() override;
    void OnTabEntered() override;
    void OnTabExited() override { SaveState(); }
    void OnOK() override;
protected:
    DECLARE_MESSAGE_MAP()
    afx_msg void OnRefresh();
    afx_msg void OnDownload();
    afx_msg void OnConfigure();
    afx_msg void OnCancelTask();
    afx_msg void OnPlaylist();
    afx_msg void OnTrackChanged(NMHDR*, LRESULT*);
    afx_msg void OnDestroy();
    afx_msg void OnSize(UINT, int, int);
    afx_msg void OnTimer(UINT_PTR);
private:
    void UpdateButtons();
    void RenderPlaylists();
    void RenderTracks();
    void SaveState();
    bool HasPlaylist() const;
    void Layout(int width, int height);
    CListBox m_playlist_list;
    CListCtrl m_track_list;
    std::vector<RemotePlaylist> m_playlists;
    std::shared_ptr<RemoteLibraryTask> m_library_task, m_offline_task;
    std::wstring m_loaded_namespace;
    RemoteBrowseState m_state;
    bool m_rendering{};
    int m_selected_playlist{-1};
};
