#pragma once
#include "TabDlg.h"
#include "RemoteLibraryConfig.h"
#include "RemoteLibraryResource.h"
#include <thread>
#include <atomic>
#include <memory>

class CServerLibraryDlg : public CTabDlg
{
public:
    CServerLibraryDlg() : CTabDlg(IDD_SERVER_LIBRARY) {}
    ~CServerLibraryDlg();
    BOOL OnInitDialog() override;
    void OnTabEntered() override;
    void OnOK() override;
protected:
    DECLARE_MESSAGE_MAP()
    afx_msg void OnRefresh();
    afx_msg void OnDownload();
    afx_msg void OnConfigure();
    afx_msg void OnCancelTask();
    afx_msg void OnPlaylist();
    afx_msg void OnTrackChanged(NMHDR*, LRESULT*);
    afx_msg void OnDoubleClick(NMHDR*, LRESULT*);
    afx_msg void OnDestroy();
    afx_msg void OnSize(UINT, int, int);
    afx_msg LRESULT OnFinished(WPARAM, LPARAM);
private:
    void Download(bool play);
    void Busy(bool value);
    void RenderPlaylists();
    void Layout(int width, int height);
    CListBox m_playlist_list;
    CListCtrl m_track_list;
    std::vector<RemotePlaylist> m_playlists;
    std::vector<RemotePlaylist> m_result_playlists;
    std::vector<std::wstring> m_result_files;
    std::wstring m_result_status;
    std::string m_loaded_signature;
    std::thread m_worker;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    bool m_sync_task{};
    bool m_play_task{};
    bool m_success{};
    int m_selected_playlist{};
};
