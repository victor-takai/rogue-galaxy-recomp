// "Rogue Galaxy.exe": debug\RogueGalaxy.bat's work without its terminal.
//
// A batch file always opens a terminal, and RogueGalaxy.bat's stays the whole
// game: it waits for the game to keep its log and show an error. This does
// the same as a Windows program with no window of its own. From its folder
// (the repository's, where the build puts it) it sets the batch file's
// variables, keeps the previous log as build\last-run.prev.log, starts
// build\game\src\roguegalaxy.exe on disc\SCUS_974.90 with its output in
// build\last-run.log and no console, and waits; a game that ends with an
// error shows the log's last lines in a message box, as the batch file does
// in its terminal. Arguments go to the game after the disc, as the batch
// file's do. The debug\ launchers keep the batch file and its terminal
// (it was beside this one until the first release, 0.1.0).
//
// Plain C linked statically: it starts from the repository's folder, away
// from the DLLs the build puts beside the game.
#include <windows.h>

#include <stdio.h>
#include <wchar.h>

static const wchar_t kTitle[] = L"Rogue Galaxy";
static const wchar_t kGame[] = L"build\\game\\src\\roguegalaxy.exe";
static const wchar_t kDisc[] = L"disc\\SCUS_974.90";
static const wchar_t kLog[] = L"build\\last-run.log";
static const wchar_t kPreviousLog[] = L"build\\last-run.prev.log";
// The batch file's 15 lines, from at most the log's last 8 KB, each cut at
// 150 characters: the [gfx] lines run to 300 and more, which would make a
// message box as tall as the screen.
enum { kTailLines = 15, kTailBytes = 8192, kLineCharacters = 150 };

