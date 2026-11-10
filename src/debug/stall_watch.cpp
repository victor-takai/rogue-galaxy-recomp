#include "debug/stall_watch.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <thread>
#include <variant>

#include "debug/host_stacks.h"
#include "flip_pacing.h"
#include "ps2_runtime.h"
#include "return_hook.h"
#include "runtime/ee_scheduler.h"
#include "vif1_thread.h"

namespace rg::debug
{
namespace
{

std::thread g_thread;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_reportRequested{false};

const char *statusName(EeThreadStatus status)
{
    switch (status)
    {
    case EeThreadStatus::Running:
        return "running";
    case EeThreadStatus::Ready:
        return "ready";
    case EeThreadStatus::Waiting:
        return "waiting";
    case EeThreadStatus::WaitingSuspended:
        return "waiting+suspended";
    case EeThreadStatus::Suspended:
        return "suspended";
    case EeThreadStatus::Dormant:
        return "dormant";
    }
    return "?";
}

const char *reasonName(EeWaitReason reason)
{
    switch (reason)
    {
    case EeWaitReason::None:
        return "";
    case EeWaitReason::Sleep:
        return " on SleepThread";
    case EeWaitReason::Semaphore:
        return " on semaphore";
    case EeWaitReason::EventFlag:
        return " on event flag";
    case EeWaitReason::VSync:
        return " on vsync";
    case EeWaitReason::External:
        return " on the host";
    case EeWaitReason::Mpeg:
        return " on MPEG";
    }
    return " on ?";
}

// Read from this thread while the game's thread may be running: the
// pointers are stable (map nodes), the fields plain, and a stall is when
// nothing changes anyway. Only for the log.
void dump(PS2Runtime &runtime, double stalledSeconds, uint64_t flips)
{
    EeScheduler &scheduler = runtime.eeScheduler();
    if (stalledSeconds > 0.0)
    {
        std::fprintf(stderr, "[stall] no flip for %.0f s (%llu flips so far, VIF1 %s), guest threads:\n",
                     stalledSeconds, static_cast<unsigned long long>(flips), rg::vif1::busy() ? "busy" : "idle");
    }
    else
    {
        std::fprintf(stderr, "[stall] report on request (%llu flips so far, VIF1 %s), guest threads:\n",
                     static_cast<unsigned long long>(flips), rg::vif1::busy() ? "busy" : "idle");
    }
    // The game's main thread never ends on purpose; one that did looks like
    // a freeze with every other thread waiting (2026-10-19, a battle).
    if (const GuestThread *main = scheduler.thread(EeScheduler::kMainThreadId); !main)
    {
        std::fprintf(stderr, "[stall]   thread %d (the main thread) no longer exists\n", EeScheduler::kMainThreadId);
    }
    else if (main->status == EeThreadStatus::Dormant)
    {
        std::fprintf(stderr, "[stall]   thread %d (the main thread) is dormant: it ended\n", EeScheduler::kMainThreadId);
    }
    for (int id = 0; id <= EeScheduler::kLastThreadId; ++id)
    {
        const GuestThread *thread = scheduler.thread(id);
        if (!thread || thread->status == EeThreadStatus::Dormant)
        {
            continue;
        }
        const R5900Context &ctx = thread->activeContext();
        char detail[160] = "";
        switch (thread->wait.reason)
        {
        case EeWaitReason::Semaphore:
            if (const auto *wait = std::get_if<EeSemaphoreWait>(&thread->wait.payload))
            {
                const EeSemaphore *sema = scheduler.semaphore(wait->id);
                if (sema)
                {
                    std::snprintf(detail, sizeof(detail), " %d (count %d of %d, %zu waiting)", wait->id, sema->count,
                                  sema->maxCount, sema->waiters.size());
                }
                else
                {
                    std::snprintf(detail, sizeof(detail), " %d (no such semaphore)", wait->id);
                }
            }
            break;
        case EeWaitReason::EventFlag:
            if (const auto *wait = std::get_if<EeEventFlagWait>(&thread->wait.payload))
            {
                std::snprintf(detail, sizeof(detail), " %d bits 0x%08X mode %u", wait->id, wait->bits, wait->mode);
            }
            break;
        case EeWaitReason::VSync:
            if (const auto *wait = std::get_if<EeVSyncWait>(&thread->wait.payload))
            {
                std::snprintf(detail, sizeof(detail), " after tick %llu",
                              static_cast<unsigned long long>(wait->afterTick));
            }
            break;
        case EeWaitReason::External:
            if (const auto *wait = std::get_if<EeExternalWait>(&thread->wait.payload))
            {
                std::snprintf(detail, sizeof(detail), " type %u token %llu", wait->type,
                              static_cast<unsigned long long>(wait->token));
            }
            break;
        default:
            break;
        }
        std::fprintf(stderr, "[stall]   thread %d %s%s%s, priority %d, pc 0x%08X ra 0x%08X sp 0x%08X%s%s\n", id,
                     statusName(thread->status), reasonName(thread->wait.reason), detail, thread->currentPriority,
                     ctx.pc, getRegU32(&ctx, 31), getRegU32(&ctx, 29), thread->inGuestCallMask != 0u ? ", in a guest call" : "",
                     thread->invocations.empty() ? "" : ", inside a callback");
        if (thread->wait.reason == EeWaitReason::Semaphore)
        {
            // WaitVsyncCount(count = s1, n = s0) waits on the vsync semaphore
            // (its id at gp-0x7258) until the counter at gp-0x40AC passes
            // count + n; the vblank handler signals only while the flag at
            // gp-0x40B0 is set. The live words tell a thread re-waiting every
            // vblank from one never woken (Rogue Galaxy, SCUS_974.90).
            const uint8_t *rdram = runtime.memory().getRDRAM();
            auto word = [rdram](uint32_t address) {
                uint32_t value = 0u;
                if (rdram && address + 4u <= PS2_RAM_SIZE)
                {
                    std::memcpy(&value, rdram + address, sizeof(value));
                }
                return value;
            };
            const uint32_t vsyncSema = word(0x485170u - 0x7258u);
            if (const auto *wait = std::get_if<EeSemaphoreWait>(&thread->wait.payload);
                wait && vsyncSema != 0u && wait->id == static_cast<int>(vsyncSema))
            {
                std::fprintf(stderr,
                             "[stall]     the vsync semaphore: counter %u, flag %u; this thread's count %u + n %u (a0 %u)\n",
                             word(0x485170u - 0x40ACu), word(0x485170u - 0x40B0u), getRegU32(&ctx, 17),
                             getRegU32(&ctx, 16), getRegU32(&ctx, 4));
            }
            else
            {
                std::fprintf(stderr, "[stall]     v0 %08X a0 %08X a1 %08X s0 %08X s1 %08X\n", getRegU32(&ctx, 2),
                             getRegU32(&ctx, 4), getRegU32(&ctx, 5), getRegU32(&ctx, 16), getRegU32(&ctx, 17));
            }
        }
        if (thread->status == EeThreadStatus::Running || thread->status == EeThreadStatus::Ready)
        {
            // What a loop is working on: its arguments and saved registers.
            std::fprintf(stderr,
                         "[stall]     v0 %08X a0 %08X a1 %08X a2 %08X a3 %08X s0 %08X s1 %08X s2 %08X s3 %08X s4 "
                         "%08X s5 %08X s6 %08X s7 %08X gp %08X\n",
                         getRegU32(&ctx, 2), getRegU32(&ctx, 4), getRegU32(&ctx, 5), getRegU32(&ctx, 6),
                         getRegU32(&ctx, 7), getRegU32(&ctx, 16), getRegU32(&ctx, 17), getRegU32(&ctx, 18),
                         getRegU32(&ctx, 19), getRegU32(&ctx, 20), getRegU32(&ctx, 21), getRegU32(&ctx, 22),
                         getRegU32(&ctx, 23), getRegU32(&ctx, 28));
        }
    }
    rg::hooks::describePending(stderr);
    // The host side: a guest thread "running" with a pc that never moves is
    // parked in host code, and only the host stacks say where.
    printHostStacks(stderr, "[stall]");
    // Where the running thread passes its safe points over the next second:
    // the loop it is in, if it is in one.
    const int running = scheduler.currentThreadId();
    if (const GuestThread *thread = scheduler.thread(running))
    {
        struct Seen
        {
            uint32_t pc;
            uint32_t count;
        };
        Seen seen[16] = {};
        unsigned distinct = 0u;
        for (int sample = 0; sample < 40; ++sample)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            const uint32_t pc = thread->activeContext().pc;
            unsigned i = 0u;
            for (; i < distinct; ++i)
            {
                if (seen[i].pc == pc)
                {
                    ++seen[i].count;
                    break;
                }
            }
            if (i == distinct && distinct < 16u)
            {
                seen[distinct++] = Seen{pc, 1u};
            }
        }
        std::fprintf(stderr, "[stall]   thread %d's pc over 40 samples:", running);
        for (unsigned i = 0; i < distinct; ++i)
        {
            std::fprintf(stderr, " 0x%08X x%u", seen[i].pc, seen[i].count);
        }
        std::fprintf(stderr, "\n");
    }
    std::fflush(stderr);
}

void run(PS2Runtime *runtime, double threshold)
{
    using Clock = std::chrono::steady_clock;
    uint64_t lastFlips = 0u;
    Clock::time_point lastChange = Clock::now();
    Clock::time_point lastDump{};
    // The main thread's last known place, for a freeze the flips do not
    // show: a loading screen keeps flipping while the main thread waits on
    // a semaphore nobody signals (2026-10-19, a return to the title).
    uint32_t mainPc = 0u, mainSp = 0u;
    uint8_t mainStatus = 0u;
    Clock::time_point mainChange = Clock::now();
    while (!g_stop.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const uint64_t flips = rg::graphics::guestFlips();
        const Clock::time_point now = Clock::now();
        if (g_reportRequested.exchange(false, std::memory_order_acq_rel))
        {
            dump(*runtime, 0.0, flips);
            lastDump = now;
            continue;
        }
        if (const GuestThread *main = runtime->eeScheduler().thread(EeScheduler::kMainThreadId);
            main && flips != 0u && !runtime->eeScheduler().hostPaused())
        {
            const R5900Context &ctx = main->activeContext();
            const uint32_t pc = ctx.pc, sp = getRegU32(&ctx, 29);
            const uint8_t status = static_cast<uint8_t>(main->status);
            // The main loop sits in WaitVsyncCount(count, n) every frame, so
            // a sample inside that wait says nothing by itself; it is stuck
            // only once the vsync counter has passed count + n and it still
            // waits (the counter at gp-0x40AC, count in s1, n in s0; the
            // vsync semaphore's id at gp-0x7258). Before this rule the
            // report fired on every intro movie.
            bool waitingItsTurn = false;
            if (main->wait.reason == EeWaitReason::Semaphore)
            {
                const uint8_t *rdram = runtime->memory().getRDRAM();
                auto word = [rdram](uint32_t address) {
                    uint32_t value = 0u;
                    if (rdram && address + 4u <= PS2_RAM_SIZE)
                    {
                        std::memcpy(&value, rdram + address, sizeof(value));
                    }
                    return value;
                };
                const uint32_t vsyncSema = word(0x485170u - 0x7258u);
                const auto *wait = std::get_if<EeSemaphoreWait>(&main->wait.payload);
                if (wait && vsyncSema != 0u && wait->id == static_cast<int>(vsyncSema))
                {
                    const uint32_t counter = word(0x485170u - 0x40ACu);
                    waitingItsTurn = counter - getRegU32(&ctx, 17) < getRegU32(&ctx, 16);
                }
            }
            if (pc != mainPc || sp != mainSp || status != mainStatus || main->status == EeThreadStatus::Running ||
                waitingItsTurn)
            {
                mainPc = pc;
                mainSp = sp;
                mainStatus = status;
                mainChange = now;
            }
            else if (std::chrono::duration<double>(now - mainChange).count() >= threshold &&
                     (lastDump == Clock::time_point{} || std::chrono::duration<double>(now - lastDump).count() >= 60.0))
            {
                std::fprintf(stderr, "[stall] the main thread has not moved for %.0f s (pc 0x%08X sp 0x%08X) while the game flips\n",
                             std::chrono::duration<double>(now - mainChange).count(), pc, sp);
                dump(*runtime, 0.0, flips);
                lastDump = now;
                continue;
            }
        }
        // The host's pause, and the boot before the first flip, are not
        // stalls.
        if (flips != lastFlips || flips == 0u || runtime->eeScheduler().hostPaused())
        {
            lastFlips = flips;
            lastChange = now;
            continue;
        }
        const double stalled = std::chrono::duration<double>(now - lastChange).count();
        if (stalled < threshold)
        {
            continue;
        }
        if (lastDump == Clock::time_point{} || std::chrono::duration<double>(now - lastDump).count() >= 60.0)
        {
            dump(*runtime, stalled, flips);
            lastDump = now;
        }
    }
}

} // namespace

void startStallWatch(PS2Runtime &runtime)
{
    const char *enabled = std::getenv("RG_STALL_WATCH");
    if (enabled && *enabled == '0')
    {
        return;
    }
    const char *seconds = std::getenv("RG_STALL_SECONDS");
    const double threshold = seconds && std::atof(seconds) > 0.0 ? std::atof(seconds) : 15.0;
    g_stop.store(false, std::memory_order_release);
    g_thread = std::thread(run, &runtime, threshold);
    // The same report as F10 from outside (`pkill -USR1 roguegalaxy`): for a
    // hidden window, or a hang that keeps flipping.
    std::signal(SIGUSR1, [](int) { g_reportRequested.store(true, std::memory_order_release); });
}

void requestStallReport()
{
    g_reportRequested.store(true, std::memory_order_release);
}

void stopStallWatch()
{
    g_stop.store(true, std::memory_order_release);
    if (g_thread.joinable())
    {
        g_thread.join();
    }
}

} // namespace rg::debug
