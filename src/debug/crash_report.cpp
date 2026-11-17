#include "debug/crash_report.h"


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