static void stop(const wchar_t *message)
{
    // In front: the game's window has gone, and this program has none.
    MessageBoxW(NULL, message, kTitle, MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
}

// The command line after this program's own name, skipped as the C runtime
// skips it.
static const wchar_t *arguments(void)
{
    const wchar_t *line = GetCommandLineW();
    if (*line == L'"')
    {
        ++line;
        while (*line && *line != L'"')
        {
            ++line;
        }
        if (*line == L'"')
        {
            ++line;
        }
    }
    else
    {
        while (*line && *line != L' ' && *line != L'\t')
        {
            ++line;
        }
    }
    while (*line == L' ' || *line == L'\t')
    {
        ++line;
    }
    return line;
}

// The game ended with `code`: the end of its log, in a message box.
static void showError(DWORD code)
{
    static char bytes[kTailBytes + 1];
    static wchar_t text[kTailBytes + 1];
    static wchar_t tail[kTailBytes + 1];
    static wchar_t message[kTailBytes + 256];
    DWORD read = 0;
    HANDLE log = CreateFileW(kLog, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log != INVALID_HANDLE_VALUE)
    {
        LARGE_INTEGER size;
        if (GetFileSizeEx(log, &size))
        {
            LARGE_INTEGER start;
            start.QuadPart = size.QuadPart > kTailBytes ? size.QuadPart - kTailBytes : 0;
            if (SetFilePointerEx(log, start, NULL, FILE_BEGIN) && !ReadFile(log, bytes, kTailBytes, &read, NULL))
            {
                read = 0;
            }
        }
        CloseHandle(log);
    }
    // Back from the end past kTailLines line breaks, a last one aside.
    DWORD begin = read;
    while (begin > 0 && (bytes[begin - 1] == '\n' || bytes[begin - 1] == '\r'))
    {
        --begin;
    }
    const DWORD end = begin;
    for (int lines = 0; begin > 0; --begin)
    {
        if (bytes[begin - 1] == '\n' && ++lines == kTailLines)
        {
            break;
        }
    }
    int length = MultiByteToWideChar(CP_UTF8, 0, bytes + begin, (int)(end - begin), text, kTailBytes);
    if (length <= 0 && end > begin)
    {
        length = MultiByteToWideChar(CP_ACP, 0, bytes + begin, (int)(end - begin), text, kTailBytes);
    }
    // The lines, the long ones cut (never longer than the text itself).
    int out = 0;
    int column = 0;
    for (int index = 0; index < length; ++index)
    {
        const wchar_t character = text[index];
        if (character == L'\r')
        {
            continue;
        }
        column = character == L'\n' ? 0 : column + 1;
        if (column <= kLineCharacters)
        {
            tail[out++] = character;
        }
        else if (column == kLineCharacters + 1)
        {
            tail[out - 1] = tail[out - 2] = tail[out - 3] = L'.';
        }
    }
    tail[out] = L'\0';
    // A crash's code (0xC0000005 and the like) reads better in hexadecimal.
    swprintf(message, sizeof(message) / sizeof(message[0]),
             code >= 0x10000u ? L"The game stopped with an error (exit code 0x%08lX). The end of %ls:\n\n%ls"
                              : L"The game stopped with an error (exit code %lu). The end of %ls:\n\n%ls",
             (unsigned long)code, kLog, tail);
    stop(message);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR commandLine, int show)
{
    (void)instance;
    (void)previous;
    (void)commandLine;
    (void)show;

    // The repository's folder: this program's own.
    static wchar_t folder[32768];
    const DWORD length = GetModuleFileNameW(NULL, folder, sizeof(folder) / sizeof(folder[0]));
    if (length == 0 || length >= sizeof(folder) / sizeof(folder[0]))
    {
        stop(L"Could not find this program's folder.");
        return 1;
    }
    wchar_t *slash = wcsrchr(folder, L'\\');
    if (slash)
    {
        *slash = L'\0';
    }
    if (!SetCurrentDirectoryW(folder))
    {
        stop(L"Could not enter this program's folder.");
        return 1;
    }

    // A second start from this folder while its game is up (nothing shows
    // for a moment after a double-click) would start a second game on the
    // same log and memory card. A file of the folder's, held open with no
    // sharing until this program ends (Windows then closes and deletes it,
    // however it ends), tells: it is the same file however the folder's path
    // is spelled (a subst drive, a short name, a link), and another folder's
    // game (a second copy, a worktree's) is free to run beside this one. The
    // game does not inherit it. Without build\ there is nothing to hold: the
    // game is not built, and that is said below.
    const HANDLE running = CreateFileW(L"build\\launcher.lock", GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
                                       FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (running == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION)
    {
        MessageBoxW(NULL, L"Rogue Galaxy is already running from this folder.", kTitle,
                    MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        return 0;
    }

    if (GetFileAttributesW(kGame) == INVALID_FILE_ATTRIBUTES)
    {
        stop(L"The game is not built yet: run Install.bat, or see docs\\BUILDING.md.");
        return 1;
    }
    if (GetFileAttributesW(kDisc) == INVALID_FILE_ATTRIBUTES)
    {
        stop(L"No extracted disc in disc\\: run  python setup.py extract  first.");
        return 1;
    }

    // RogueGalaxy.bat's variables (it says what they do): the [gfx] line a
    // second unless one is set already, and other programs' Vulkan layers
    // kept out of the game. Not under Wine or Proton, where the variable
    // reaches the host's Vulkan loader and would keep out the layers the
    // picture goes through (gamescope's on a Steam Deck, Steam's overlay).
    if (GetEnvironmentVariableW(L"DQ8_GFX_STATS_EVERY", NULL, 0) == 0)
    {
        SetEnvironmentVariableW(L"DQ8_GFX_STATS_EVERY", L"60");
    }
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const BOOL wine = ntdll != NULL && GetProcAddress(ntdll, "wine_get_version") != NULL;
    if (!wine)
    {
        SetEnvironmentVariableW(L"VK_LOADER_LAYERS_DISABLE", L"~implicit~");
    }

    // The previous run's log survives one more run (there may be none).
    CopyFileW(kLog, kPreviousLog, FALSE);
    SECURITY_ATTRIBUTES inherited = {sizeof(inherited), NULL, TRUE};
    HANDLE log = CreateFileW(kLog, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherited, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, NULL);
    if (log == INVALID_HANDLE_VALUE)
    {
        stop(L"Could not write build\\last-run.log.");
        return 1;
    }
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherited, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, NULL);

    // The batch file's command: the game, the disc, then this program's
    // arguments.
    static wchar_t command[32768];
    const wchar_t *extra = arguments();
    if (swprintf(command, sizeof(command) / sizeof(command[0]), *extra ? L"\"%ls\" \"%ls\" %ls" : L"\"%ls\" \"%ls\"",
                 kGame, kDisc, extra) < 0)
    {
        stop(L"The arguments are too long.");
        return 1;
    }
    STARTUPINFOW startup = {0};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = log;
    startup.hStdError = log;
    PROCESS_INFORMATION game = {0};
    const BOOL started =
        CreateProcessW(kGame, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &startup, &game);
    const DWORD error = GetLastError();
    CloseHandle(log);
    if (input != INVALID_HANDLE_VALUE)
    {
        CloseHandle(input);
    }
    if (!started)
    {
        wchar_t message[128];
        swprintf(message, sizeof(message) / sizeof(message[0]), L"The game could not be started (error %lu).",
                 (unsigned long)error);
        stop(message);
        return 1;
    }
    CloseHandle(game.hThread);

    WaitForSingleObject(game.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(game.hProcess, &code);
    CloseHandle(game.hProcess);
    if (code != 0)
    {
        showError(code);
    }
    return (int)code;
}
