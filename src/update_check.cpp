#include "update_check.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>


namespace rg::updates
{
namespace
{

// This fork's releases; RG_UPDATE_REPO=owner/name points the check elsewhere (a test).
std::string repo()
{
    const char *given = std::getenv("RG_UPDATE_REPO");
    return given && *given ? given : "thibault-lgm/deck-galaxy";
}
std::string releasePage()
{
    return "https://github.com/" + repo() + "/releases/tag/";
}
// Long enough for the menu to have applied its settings, and for the game's
// first loading to be under way.
constexpr auto kDelay = std::chrono::seconds(4);
constexpr size_t kMostBytes = 1u << 20;

std::atomic<bool> g_enabled{true};
std::atomic<bool> g_checked{false};
std::atomic<bool> g_reachable{false};
std::mutex g_mutex;
Release g_newer;

// curl does the TLS (SteamOS and nearly every distribution ship it), so the
// game links no HTTP library; the same 5 seconds and 1 MB as WinHTTP's.
std::string fetch()
{
    std::string body;
    // RG_UPDATE_API=URL reads the release list from elsewhere (a file:// URL, in a test).
    const char *api = std::getenv("RG_UPDATE_API");
    const std::string url = api && *api ? std::string(api)
                                        : "https://api.github.com/repos/" + repo() + "/releases?per_page=10";
    const std::string command = "curl --silent --fail --location --max-time 5 --user-agent RogueGalaxyRecompiled "
                                "--header 'Accept: application/vnd.github+json' '" + url + "' 2>/dev/null";
    std::FILE *pipe = popen(command.c_str(), "r");
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

void start(const char *current, bool delayed)
{
    // RG_UPDATE_AS=VERSION checks as that version, to see the notice.
    if (const char *as = std::getenv("RG_UPDATE_AS"); as && *as)
    {
        current = as;
    }
    std::thread([version = std::string(current ? current : ""), delayed] {
        if (delayed)
        {
            std::this_thread::sleep_for(kDelay);
        }
        if (!g_enabled.load(std::memory_order_relaxed))
        {
            return;
        }
        const std::string tag = firstTag(fetch());
        g_checked.store(true, std::memory_order_release);
        if (tag.empty())
        {
            std::fprintf(stderr, "[updates] no release read from GitHub\n");
            return;
        }
        g_reachable.store(true, std::memory_order_release);
        std::fprintf(stderr, "[updates] newest release %s, this is %s\n", tag.c_str(), version.c_str());
        if (isNewer(tag, version))
        {
            const std::lock_guard<std::mutex> lock(g_mutex);
            g_newer.version = tag[0] == 'v' || tag[0] == 'V' ? tag.substr(1) : tag;
            g_newer.url = releasePage() + tag;
        }
    }).detach();
}

void setEnabled(bool on)
{
    g_enabled.store(on, std::memory_order_relaxed);
}

bool reachable()
{
    return g_reachable.load(std::memory_order_acquire);
}

bool checked()
{
    return g_checked.load(std::memory_order_acquire);
}

Release newer()
{
    const std::lock_guard<std::mutex> lock(g_mutex);
    return g_newer;
}

} // namespace rg::updates
