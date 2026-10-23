#pragma once
class CUpdateHelper
{
public:
    enum class UpdateSource { GitHubSource, GiteeSource };
    static void Launch(bool message);
    static void NotifyReady();
    static bool RecoverInterrupted();
};
