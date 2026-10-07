#include "stdafx.h"
#include "RemoteLibraryConfig.h"
#include "MusicPlayer2.h"
#include "Common.h"
#include "nlohmann/json.hpp"
#include <filesystem>
#include <fstream>
#include <wincrypt.h>

#pragma comment(lib, "crypt32.lib")

namespace
{
    std::filesystem::path ConfigPath()
    {
        return std::filesystem::path(theApp.m_config_dir) / L"remote-library.json";
    }

    std::string Protect(const std::string& value, bool encrypt)
    {
        DATA_BLOB input{static_cast<DWORD>(value.size()), reinterpret_cast<BYTE*>(const_cast<char*>(value.data()))};
        DATA_BLOB output{};
        BOOL success = encrypt ? CryptProtectData(&input, L"MusicPlayer2 server key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)
            : CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output);
        if (!success) throw std::runtime_error("Windows credential protection failed");
        std::string result(reinterpret_cast<char*>(output.pbData), output.cbData);
        SecureZeroMemory(output.pbData, output.cbData);
        LocalFree(output.pbData);
        return result;
    }
}

RemoteLibraryConfig RemoteLibraryConfig::Load()
{
    RemoteLibraryConfig config;
    config.cache_directory = (std::filesystem::path(theApp.m_config_dir) / L"remote-cache").wstring();
    try
    {
        std::ifstream input(ConfigPath(), std::ios::binary);
        if (!input) return config;
        const auto value = nlohmann::json::parse(input);
        config.url = CCommon::StrToUnicode(value.value("url", ""), CodeType::UTF8);
        config.cache_directory = CCommon::StrToUnicode(value.value("cache_directory", CCommon::UnicodeToStr(config.cache_directory, CodeType::UTF8_NO_BOM)), CodeType::UTF8);
        if (value.contains("protected_key"))
        {
            const auto bytes = value["protected_key"].get<std::vector<unsigned char>>();
            config.api_key = Protect(std::string(bytes.begin(), bytes.end()), false);
        }
    }
    catch (const std::exception&) { config.api_key.clear(); }
    return config;
}

bool RemoteLibraryConfig::Save(std::wstring& error) const
{
    try
    {
        const std::string encrypted = Protect(api_key, true);
        const nlohmann::json value = {{"url", CCommon::UnicodeToStr(url, CodeType::UTF8_NO_BOM)},
            {"cache_directory", CCommon::UnicodeToStr(cache_directory, CodeType::UTF8_NO_BOM)},
            {"protected_key", std::vector<unsigned char>(encrypted.begin(), encrypted.end())}};
        std::filesystem::create_directories(ConfigPath().parent_path());
        const std::wstring temporary = ConfigPath().wstring() + L".tmp";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << value.dump(2);
        output.close();
        if (!output || !MoveFileExW(temporary.c_str(), ConfigPath().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("configuration write failed");
        return true;
    }
    catch (const std::exception& exception)
    {
        error = CCommon::StrToUnicode(exception.what(), CodeType::UTF8);
        return false;
    }
}

CRemoteLibraryClient RemoteLibraryConfig::Client() const
{
    CRemoteLibraryClient client;
    client.Configure(url, api_key, cache_directory);
    return client;
}
