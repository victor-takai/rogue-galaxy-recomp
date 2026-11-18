#include "achievements.h"

#include "mini_json.h"

extern "C"
{
#include "rc_client.h"
#include "rhash/md5.h"
}

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <vector>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef RG_APP_VERSION
#define RG_APP_VERSION "0"
#endif

namespace rg::achievements
{
namespace
{
// The PS2's main memory, 32 MB: RetroAchievements' addresses are offsets in it.
constexpr uint32_t kEeRamSize = 0x02000000u;
constexpr const char *kExecutableName = "SCUS_974.90";
// The ids from which RetroAchievements adds its own notices to a set (the
// "Unknown Emulator" warning): not achievements to show or to celebrate.
constexpr uint32_t kNoticeIdsFrom = 100000000u;

enum class State
{
    Off,
    Starting,
    LoggingIn,
    Loading,
    Running,
    Failed
};

std::atomic<bool> g_enabled{false};
State g_state = State::Off;
bool g_test = false;
std::string g_failure;
std::string g_executable;
std::string g_summary;
const uint8_t *g_rdram = nullptr;
rc_client_t *g_client = nullptr;

std::mutex g_listMutex;
std::vector<Row> g_list;

std::mutex g_toastMutex;
std::deque<Toast> g_toasts;

void pushToast(std::string text, bool earned = false, std::string badge = {})
{
    std::lock_guard<std::mutex> lock(g_toastMutex);
    g_toasts.push_back({std::move(text), earned, std::move(badge)});
}

void fail(const std::string &why)
{
    g_state = State::Failed;
    g_failure = why;
    std::fprintf(stderr, "[ra] %s\n", why.c_str());
}

// The settings folder: the one RG_SETTINGS_FILE is in, else the usual one.
std::filesystem::path settingsDir()
{
    if (const char *file = std::getenv("RG_SETTINGS_FILE"); file && *file)
    {
        return std::filesystem::path(file).parent_path();
    }
    const char *home = std::getenv("HOME");
    return std::filesystem::path(home && *home ? home : ".") / ".local" / "share" / "RogueGalaxy";
}

// ---- the network: curl, on a worker thread ----------------------------------

struct Request
{
    uint32_t generation = 0;
    std::string url;
    std::string post;
    std::string contentType;
    rc_client_server_callback_t callback = nullptr;
    void *callbackData = nullptr;
};

struct Response
{
    uint32_t generation = 0;
    std::string body;
    int status = 0;
    rc_client_server_callback_t callback = nullptr;
    void *callbackData = nullptr;
};

std::atomic<uint32_t> g_requestNumber{0};
// Bumped when the client is dropped: answers to its requests are dropped too.
std::atomic<uint32_t> g_generation{0};
std::mutex g_netMutex;
std::condition_variable g_netWake;
std::deque<Request> g_requests;
std::deque<Response> g_responses;
bool g_netStarted = false;

// Runs curl for one request; the body goes through a file only the user can
// read (it holds the token), never on the command line.
Response perform(const Request &request)
{
    Response response;
    response.generation = request.generation;
    response.callback = request.callback;
    response.callbackData = request.callbackData;
    response.status = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
    if (request.url.find('\'') != std::string::npos)
    {
        response.status = RC_API_SERVER_RESPONSE_CLIENT_ERROR;
        return response;
    }
    std::string command = "curl --silent --location --max-time 30 --user-agent 'RogueGalaxyRecompiled/" RG_APP_VERSION
                          " rcheevos/12' -w '\\n%{http_code}'";
    std::filesystem::path bodyFile;
    if (!request.post.empty())
    {
        bodyFile = settingsDir() / "ra" /
                   ("request-" + std::to_string(getpid()) + "-" + std::to_string(g_requestNumber.fetch_add(1)) + ".tmp");
        std::error_code ec;
        std::filesystem::create_directories(bodyFile.parent_path(), ec);
        {
            std::ofstream out(bodyFile, std::ios::binary | std::ios::trunc);
            out << request.post;
        }
        chmod(bodyFile.c_str(), 0600);
        command += " -H 'Content-Type: " +
                   (request.contentType.empty() ? std::string("application/x-www-form-urlencoded")
                                                : request.contentType) +
                   "' --data-binary '@" + bodyFile.string() + "'";
    }
    command += " '" + request.url + "' 2>/dev/null";
    std::string output;
    if (std::FILE *pipe = popen(command.c_str(), "r"))
    {
        char chunk[4096];
        size_t got;
        while ((got = std::fread(chunk, 1, sizeof chunk, pipe)) > 0)
        {
            output.append(chunk, got);
        }
        pclose(pipe);
    }
    if (!bodyFile.empty())
    {
        std::error_code ec;
        std::filesystem::remove(bodyFile, ec);
    }
    const size_t cut = output.rfind('\n');
    if (cut != std::string::npos)
    {
        const int code = std::atoi(output.c_str() + cut + 1);
        if (code > 0)
        {
            response.status = code;
            response.body = output.substr(0, cut);
        }
    }
    return response;
}

void netThread()
{
    for (;;)
    {
        Request request;
        {
            std::unique_lock<std::mutex> lock(g_netMutex);
            g_netWake.wait(lock, [] { return !g_requests.empty(); });
            request = std::move(g_requests.front());
            g_requests.pop_front();
        }
        Response response = perform(request);
        std::lock_guard<std::mutex> lock(g_netMutex);
        g_responses.push_back(std::move(response));
    }
}

void serverCall(const rc_api_request_t *request, rc_client_server_callback_t callback, void *callbackData,
                rc_client_t *)
{
    Request queued;
    queued.url = request->url ? request->url : "";
    queued.post = request->post_data ? request->post_data : "";
    queued.contentType = request->content_type ? request->content_type : "";
    queued.generation = g_generation.load();
    queued.callback = callback;
    queued.callbackData = callbackData;
    std::lock_guard<std::mutex> lock(g_netMutex);
    if (!g_netStarted)
    {
        g_netStarted = true;
        std::thread(netThread).detach();
    }
    g_requests.push_back(std::move(queued));
    g_netWake.notify_one();
}

// Hands the answers that came in to rc_client, on the frame thread.
void deliverResponses()
{
    for (;;)
    {
        Response response;
        {
            std::lock_guard<std::mutex> lock(g_netMutex);
            if (g_responses.empty())
            {
                return;
            }
            response = std::move(g_responses.front());
            g_responses.pop_front();
        }
        if (response.generation != g_generation.load() || !response.callback)
        {
            continue; // for a client that is gone
        }
        rc_api_server_response_t answer{};
        answer.body = response.body.c_str();
        answer.body_length = response.body.size();
        answer.http_status_code = response.status;
        response.callback(&answer, response.callbackData);
    }
}

// ---- the badges --------------------------------------------------------------

// The pictures of the achievements, from RetroAchievements' media server, one
// at a time on their own thread, into ra/badges/ (a badge is its authors'
// artwork: kept on the player's machine, never in the repository).
struct BadgeJob
{
    std::string url;
    std::filesystem::path path;
};
std::mutex g_badgeMutex;
std::condition_variable g_badgeWake;
std::deque<BadgeJob> g_badgeJobs;
std::set<std::string> g_badgeAsked;
bool g_badgeStarted = false;

void badgeThread()
{
    for (;;)
    {
        BadgeJob job;
        {
            std::unique_lock<std::mutex> lock(g_badgeMutex);
            g_badgeWake.wait(lock, [] { return !g_badgeJobs.empty(); });
            job = std::move(g_badgeJobs.front());
            g_badgeJobs.pop_front();
        }
        const std::string partial = job.path.string() + ".part";
        const std::string command = "curl --silent --fail --location --max-time 20 -o '" + partial + "' '" +
                                    job.url + "' 2>/dev/null";
        std::error_code ec;
        if (std::system(command.c_str()) == 0)
        {
            std::filesystem::rename(partial, job.path, ec);
        }
        else
        {
            std::filesystem::remove(partial, ec);
        }
    }
}

// The file a badge URL is kept in, asking for it when it is not there yet
// (list() hands it out once it is).
std::string badgeFile(const char *url)
{
    if (!url || !*url || std::strchr(url, '\''))
    {
        return {};
    }
    const std::string text = url;
    const std::filesystem::path path =
        settingsDir() / "ra" / "badges" / text.substr(text.find_last_of('/') + 1);
    std::error_code ec;
    if (std::filesystem::exists(path, ec))
    {
        return path.string();
    }
    std::lock_guard<std::mutex> lock(g_badgeMutex);
    if (g_badgeAsked.insert(text).second)
    {
        std::filesystem::create_directories(path.parent_path(), ec);
        g_badgeJobs.push_back({text, path});
        if (!g_badgeStarted)
        {
            g_badgeStarted = true;
            std::thread(badgeThread).detach();
        }
        g_badgeWake.notify_one();
    }
    return path.string();
}

// ---- the game's memory --------------------------------------------------------

uint32_t readMemory(uint32_t address, uint8_t *buffer, uint32_t count, rc_client_t *)
{
    if (!g_rdram || address >= kEeRamSize)
    {
        return 0u;
    }
    count = std::min(count, kEeRamSize - address);
    std::memcpy(buffer, g_rdram + address, count);
    return count;
}

// RetroAchievements' hash of a PS2 disc: the md5 of the boot file's name and
// its contents.
bool gameHash(std::string &hash)
{
    std::ifstream in(g_executable, std::ios::binary);
    if (!in)
    {
        return false;
    }
    md5_state_t md5;
    md5_init(&md5);
    md5_append(&md5, reinterpret_cast<const md5_byte_t *>(kExecutableName),
               static_cast<int>(std::strlen(kExecutableName)));
    char chunk[65536];
    while (in.read(chunk, sizeof chunk) || in.gcount() > 0)
    {
        md5_append(&md5, reinterpret_cast<const md5_byte_t *>(chunk), static_cast<int>(in.gcount()));
    }
    md5_byte_t digest[16];
    md5_finish(&md5, digest);
    char text[33];
    for (int i = 0; i < 16; ++i)
    {
        std::snprintf(text + i * 2, 3, "%02x", digest[i]);
    }
    hash = text;
    return true;
}

// ---- rc_client's events -----------------------------------------------------

void refreshSummary();

void onEvent(const rc_client_event_t *event, rc_client_t *)
{
    switch (event->type)
    {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
        if (event->achievement && event->achievement->id >= kNoticeIdsFrom)
        {
            std::fprintf(stderr, "[ra] notice %u: %s\n", event->achievement->id, event->achievement->title);
        }
        else if (event->achievement)
        {
            pushToast(std::string(g_test ? "[test] Achievement unlocked: " : "Achievement unlocked: ") + event->achievement->title + " (" +
                          std::to_string(event->achievement->points) + ")",
                      true, badgeFile(event->achievement->badge_url));
            std::fprintf(stderr, "[ra] unlocked %u: %s\n", event->achievement->id, event->achievement->title);
            refreshSummary();
        }
        break;
    case RC_CLIENT_EVENT_GAME_COMPLETED:
        pushToast("RetroAchievements: every achievement earned", true);
        break;
    case RC_CLIENT_EVENT_SERVER_ERROR:
        if (event->server_error)
        {
            std::fprintf(stderr, "[ra] server error (%s): %s\n", event->server_error->api,
                         event->server_error->error_message);
            pushToast(std::string("RetroAchievements: ") + event->server_error->error_message);
        }
        break;
    case RC_CLIENT_EVENT_DISCONNECTED:
        pushToast("RetroAchievements: offline, unlocks wait for the connection");
        break;
    case RC_CLIENT_EVENT_RECONNECTED:
        pushToast("RetroAchievements: back online");
        break;
    default:
        break;
    }
}

// The set as rc_client lists it, copied for the menu: RetroAchievements'
// page shows the earned ones first, then the others, each in the set's order.
void refreshList()
{
    std::vector<Row> earned, others;
    rc_client_achievement_list_t *list = rc_client_create_achievement_list(
        g_client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    if (list)
    {
        for (uint32_t b = 0; b < list->num_buckets; ++b)
        {
            const rc_client_achievement_bucket_t &bucket = list->buckets[b];
            for (uint32_t i = 0; i < bucket.num_achievements; ++i)
            {
                const rc_client_achievement_t *a = bucket.achievements[i];
                if (a->id >= kNoticeIdsFrom)
                {
                    continue; // RetroAchievements' own warning, not one of the set
                }
                const bool isEarned = a->state == RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED;
                Row row{a->title ? a->title : "", a->description ? a->description : "", a->points, isEarned, {}};
                // The colour badge once earned, the grey one before.
                row.badge = badgeFile(isEarned ? a->badge_url : a->badge_locked_url);
                (isEarned ? earned : others).push_back(std::move(row));
            }
        }
        rc_client_destroy_achievement_list(list);
    }
    earned.insert(earned.end(), std::make_move_iterator(others.begin()), std::make_move_iterator(others.end()));
    std::lock_guard<std::mutex> lock(g_listMutex);
    g_list = std::move(earned);
}

void refreshSummary()
{
    refreshList();
    rc_client_user_game_summary_t summary{};
    rc_client_get_user_game_summary(g_client, &summary);
    const rc_client_game_t *game = rc_client_get_game_info(g_client);
    std::ostringstream text;
    text << (game && game->title ? game->title : "Rogue Galaxy") << ": " << summary.num_unlocked_achievements
         << " of " << summary.num_core_achievements << " achievements";
    g_summary = text.str();
}

void onLoaded(int result, const char *error, rc_client_t *, void *)
{
    if (result != RC_OK)
    {
        fail(std::string("the game was not found by RetroAchievements: ") + (error ? error : "?"));
        return;
    }
    g_state = State::Running;
    refreshSummary();
    std::fprintf(stderr, "[ra] %s\n", g_summary.c_str());
    pushToast("RetroAchievements: " + g_summary);
}

void onLogin(int result, const char *error, rc_client_t *client, void *)
{
    if (result != RC_OK)
    {
        fail(std::string("sign-in refused: ") + (error ? error : "?") +
             " (sign in again on the Achievements page of the menu's Settings)");
        return;
    }
    std::string hash;
    if (!gameHash(hash))
    {
        fail("cannot read " + g_executable + " for the game's hash");
        return;
    }
    g_state = State::Loading;
    rc_client_begin_load_game(client, hash.c_str(), onLoaded, nullptr);
}

// The saved account: ra/login.json with "user" and "token".
bool readLogin(std::string &user, std::string &token)
{
    std::ifstream in(settingsDir() / "ra" / "login.json", std::ios::binary);
    if (!in)
    {
        return false;
    }
    std::stringstream text;
    text << in.rdbuf();
    const std::string json = text.str();
    rg::json::Value root;
    if (!rg::json::Reader(json).parse(root) || root.type != rg::json::Value::Type::Object)
    {
        return false;
    }
    const rg::json::Value *u = root.find("user");
    const rg::json::Value *t = root.find("token");
    if (!u || !t || u->string.empty() || t->string.empty())
    {
        return false;
    }
    user = u->string;
    token = t->string;
    return true;
}

void start()
{
    g_state = State::Starting;
    std::string user, token;
    if (!readLogin(user, token))
    {
        fail("not signed in: sign in on the Achievements page of the menu's Settings");
        return;
    }
    g_client = rc_client_create(readMemory, serverCall);
    rc_client_set_event_handler(g_client, onEvent);
    rc_client_set_hardcore_enabled(g_client, 0);
    // RG_ACHIEVEMENTS_TEST=1: a check of the conditions against a save, that
    // sends nothing. Spectator mode raises the events without the unlock
    // calls; encore lets what the account already earned trigger again.
    if (const char *test = std::getenv("RG_ACHIEVEMENTS_TEST"); test && *test && *test != '0')
    {
        g_test = true;
        rc_client_set_spectator_mode_enabled(g_client, 1);
        rc_client_set_encore_mode_enabled(g_client, 1);
        std::fprintf(stderr, "[ra] test mode: nothing is sent to RetroAchievements\n");
    }
    g_state = State::LoggingIn;
    rc_client_begin_login_with_token(g_client, user.c_str(), token.c_str(), onLogin, nullptr);
}

// ---- the account ----------------------------------------------------------------

std::mutex g_accountMutex;
std::string g_accountMessage;
bool g_accountBusy = false;
bool g_accountFailed = false;
// Set by signing in or out, for the frame thread to drop the client and start
// again: rc_client belongs to that thread.
std::atomic<bool> g_resetRequested{false};

std::string urlEncode(const std::string &text)
{
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : text)
    {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        {
            out += static_cast<char>(c);
        }
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

void setAccountMessage(const std::string &message, bool busy, bool failed)
{
    std::lock_guard<std::mutex> lock(g_accountMutex);
    g_accountMessage = message;
    g_accountBusy = busy;
    g_accountFailed = failed;
}

// The reply of the sign-in: the user's name and token on success.
void finishSignIn(const std::string &user, const Response &response)
{
    rg::json::Value root;
    if (response.status <= 0 || !rg::json::Reader(response.body).parse(root) ||
        root.type != rg::json::Value::Type::Object)
    {
        setAccountMessage("RetroAchievements could not be reached.", false, true);
        return;
    }
    const rg::json::Value *ok = root.find("Success");
    const rg::json::Value *token = root.find("Token");
    if (!ok || !ok->boolean || !token || token->string.empty())
    {
        const rg::json::Value *error = root.find("Error");
        setAccountMessage(error && !error->string.empty() ? error->string : "Sign-in refused.", false, true);
        return;
    }
    const rg::json::Value *name = root.find("User");
    const std::filesystem::path file = settingsDir() / "ra" / "login.json";
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    {
        const int fd = open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0)
        {
            setAccountMessage("The sign-in could not be saved.", false, true);
            return;
        }
        std::string json = "{\"user\": \"" + (name && !name->string.empty() ? name->string : user) +
                           "\", \"token\": \"" + token->string + "\"}";
        const bool written = write(fd, json.data(), json.size()) == static_cast<ssize_t>(json.size());
        close(fd);
        chmod(file.c_str(), 0600);
        if (!written)
        {
            setAccountMessage("The sign-in could not be saved.", false, true);
            return;
        }
    }
    setAccountMessage("", false, false);
    g_resetRequested.store(true);
}

// The frame thread's side of signing in or out: drop the client and what it
// held, so the next frame starts again with whatever account is saved.
void resetClient()
{
    if (g_client)
    {
        rc_client_destroy(g_client);
        g_client = nullptr;
    }
    g_generation.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(g_netMutex);
        g_responses.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_listMutex);
        g_list.clear();
    }
    g_summary.clear();
    g_failure.clear();
    g_state = State::Off;
}
} // namespace

