#include "../MusicPlayer2/RemoteLibraryState.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <atomic>

int main()
{
    RemoteRefreshSchedule clock;
    assert(clock.Due(0, true, false));
    clock.Started();
    assert(!clock.Due(999999, true, true));
    clock.Finished(1000, true);
    assert(!clock.Due(3600999, true, false));
    assert(clock.Due(3601000, true, false));
    clock.Finished(3601000, false);
    assert(!clock.Due(10800999, true, false));
    assert(clock.Due(10801000, true, false));
    clock.Finished(10801000, false);
    assert(clock.Delay() == 14400000);
    clock.Finished(10801000, false);
    assert(clock.Delay() == 21600000);
    clock.Finished(10801000, true);
    assert(clock.Delay() == 3600000);
    assert(!clock.Due(9999999, false, false));

    RemoteTransferProgress p;
    p.phase = RemoteTransferPhase::Downloading; p.bytes = 25; p.total = 100;
    assert(p.Active() && p.Fraction() == .25);
    p.bulk = true; p.completed_bytes = 100; p.total_bytes = 400;
    assert(p.Fraction() == .3125);
    p.bytes = 999; assert(p.Fraction() == 1.0);
    p.phase = RemoteTransferPhase::Cancelled; assert(!p.Active());
    p.total_bytes = 0; assert(p.Fraction() == 0);
    RemoteTransferState state; state.Set(p); assert(!state.Read().Active());
    std::atomic<bool> finished{false};
    std::thread writer([&] {
        for (unsigned i=0;i<20000;++i)
        {
            RemoteTransferProgress value;
            value.bytes=i; value.total=i+1; value.path=std::to_wstring(i);
            state.Set(value);
        }
        finished=true;
    });
    while (!finished)
    {
        const auto value=state.Read();
        if (!value.path.empty()) assert(value.path==std::to_wstring(value.bytes) && value.total==value.bytes+1);
    }
    writer.join();
    struct Track { std::string id; bool unavailable{}; unsigned size{}; };
    unsigned checks=0;
    auto pending=PendingRemoteTracks(std::vector<Track>{{"a",false,100},{"a",false,100},{"cached",false,500},{"missing",true,999},{"b",false,300}},
        [&checks](const Track& t){ ++checks; return t.id=="cached"; });
    assert(checks==3 && pending.size()==2 && pending[0].id=="a" && pending[1].id=="b");
    assert(pending[0].size+pending[1].size==400);
    std::cout << "PASS refresh interval/backoff/coalescing and byte-weighted transfer state\n";
}
