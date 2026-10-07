#pragma once
#include "RemoteLibraryClient.h"

struct RemoteLibraryConfig
{
    std::wstring url;
    std::string api_key;
    std::wstring cache_directory;
    static RemoteLibraryConfig Load();
    bool Save(std::wstring& error) const;
    CRemoteLibraryClient Client() const;
};