Account account()
{
    Account out;
    std::string user, token;
    out.signedIn = readLogin(user, token);
    if (out.signedIn)
    {
        out.user = user;
    }
    std::lock_guard<std::mutex> lock(g_accountMutex);
    out.message = g_accountMessage;
    out.busy = g_accountBusy;
    out.failed = g_accountFailed;
    return out;
}

void signIn(const std::string &user, const std::string &password)
{
    {
        std::lock_guard<std::mutex> lock(g_accountMutex);
        if (g_accountBusy)
        {
            return;
        }
        g_accountBusy = true;
        g_accountFailed = false;
        g_accountMessage = "Signing in...";
    }
    Request request;
    request.url = "https://retroachievements.org/dorequest.php";
    request.post = "r=login2&u=" + urlEncode(user) + "&p=" + urlEncode(password);
    std::thread([request, user]() mutable {
        const Response response = perform(request);
        request.post.clear(); // the password goes with it
        finishSignIn(user, response);
    }).detach();
}

void signOut()
{
    std::error_code ec;
    std::filesystem::remove(settingsDir() / "ra" / "login.json", ec);
    setAccountMessage("Signed out.", false, false);
    g_resetRequested.store(true);
}

void setEnabled(bool on)
{
    g_enabled.store(on, std::memory_order_relaxed);
}

