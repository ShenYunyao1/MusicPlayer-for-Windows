#pragma once
#include "TabDlg.h"
#include "RemoteLibraryConfig.h"
#include "RemoteLibraryResource.h"
#include <thread>

class CServerSettingsDlg : public CTabDlg
{
public:
    CServerSettingsDlg(CWnd* parent = nullptr) : CTabDlg(IDD_SERVER_SETTINGS, parent) {}
    ~CServerSettingsDlg();
    BOOL OnInitDialog() override;
    void GetDataFromUi() override;
protected:
    DECLARE_MESSAGE_MAP()
    afx_msg void OnTest();
    afx_msg void OnDestroy();
    afx_msg LRESULT OnFinished(WPARAM, LPARAM);
private:
    RemoteLibraryConfig ReadConfig();
    std::thread m_worker;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    std::wstring m_result;
};
