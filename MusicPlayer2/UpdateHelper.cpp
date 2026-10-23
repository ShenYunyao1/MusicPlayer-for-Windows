#include "stdafx.h"
#include "UpdateHelper.h"
#include "MusicPlayer2.h"
#include <fstream>
#include <filesystem>

void CUpdateHelper::Launch(bool message)
{
#ifndef _M_X64
    if (message) AfxMessageBox(L"自动更新仅支持 Windows x64。请从独立仓库下载 x64 完整包。", MB_ICONINFORMATION);
#else
    if (theApp.m_pMainWnd == nullptr || !IsWindow(theApp.m_pMainWnd->GetSafeHwnd())) return;
    const std::wstring executable = theApp.m_module_dir + L"MusicPlayerUpdater.exe";
    if (GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        if (message) AfxMessageBox(L"缺少 MusicPlayerUpdater.exe，请下载并完整解压官方发行包。", MB_ICONWARNING);
        return;
    }
    std::wstring root = theApp.m_module_dir;
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
    std::wstring state = theApp.m_config_dir + L"updates";
    FILETIME created{}, exited{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    const auto creation = (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    std::wstring command = L"\"" + executable + L"\" --install \"" + root + L"\" --state \"" + state
        + L"\" --parent " + std::to_wstring(GetCurrentProcessId()) + L" --parent-created " + std::to_wstring(creation) + L" --window "
        + std::to_wstring(reinterpret_cast<UINT_PTR>(theApp.GetMainWnd()->GetSafeHwnd()));
    if (!message) command += L" --background";
    STARTUPINFOW startup{ sizeof(startup) };
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), &command[0], nullptr, nullptr, FALSE, 0, nullptr,
        root.c_str(), &startup, &process))
    {
        if (message) AfxMessageBox(L"无法启动更新器，请检查文件或使用完整发行包。", MB_ICONWARNING);
        return;
    }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    CloseHandle(process.hProcess);
#endif
}

void CUpdateHelper::NotifyReady()
{
    wchar_t token[65]{};
    if (GetEnvironmentVariableW(L"MUSICPLAYER_UPDATE_NONCE", token, 65) != 64) return;
    for (const auto c : std::wstring(token))
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return;
    const std::wstring path = theApp.m_module_dir + L".musicplayer-update\\ready.txt";
    std::ofstream output(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (!output) return;
    const std::wstring wideToken(token);
    output << std::string(wideToken.begin(), wideToken.end()) << "\n" << CLIENT_VERSION_A << "\n" << CLIENT_COMMIT << "\n";
    output.close();
    if (output.good()) SetEnvironmentVariableW(L"MUSICPLAYER_UPDATE_NONCE", nullptr);
}

bool CUpdateHelper::RecoverInterrupted()
{
    if (GetEnvironmentVariableW(L"MUSICPLAYER_UPDATE_NONCE", nullptr, 0) > 0) return false;
    std::ifstream input(std::filesystem::path(theApp.m_module_dir + L".musicplayer-update\\journal.json"));
    if (!input) return false;
    const std::string journal((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    bool pending = false;
    for (const auto* state : { "applying", "awaiting_start", "rolling_back" })
        if (journal.find(std::string("\"state\": \"") + state + "\"") != std::string::npos) pending = true;
    if (!pending) return false;
    const auto executable = theApp.m_module_dir + L"MusicPlayerUpdater.exe";
    std::wstring root = theApp.m_module_dir;
    while (!root.empty() && root.back() == L'\\') root.pop_back();
    std::wstring command = L"\"" + executable + L"\" --recover --install \"" + root
        + L"\" --parent " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW startup{ sizeof(startup) }; PROCESS_INFORMATION process{};
    if (CreateProcessW(executable.c_str(), &command[0], nullptr, nullptr, FALSE, 0, nullptr, root.c_str(), &startup, &process))
    { CloseHandle(process.hThread); CloseHandle(process.hProcess); }
    else AfxMessageBox(L"上次更新未完成，请运行独立更新器 --recover 或完整解压官方发行包恢复。", MB_ICONWARNING);
    return true;
}