bool signedIn()
{
    std::error_code ec;
    return std::filesystem::exists(settingsDir() / "ra" / "login.json", ec);
}

bool enabled()
{
    return g_enabled.load(std::memory_order_relaxed);
}

void setExecutable(const std::string &path)
{
    g_executable = path;
}

void frame(const uint8_t *rdram)
{
    if (const char *env = std::getenv("RG_ACHIEVEMENTS"); env && *env && *env != '0')
    {
        g_enabled.store(true, std::memory_order_relaxed);
    }
    g_rdram = rdram;
    if (g_resetRequested.exchange(false))
    {
        resetClient();
    }
    if (!enabled())
    {
        return;
    }
    if (g_state == State::Off)
    {
        start();
    }
    if (!g_client)
    {
        return;
    }
    deliverResponses();
    if (g_state == State::Running)
    {
        rc_client_do_frame(g_client);
    }
    else
    {
        rc_client_idle(g_client);
    }
}

std::vector<Row> list()
{
    std::vector<Row> rows;
    {
        std::lock_guard<std::mutex> lock(g_listMutex);
        rows = g_list;
    }
    // A badge is handed out once it is on disk.
    for (Row &row : rows)
    {
        std::error_code ec;
        if (!row.badge.empty() && !std::filesystem::exists(row.badge, ec))
        {
            row.badge.clear();
        }
    }
    return rows;
}

bool takeToast(Toast &toast)
{
    std::lock_guard<std::mutex> lock(g_toastMutex);
    if (g_toasts.empty())
    {
        return false;
    }
    toast = std::move(g_toasts.front());
    g_toasts.pop_front();
    return true;
}

std::string status()
{
    switch (g_state)
    {
    case State::Off:
        return enabled() ? "Starting" : "Off";
    case State::Starting:
    case State::LoggingIn:
        return "Signing in";
    case State::Loading:
        return "Loading the achievements";
    case State::Running:
        return g_summary;
    case State::Failed:
        return g_failure;
    }
    return {};
}

} // namespace rg::achievements
