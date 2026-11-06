#include "return_hook.h"

#include <cstdio>
#include <vector>

#include "runtime/ee_scheduler.h"

namespace rg::hooks
{
namespace
{

// A call that a safe point unwound, waiting for its caller to resume. Only
// the game's thread, which runs all guest code, touches these.
struct Pending
{
    uint32_t returnPc;
    uint32_t sp;  // the caller's, and its guest thread: together they tell
    int thread;   // apart threads stopped at the same place
    After after;
    uint32_t data;
    PS2Runtime::RecompiledFunction resume; // the table entry this replaced
};
std::vector<Pending> g_pending;

// In the table at a pending call's return address until the caller resumes
// there.
void resumeAtReturn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t pc = ctx->pc;
    const uint32_t sp = getRegU32(ctx, 29);
    const EeScheduler &scheduler = runtime->eeScheduler();
    const int thread = scheduler.currentThreadId();
    PS2Runtime::RecompiledFunction resume = nullptr;
    bool due = false;
    Pending call{};
    bool othersWaiting = false;
    for (size_t i = 0; i < g_pending.size();)
    {
        const Pending &entry = g_pending[i];
        if (entry.returnPc != pc)
        {
            ++i;
            continue;
        }
        resume = entry.resume;
        if (!due && entry.sp == sp && entry.thread == thread)
        {
            call = entry;
            due = true;
            g_pending.erase(g_pending.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        const GuestThread *owner = scheduler.thread(entry.thread);
        if (!owner || owner->status == EeThreadStatus::Dormant)
        {
            // Its thread exited while the call was unwound: nothing resumes
            // here for it, and a later thread on the same stack must not run
            // its `after` with its data.
            g_pending.erase(g_pending.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        othersWaiting = true;
        ++i;
    }
    if (!resume)
    {
        // Nothing of ours was pending here: the entry should not be this.
        // Returning with pc unchanged would run this again forever.
        std::fprintf(stderr, "[rg] return hook at 0x%08X with nothing pending, stopping\n", pc);
        runtime->requestStop();
        return;
    }
    if (!othersWaiting)
    {
        runtime->replaceFunction(pc, resume);
    }
    if (due)
    {
        call.after(rdram, ctx, runtime, call.data);
    }
    resume(rdram, ctx, runtime);
}

// Puts resumeAtReturn in the table at the call's return address, so `after`
// runs when the caller resumes there. False when there is no entry to wait
// at.
bool deferUntilReturn(uint32_t returnPc, uint32_t sp, After after, uint32_t data, PS2Runtime *runtime)
{
    PS2Runtime::RecompiledFunction resume = nullptr;
    for (const Pending &call : g_pending)
    {
        if (call.returnPc == returnPc)
        {
            resume = call.resume;
            break;
        }
    }
    if (!resume)
    {
        // The table has a slot for every address in the code range, empty
        // where no function or resume label was generated. lookupFunction()
        // answers an empty slot with its missing-function fallback, which the
        // restore would then write into the slot: the run loop, which ends a
        // thread that reaches an empty slot, would instead spin on the
        // fallback. An empty slot is nowhere to wait.
        if (!runtime->hasFunction(returnPc))
        {
            return false;
        }
        resume = runtime->lookupFunction(returnPc);
        if (!runtime->replaceFunction(returnPc, &resumeAtReturn))
        {
            return false;
        }
    }
    g_pending.push_back(Pending{returnPc, sp, runtime->eeScheduler().currentThreadId(), after, data, resume});
    return true;
}

} // namespace

void callThen(PS2Runtime::RecompiledFunction original, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
              After after, uint32_t data)
{
    const uint32_t returnPc = getRegU32(ctx, 31);
    const uint32_t sp = getRegU32(ctx, 29);
    try
    {
        original(rdram, ctx, runtime);
    }
    catch (const EeDispatcherTransfer &)
    {
        // A wait the scheduler cannot suspend, a callback pushed onto this
        // thread or the thread's exit unwinds the stack with this instead of
        // returning. The thread, unless it exited, is dispatched again from
        // its saved pc, which is a resume label, and returns to the caller
        // through the table like any unwound call.
        if (!deferUntilReturn(returnPc, sp, after, data, runtime))
        {
            after(rdram, ctx, runtime, data);
        }
        throw;
    }
    if (ctx->pc == returnPc || ctx->pc == 0u)
    {
        after(rdram, ctx, runtime, data);
        return;
    }
    if (!deferUntilReturn(returnPc, sp, after, data, runtime))
    {
        // No table entry to wait at: better early than never.
        after(rdram, ctx, runtime, data);
    }
}

bool runToEnd(PS2Runtime::RecompiledFunction function, uint32_t entry, uint8_t *rdram, R5900Context &local,
              PS2Runtime *runtime)
{
    // No code is there: where the function's return lands.
    constexpr uint32_t kDone = 0x7FFFFFF0u;
    const EeScheduler &scheduler = runtime->eeScheduler();
    local.r[31] = _mm_set_epi64x(0, static_cast<int64_t>(kDone));
    local.pc = entry;
    PS2Runtime::RecompiledFunction next = function;
    // Each pass gets past at least one safe point, so this ends; the bound
    // only guards against a function that never returns.
    for (int pass = 0; pass < 1000000; ++pass)
    {
        const uint32_t at = local.pc;
        const uint32_t sp = getRegU32(&local, 29);
        const uint32_t ra = getRegU32(&local, 31);
        const uint64_t serial = scheduler.guestUnwindSerial();
        next(rdram, &local, runtime);
        if (local.pc == at && getRegU32(&local, 29) == sp && !scheduler.guestUnwoundSince(serial))
        {
            local.pc = ra;
        }
        if (local.pc == kDone)
        {
            return true;
        }
        if (local.pc == 0u || runtime->isStopRequested() || !runtime->hasFunction(local.pc))
        {
            return false;
        }
        next = runtime->lookupFunction(local.pc);
    }
    return false;
}

void describePending(std::FILE *out)
{
    const size_t count = g_pending.size();
    std::fprintf(out, "[stall] return hooks pending: %zu\n", count);
    for (size_t i = 0; i < count && i < g_pending.size(); ++i)
    {
        const Pending &call = g_pending[i];
        std::fprintf(out, "[stall]   return to 0x%08X, sp 0x%08X, thread %d, data 0x%08X\n", call.returnPc, call.sp,
                     call.thread, call.data);
    }
}

} // namespace rg::hooks
