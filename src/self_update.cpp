#include "self_update.h"

#include "update_check.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#if defined(__linux__)
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace rg::selfupdate
{
namespace
{

std::mutex g_mutex;
State g_state = State::Checking; // Working, Failed or Done once started
std::string g_text;
float g_progress = -1.0f;

constexpr const char *kProgressFile = "build/update.progress";
constexpr const char *kLogFile = "build/update.log";

void setWorking(const std::string &text, float progress)
{
    const std::lock_guard<std::mutex> lock(g_mutex);
    g_state = State::Working;
    g_text = text;
    g_progress = progress;
}

void finish(State state, const std::string &text)
{
    const std::lock_guard<std::mutex> lock(g_mutex);
    g_state = state;
    g_text = text;
    g_progress = state == State::Done ? 1.0f : -1.0f;
}

// The script's last progress line: "<fraction or -1> <text>".
void readProgress()
{
    std::ifstream file(kProgressFile);
    std::string line;
    if (!file || !std::getline(file, line))
    {
        return;
    }
    char *end = nullptr;
    const float fraction = std::strtof(line.c_str(), &end);
    std::string text = end ? end : "";
    while (!text.empty() && text.front() == ' ')
    {
        text.erase(text.begin());
    }
    setWorking(text, fraction);
}

#if defined(__linux__)
void run()
{
    std::remove(kProgressFile);
    setWorking("Starting", -1.0f);
    // update.sh's output goes to the log, and its input from nowhere.
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, kLogFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);
    const char *const arguments[] = {"sh", "update.sh", nullptr};
    pid_t pid = 0;
    const int error = posix_spawnp(&pid, "sh", &actions, nullptr, const_cast<char *const *>(arguments), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (error != 0)
    {
        finish(State::Failed, "Could not start update.sh");
        return;
    }
    int status = 0;
    for (;;)
    {
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid)
        {
            break;
        }
        if (done < 0)
        {
            finish(State::Failed, "Lost the update process");
            return;
        }
        readProgress();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        std::string why = "The update failed: see build/update.log";
        std::ifstream file(kProgressFile);
        std::string line;
        if (file && std::getline(file, line) && line.rfind("error ", 0) == 0)
        {
            why = line.substr(6);
        }
        finish(State::Failed, why);
        return;
    }
    finish(State::Done, "Updated: restarting");
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    // The launcher holds build/.launcher.lock on file descriptor 9 for as
    // long as the game runs; the script would find it taken by this process.
    close(9);
    const char *const again[] = {"sh", "rogue-galaxy.sh", nullptr};
    execvp("sh", const_cast<char *const *>(again));
    finish(State::Failed, "Updated, but the game could not start again: start it from Steam");
}
#endif

} // namespace

Status status()
{
    Status result;
    result.state = State::Checking;
    {
        const std::lock_guard<std::mutex> lock(g_mutex);
        if (g_state != State::Checking)
        {
            result.state = g_state;
            result.text = g_text;
            result.progress = g_progress;
        }
    }
    const updates::Release newer = updates::newer();
    result.version = newer.version;
    if (result.state == State::Checking && updates::checked())
    {
        result.state = !updates::reachable() ? State::Offline
                       : newer.version.empty() ? State::UpToDate
                                               : State::Available;
    }
    // A failed update leaves the release Available again to retry.
    return result;
}

void start()
{
#if defined(__linux__)
    const Status now = status();
    if (now.state != State::Available && now.state != State::Failed)
    {
        return;
    }
    setWorking("Starting", -1.0f);
    std::thread(run).detach();
#endif
}

} // namespace rg::selfupdate
