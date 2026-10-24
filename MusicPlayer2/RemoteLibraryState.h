#pragma once
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <set>

template<class Track, class Cached>
std::vector<Track> PendingRemoteTracks(const std::vector<Track>& tracks, Cached cached)
{
    std::set<std::string> seen;
    std::vector<Track> pending;
    for (const auto& t:tracks)
        if (!t.unavailable && seen.insert(t.id).second && !cached(t)) pending.push_back(t);
    return pending;
}

enum class RemoteTransferPhase { Idle, Queued, Checking, Downloading, Verifying, Ready, Failed, Cancelled };
struct RemoteTransferProgress
{
    RemoteTransferPhase phase{RemoteTransferPhase::Idle};
    std::uint64_t bytes{}, total{}, completed_bytes{}, total_bytes{};
    std::size_t completed_files{}, total_files{};
    bool bulk{};
    double bytes_per_second{};
    std::wstring path, name, namespace_id;
    bool Active() const { return phase==RemoteTransferPhase::Queued || phase==RemoteTransferPhase::Checking || phase==RemoteTransferPhase::Downloading || phase==RemoteTransferPhase::Verifying; }
    double Fraction() const
    {
        const auto denominator=bulk ? total_bytes : total;
        if (!denominator) return 0;
        return (std::min)(1.0,static_cast<double>((bulk ? completed_bytes : 0)+bytes)/denominator);
    }
};
class RemoteTransferState
{
public:
    void Set(const RemoteTransferProgress& p) { std::lock_guard<std::mutex> lock(m_mutex); m_value=p; }
    RemoteTransferProgress Read() const { std::lock_guard<std::mutex> lock(m_mutex); return m_value; }
private:
    mutable std::mutex m_mutex;
    RemoteTransferProgress m_value;
};
class RemoteRefreshSchedule
{
public:
    bool Due(std::uint64_t now, bool interested, bool busy) const { return interested && !busy && now>=m_due; }
    void Started() { m_due=UINT64_MAX; }
    void Finished(std::uint64_t now,bool success) { m_failures=success?0:(std::min)(m_failures+1,3u); m_due=now+Delay(); }
    std::uint64_t Delay() const { return m_failures==0?3600000:m_failures==1?7200000:m_failures==2?14400000:21600000; }
    void Reset() { m_due=0;m_failures=0; }
private:
    std::uint64_t m_due{};
    unsigned m_failures{};
};
