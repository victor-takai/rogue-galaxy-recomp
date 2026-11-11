#include "debug/crash_report.h"

#if defined(_WIN32)

#include <windows.h>
#include <psapi.h>

#include <cstdio>

namespace rg::debug
{
namespace
{

// "module+0xoffset" for an address, without allocating: the heap may be
// what broke.
void describe(DWORD64 address, char *out, size_t size)
{
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(address), &module) &&
        module)
    {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(module, path, MAX_PATH);
        const char *name = path;
        for (const char *p = path; *p; ++p)
        {
            if (*p == '\\' || *p == '/')
            {
                name = p + 1;
            }
        }
        std::snprintf(out, size, "%s+0x%llx", name,
                      static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
        return;
    }
    std::snprintf(out, size, "0x%llx", static_cast<unsigned long long>(address));
}

LONG WINAPI report(EXCEPTION_POINTERS *info)
{
    const EXCEPTION_RECORD *record = info->ExceptionRecord;
    char where[MAX_PATH + 32];
    describe(reinterpret_cast<DWORD64>(record->ExceptionAddress), where, sizeof(where));
    std::fprintf(stderr, "\n[crash] exception 0x%08lX at %s on thread %lu\n", record->ExceptionCode, where,
                 GetCurrentThreadId());
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
    {
        const ULONG_PTR access = record->ExceptionInformation[0];
        std::fprintf(stderr, "[crash] %s address 0x%llx\n",
                     access == 1 ? "writing" : access == 8 ? "executing" : "reading",
                     static_cast<unsigned long long>(record->ExceptionInformation[1]));
    }
    CONTEXT ctx = *info->ContextRecord;
    if (ctx.Rip == 0)
    {
        // A call through a null pointer: the caller is at the top of the stack.
        // Read through the kernel like the frames below: a bad stack pointer
        // would otherwise fault again here and lose the whole report.
        std::fprintf(stderr, "[crash]   #-- 0x0 (called through a null pointer)\n");
        DWORD64 ret = 0;
        if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(ctx.Rsp), &ret, sizeof(ret),
                              nullptr))
        {
            ctx.Rip = ret;
            ctx.Rsp += 8;
        }
    }
    for (int frame = 0; frame < 48 && ctx.Rip; ++frame)
    {
        describe(ctx.Rip, where, sizeof(where));
        std::fprintf(stderr, "[crash]   #%02d %s\n", frame, where);
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (!fn)
        {
            DWORD64 ret = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(ctx.Rsp), &ret, sizeof(ret),
                                   nullptr))
            {
                break;
            }
            ctx.Rip = ret;
            ctx.Rsp += 8;
            continue;
        }
        void *handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void installCrashReport()
{
    SetUnhandledExceptionFilter(&report);
}

} // namespace rg::debug

#elif defined(__linux__)

#include <execinfo.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace rg::debug
{
namespace
{

// Written with write(2) and glibc's backtrace_symbols_fd: no allocation and
// no stdio, as the heap may be what broke. Each frame comes as
// "module(+0xoffset) [address]"; offsets in the executable symbolize with
// `llvm-symbolizer --obj=build/game/src/roguegalaxy`.
void say(const char *text)
{
    const size_t length = std::strlen(text);
    if (write(STDERR_FILENO, text, length) < 0)
    {
        // Nowhere left to report.
    }
}

void hex(unsigned long long value, char *out)
{
    static const char digits[] = "0123456789abcdef";
    int length = 0;
    char reversed[17];
    do
    {
        reversed[length++] = digits[value & 15u];
        value >>= 4;
    } while (value);
    int i = 0;
    out[i++] = '0';
    out[i++] = 'x';
    while (length)
    {
        out[i++] = reversed[--length];
    }
    out[i] = 0;
}

void report(int signal, siginfo_t *info, void *)
{
    char number[24];
    say("\n[crash] signal ");
    hex(static_cast<unsigned long long>(signal), number);
    say(number);
    say(signal == SIGSEGV ? " (SIGSEGV)" : signal == SIGBUS ? " (SIGBUS)" : signal == SIGILL ? " (SIGILL)"
                                          : signal == SIGFPE ? " (SIGFPE)" : signal == SIGABRT ? " (SIGABRT)" : "");
    say(" at address ");
    hex(reinterpret_cast<unsigned long long>(info->si_addr), number);
    say(number);
    say(" on thread ");
    hex(static_cast<unsigned long long>(syscall(SYS_gettid)), number);
    say(number);
    say("\n");
    void *frames[48];
    const int count = backtrace(frames, 48);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    // SA_RESETHAND: returning runs the default action (a core dump, or the
    // abort), so the exit status still says how the game died.
}

} // namespace

void installCrashReport()
{
    // The handler runs on a stack of its own: a stack overflow is a crash too.
    static char stack[64 * 1024];
    stack_t alternate = {};
    alternate.ss_sp = stack;
    alternate.ss_size = sizeof stack;
    sigaltstack(&alternate, nullptr);
    struct sigaction action = {};
    action.sa_sigaction = &report;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    for (const int signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT})
    {
        sigaction(signal, &action, nullptr);
    }
}

} // namespace rg::debug

#else

namespace rg::debug
{
void installCrashReport() {}
} // namespace rg::debug

#endif
