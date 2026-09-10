#include "services/http_client.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char **environ;

namespace {

constexpr const char *kUserAgent = "LILYGO-Maps/0.1 (+https://github.com/LILYGO-UI/lilygo-ui-maps)";

void close_descriptor(int &descriptor) noexcept
{
    if (descriptor >= 0) ::close(descriptor);
    descriptor = -1;
}

void drain_descriptor(int &descriptor, std::string &destination, std::size_t limit, bool &limit_exceeded)
{
    std::array<char, 8192> buffer{};
    while (descriptor >= 0) {
        const auto count = ::read(descriptor, buffer.data(), buffer.size());
        if (count > 0) {
            const auto amount = static_cast<std::size_t>(count);
            const auto previous_size = destination.size();
            if (previous_size < limit) {
                const auto remaining = limit - previous_size;
                destination.append(buffer.data(), std::min(amount, remaining));
            }
            if (amount > limit - std::min(previous_size, limit)) limit_exceeded = true;
            continue;
        }
        if (count == 0) {
            close_descriptor(descriptor);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
        close_descriptor(descriptor);
    }
}

std::string trim_error(std::string value)
{
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ')) value.pop_back();
    return value;
}

}  // namespace

HttpClient::~HttpClient()
{
    cancel();
}

HttpResponse HttpClient::get(const std::string &url, const std::string &accept, std::size_t maximum_size)
{
    if (stopping_.load()) return {{}, "Network request was cancelled"};
    // The free ip-api endpoint is HTTP-only; keep the exception exact and do not follow redirects.
    const bool ip_api_fallback = url == "http://ip-api.com/json/";
    if (!ip_api_fallback && url.rfind("https://", 0) != 0) return {{}, "Map services must use HTTPS"};

    std::vector<std::string> arguments{
        "curl",          "--fail",          "--silent",       "--show-error",
        ip_api_fallback ? "--no-location" : "--location",
        "--proto",       ip_api_fallback ? "=http" : "=https",
        "--proto-redir", "=https",          "--connect-timeout",
        "8",             "--max-time",      "20",             "--retry",       "1",
        "--retry-delay", "1",               "--user-agent",   kUserAgent,       "--header",
        "Accept: " + accept, "--max-filesize", std::to_string(maximum_size), url,
    };
    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1U);
    for (auto &argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);

    int output_pipe[2]{-1, -1};
    int error_pipe[2]{-1, -1};
    if (::pipe(output_pipe) != 0 || ::pipe(error_pipe) != 0) {
        close_descriptor(output_pipe[0]);
        close_descriptor(output_pipe[1]);
        close_descriptor(error_pipe[0]);
        close_descriptor(error_pipe[1]);
        return {{}, "Could not create a network process pipe"};
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, output_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, error_pipe[0]);
    posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, error_pipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, output_pipe[1]);
    posix_spawn_file_actions_addclose(&actions, error_pipe[1]);

    pid_t process         = -1;
    const int spawn_error = posix_spawnp(&process, argv.front(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close_descriptor(output_pipe[1]);
    close_descriptor(error_pipe[1]);
    if (spawn_error != 0) {
        close_descriptor(output_pipe[0]);
        close_descriptor(error_pipe[0]);
        return {{}, std::string("Could not start curl: ") + std::strerror(spawn_error)};
    }

    add_process(process);
    if (stopping_.load()) ::kill(process, SIGTERM);
    (void)::fcntl(output_pipe[0], F_SETFL, ::fcntl(output_pipe[0], F_GETFL) | O_NONBLOCK);
    (void)::fcntl(error_pipe[0], F_SETFL, ::fcntl(error_pipe[0], F_GETFL) | O_NONBLOCK);

    HttpResponse response;
    bool limit_exceeded = false;
    while (output_pipe[0] >= 0 || error_pipe[0] >= 0) {
        pollfd descriptors[2]{{output_pipe[0], POLLIN | POLLHUP, 0}, {error_pipe[0], POLLIN | POLLHUP, 0}};
        const int polled = ::poll(descriptors, 2, 250);
        if (polled < 0 && errno != EINTR) break;
        drain_descriptor(output_pipe[0], response.body, maximum_size, limit_exceeded);
        drain_descriptor(error_pipe[0], response.error, 8192U, limit_exceeded);
        if (limit_exceeded || stopping_.load()) {
            ::kill(process, SIGTERM);
            break;
        }
    }
    close_descriptor(output_pipe[0]);
    close_descriptor(error_pipe[0]);

    int status = 0;
    while (::waitpid(process, &status, 0) < 0 && errno == EINTR) {
    }
    remove_process(process);

    if (limit_exceeded) return {{}, "Map service response exceeded its size limit"};
    if (stopping_.load()) return {{}, "Network request was cancelled"};
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        response.body.clear();
        response.error = trim_error(response.error);
        if (response.error.empty()) response.error = "Map service request failed";
        return response;
    }
    response.error.clear();
    return response;
}

void HttpClient::cancel() noexcept
{
    stopping_.store(true);
    std::lock_guard<std::mutex> lock(processes_mutex_);
    for (const pid_t process : processes_) ::kill(process, SIGTERM);
}

void HttpClient::add_process(pid_t process) noexcept
{
    std::lock_guard<std::mutex> lock(processes_mutex_);
    processes_.insert(process);
}

void HttpClient::remove_process(pid_t process) noexcept
{
    std::lock_guard<std::mutex> lock(processes_mutex_);
    processes_.erase(process);
}
