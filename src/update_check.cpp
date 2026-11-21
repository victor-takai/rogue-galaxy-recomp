#include "update_check.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#endif

namespace rg::updates
{
namespace
{

#if defined(_WIN32)
constexpr const wchar_t *kHost = L"api.github.com";
constexpr const wchar_t *kPath = L"/repos/victor-takai/rogue-galaxy-recomp/releases?per_page=10";
#endif
constexpr const char *kReleasePage = "https://github.com/victor-takai/rogue-galaxy-recomp/releases/tag/";
// Long enough for the menu to have applied its settings, and for the game's
// first loading to be under way.
constexpr auto kDelay = std::chrono::seconds(4);
constexpr size_t kMostBytes = 1u << 20;

std::atomic<bool> g_enabled{true};
std::mutex g_mutex;
Release g_newer;

#if defined(_WIN32)
// The releases' JSON, or empty: GitHub lists them newest first, pre-releases
// included (0.1.0 is one, which /releases/latest leaves out).
std::string fetch()
{
    std::string body;
    HINTERNET session = WinHttpOpen(L"RogueGalaxyRecompiled", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
    {
        // Before Windows 8.1.
        session = WinHttpOpen(L"RogueGalaxyRecompiled", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session)
    {
        return body;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);
    HINTERNET connect = WinHttpConnect(session, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", kPath, nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
    if (request &&
        WinHttpSendRequest(request, L"Accept: application/vnd.github+json\r\n", static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA,
                           0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr))
    {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        while (status == 200)
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || available == 0 ||
                body.size() + available > kMostBytes)
            {
                break;
            }
            const size_t old = body.size();
            body.resize(old + available);
            DWORD read = 0;
            if (!WinHttpReadData(request, body.data() + old, available, &read))
            {
                body.resize(old);
                break;
            }
            body.resize(old + read);
        }
        if (status != 200)
        {
            std::fprintf(stderr, "[updates] GitHub answered %lu\n", static_cast<unsigned long>(status));
        }
    }
    if (request)
    {
        WinHttpCloseHandle(request);
    }
    if (connect)
    {
        WinHttpCloseHandle(connect);
    }
    WinHttpCloseHandle(session);
    return body;
}
#elif defined(__linux__)
// curl does the TLS (SteamOS and nearly every distribution ship it), so the
// game links no HTTP library; the same 5 seconds and 1 MB as WinHTTP's.
std::string fetch()
{
    std::string body;
    std::FILE *pipe = popen("curl --silent --fail --location --max-time 5 --user-agent RogueGalaxyRecompiled "
                            "--header 'Accept: application/vnd.github+json' "
                            "'https://api.github.com/repos/victor-takai/rogue-galaxy-recomp/releases?per_page=10' "
                            "2>/dev/null",
                            "r");
    if (!pipe)
    {
        return body;
    }
    char chunk[4096];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof chunk, pipe)) > 0 && body.size() + got <= kMostBytes)
    {
        body.append(chunk, got);
    }
    // Over the cap or not, never a half-read list.
    const int status = pclose(pipe);
    if (status != 0)
    {
        body.clear();
    }
    return body;
}
#else
std::string fetch()
{
    return {};
}
#endif

// The first release's tag: the JSON's first "tag_name".
std::string firstTag(const std::string &json)
{
    const size_t key = json.find("\"tag_name\"");
    if (key == std::string::npos)
    {
        return {};
    }
    const size_t open = json.find('"', json.find(':', key) + 1);
    const size_t close = open == std::string::npos ? open : json.find('"', open + 1);
    return close == std::string::npos ? std::string{} : json.substr(open + 1, close - open - 1);
}

// "v0.2.0" and "0.2.0" alike: the numbers before anything else.
std::vector<int> numbers(const std::string &version)
{
    std::vector<int> parts;
    size_t i = !version.empty() && (version[0] == 'v' || version[0] == 'V') ? 1u : 0u;
    while (i < version.size() && version[i] >= '0' && version[i] <= '9')
    {
        int value = 0;
        while (i < version.size() && version[i] >= '0' && version[i] <= '9')
        {
            value = value * 10 + (version[i] - '0');
            ++i;
        }
        parts.push_back(value);
        if (i < version.size() && version[i] == '.')
        {
            ++i;
        }
        else
        {
            break;
        }
    }
    return parts;
}

bool isNewer(const std::string &tag, const std::string &current)
{
    const std::vector<int> a = numbers(tag), b = numbers(current);
    if (a.empty() || b.empty())
    {
        return false;
    }
    for (size_t i = 0; i < std::max(a.size(), b.size()); ++i)
    {
        const int x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
        if (x != y)
        {
            return x > y;
        }
    }
    return false;
}

} // namespace

void start(const char *current)
{
    // RG_UPDATE_AS=VERSION checks as that version, to see the notice.
    if (const char *as = std::getenv("RG_UPDATE_AS"); as && *as)
    {
        current = as;
    }
    std::thread([version = std::string(current ? current : "")] {
        std::this_thread::sleep_for(kDelay);
        if (!g_enabled.load(std::memory_order_relaxed))
        {
            return;
        }
        const std::string tag = firstTag(fetch());
        if (tag.empty())
        {
            std::fprintf(stderr, "[updates] no release read from GitHub\n");
            return;
        }
        std::fprintf(stderr, "[updates] newest release %s, this is %s\n", tag.c_str(), version.c_str());
        if (isNewer(tag, version))
        {
            const std::lock_guard<std::mutex> lock(g_mutex);
            g_newer.version = tag[0] == 'v' || tag[0] == 'V' ? tag.substr(1) : tag;
            g_newer.url = kReleasePage + tag;
        }
    }).detach();
}

void setEnabled(bool on)
{
    g_enabled.store(on, std::memory_order_relaxed);
}

Release newer()
{
    const std::lock_guard<std::mutex> lock(g_mutex);
    return g_newer;
}

} // namespace rg::updates
