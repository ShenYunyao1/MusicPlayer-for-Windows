#include "stdafx.h"
#include "ServerSettingsDlg.h"
#include "Common.h"
#include "Player.h"
#include "RemoteLibraryManager.h"

namespace { constexpr UINT WM_SERVER_TEST_DONE = WM_APP + 701; }

BEGIN_MESSAGE_MAP(CServerSettingsDlg, CTabDlg)
    ON_BN_CLICKED(IDC_SERVER_TEST, &CServerSettingsDlg::OnTest)
    ON_MESSAGE(WM_SERVER_TEST_DONE, &CServerSettingsDlg::OnFinished)
    ON_WM_DESTROY()
END_MESSAGE_MAP()

CServerSettingsDlg::~CServerSettingsDlg()
{
    if (m_cancel) *m_cancel = true;
    if (m_worker.joinable()) m_worker.join();
}

BOOL CServerSettingsDlg::OnInitDialog()
{
    CTabDlg::OnInitDialog();
    const auto config = RemoteLibraryConfig::Load();
    SetDlgItemTextW(IDC_SERVER_URL, config.url.c_str());
    SetDlgItemTextW(IDC_SERVER_KEY, CCommon::StrToUnicode(config.api_key, CodeType::UTF8).c_str());
    SetDlgItemTextW(IDC_SERVER_CACHE, config.cache_directory.c_str());
    SetDlgItemTextW(IDC_SERVER_TEST, L"测试连接");
    SetDlgItemTextW(IDC_SERVER_URL_LABEL, L"服务器地址（HTTP / HTTPS）");
    SetDlgItemTextW(IDC_SERVER_KEY_LABEL, L"账号的 API Key");
    SetDlgItemTextW(IDC_SERVER_CACHE_LABEL, L"缓存目录");
    SetDlgItemTextW(IDC_SERVER_STATUS, L"先在网页登录并完成首次改密，接受分享或订阅后，创建含 library:read、media:read 的 Key，显式选择本人歌单、订阅或全局曲库。\n填写根地址（如 http://127.0.0.1:8080）并应用；密钥由 Windows 加密保存。首次播放按需完整下载。");
    return TRUE;
}

RemoteLibraryConfig CServerSettingsDlg::ReadConfig()
{
    CString url, key, cache;
    GetDlgItemTextW(IDC_SERVER_URL, url);
    GetDlgItemTextW(IDC_SERVER_KEY, key);
    GetDlgItemTextW(IDC_SERVER_CACHE, cache);
    url.Trim(); key.Trim(); cache.Trim();
    RemoteLibraryConfig config;
    config.url = url.GetString();
    config.api_key = CCommon::UnicodeToStr(key.GetString(), CodeType::UTF8_NO_BOM);
    config.cache_directory = cache.GetString();
    return config;
}

void CServerSettingsDlg::GetDataFromUi()
{
    const auto config = ReadConfig();
    std::wstring error;
    if (!config.url.empty() && (!config.Client().IsConfigured() || config.cache_directory.empty()))
    {
        AfxMessageBox(L"请输入 HTTP/HTTPS 服务器地址和缓存目录。");
        return;
    }
    if (!config.Save(error)) AfxMessageBox((L"保存服务器配置失败：" + error).c_str());
    else
    {
        CPlayer::GetInstance().ValidateRemotePlaylistAccess();
        CRemoteLibraryManager::Instance().RemoveUnavailableRecentPlaylists();
    }
}

void CServerSettingsDlg::OnTest()
{
    if (m_worker.joinable()) return;
    auto client = ReadConfig().Client();
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    client.SetCancellation(m_cancel);
    const HWND window = GetSafeHwnd();
    GetDlgItem(IDC_SERVER_TEST)->EnableWindow(FALSE);
    SetDlgItemTextW(IDC_SERVER_STATUS, L"正在验证服务器连接和 API Key…");
    m_worker = std::thread([this, client, window] {
        try
        {
            std::wstring error;
            m_result = client.CheckConnection(error) ? L"v2 连接成功，读取权限通过；无歌单时请检查 Key 的歌单授权。" : L"连接失败：" + error;
        }
        catch (const std::exception&) { m_result = L"连接失败：发生异常"; }
        ::PostMessageW(window, WM_SERVER_TEST_DONE, 0, 0);
    });
}

LRESULT CServerSettingsDlg::OnFinished(WPARAM, LPARAM)
{
    if (m_worker.joinable()) m_worker.join();
    SetDlgItemTextW(IDC_SERVER_STATUS, m_result.c_str());
    GetDlgItem(IDC_SERVER_TEST)->EnableWindow(TRUE);
    return 0;
}

void CServerSettingsDlg::OnDestroy()
{
    if (m_cancel) *m_cancel = true;
    if (m_worker.joinable()) m_worker.join();
    CTabDlg::OnDestroy();
}
