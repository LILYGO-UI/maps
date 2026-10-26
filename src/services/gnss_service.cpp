#include "services/gnss_service.hpp"
#include "services/gnss_protocol.hpp"

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/file.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::vector<int> response_fields(const std::vector<std::string> &lines, const std::string &prefix)
{
    for (auto line = lines.rbegin(); line != lines.rend(); ++line) {
        auto fields = gnss_protocol::integer_response(*line, prefix);
        if (!fields.empty()) return fields;
    }
    return {};
}

std::string configured_port()
{
    for (const auto *name : {"LILYGO_MAPS_GNSS_PORT", "CELLULAR_PORT"})
        if (const auto *value = std::getenv(name); value && *value) return value;
    return "/dev/ttyACM0";
}
}  // namespace

struct GnssService::Impl {
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::atomic<bool> stopping{false};
    std::thread worker;
    std::optional<GnssFix> fix;
    std::uint64_t sequence = 0;
    std::string port;
    std::string input;
    int fd                   = -1;
    int lock_fd              = -1;
    bool own_session         = false;
    bool own_activation      = false;
    bool own_nmea            = false;
    bool single_fix          = false;
    bool rearm               = false;
    bool recover_sleeping    = false;
    bool preserve_rearm_fix  = false;
    bool nmea_enabled        = false;
    bool received_navigation = false;
    bool debug               = false;
    Clock::time_point rearm_at{};
    Clock::time_point attached_at{};
    Clock::time_point navigation_started_at{};

    ~Impl()
    {
        stopping = true;
        wake.notify_all();
        if (worker.joinable()) worker.join();
    }

    void clear_fix()
    {
        std::lock_guard<std::mutex> guard(mutex);
        fix.reset();
    }

    void log(const std::string &message) const
    {
        std::fprintf(stderr, "[maps-gnss] %s: %s\n", port.c_str(), message.c_str());
    }

    bool port_error(const char *operation) const
    {
        log(std::string(operation) + ": " + std::strerror(errno));
        return false;
    }

    void process_line(const std::string &line)
    {
        if (debug) log("< " + line);
        const auto position = gnss_protocol::parse_position(line);
        // No-fix NMEA (including GSV) proves the output path works. Do not
        // restart a receiver that is still acquiring its first satellite fix.
        if (line.rfind("$", 0) == 0 || line.rfind("#XGNSSNMEA:", 0) == 0 ||
            position.kind != gnss_protocol::PositionKind::Ignored)
            received_navigation = true;
        if (position.kind == gnss_protocol::PositionKind::Fix) {
            recover_sleeping = false;
            std::lock_guard<std::mutex> guard(mutex);
            fix = GnssFix{position.point, ++sequence, Clock::now()};
        } else if (position.kind == gnss_protocol::PositionKind::Invalid) {
            clear_fix();
        }
        const auto state = gnss_protocol::integer_response(line, "#XGNSS:");
        if (state.size() >= 2) {
            if (state[0] == 1 && (state[1] == 1 || state[1] == 2)) recover_sleeping = false;
            if (state[0] == 0 || (state[1] == 0 && !preserve_rearm_fix) || state[1] == 3) clear_fix();
            if (single_fix && (state[1] == 3 || state[1] == 4)) {
                rearm    = true;
                rearm_at = Clock::now() + 1s;
            }
        }
    }

    void close_port()
    {
        if (fd >= 0) ::close(fd);
        if (lock_fd >= 0) {
            ::flock(lock_fd, LOCK_UN);
            ::close(lock_fd);
        }
        fd = lock_fd = -1;
        input.clear();
        own_session = own_activation = own_nmea = single_fix = rearm = recover_sleeping = preserve_rearm_fix = false;
        nmea_enabled = received_navigation = false;
        clear_fix();
    }

    bool open_port()
    {
        // Keep this naming compatible with lilygo-ui-cellular and the device launcher.
        const auto lock = port == "/dev/ttyACM0"
                              ? "/tmp/lilygo_nrf9151_uart.lock"
                              : "/tmp/lilygo-cellular-" + std::to_string(std::hash<std::string>{}(port)) + ".lock";
        lock_fd         = ::open(lock.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (lock_fd < 0) return port_error("open UART lock");
        if (::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) return port_error("acquire UART lock");
        fd = ::open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) return port_error("open UART");
        termios options{};
        if (::tcgetattr(fd, &options) != 0) return port_error("read UART settings");
        ::cfmakeraw(&options);
        ::cfsetispeed(&options, B115200);
        ::cfsetospeed(&options, B115200);
        options.c_cflag |= CLOCAL | CREAD;
        options.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
        options.c_cflag |= CS8;
#ifdef CRTSCTS
        options.c_cflag &= ~CRTSCTS;
#endif
        options.c_cc[VMIN]  = 0;
        options.c_cc[VTIME] = 0;
        if (::tcsetattr(fd, TCSANOW, &options) != 0) return port_error("configure UART");
        ::tcflush(fd, TCIFLUSH);
        return true;
    }

    bool read_lines(std::vector<std::string> &lines, int wait_ms)
    {
        pollfd item{fd, POLLIN, 0};
        const auto ready = ::poll(&item, 1, wait_ms);
        if (ready < 0 && errno == EINTR) return true;
        if (ready < 0 || (item.revents & (POLLERR | POLLHUP | POLLNVAL))) return false;
        if (ready == 0) return true;
        char bytes[2048];
        const auto count = ::read(fd, bytes, sizeof(bytes));
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) return true;
        if (count <= 0) return false;
        input.append(bytes, static_cast<std::size_t>(count));
        for (;;) {
            const auto end = input.find('\n');
            if (end == std::string::npos) break;
            if (end > 4096) return false;
            auto line = input.substr(0, end);
            input.erase(0, end + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            process_line(line);
            lines.push_back(std::move(line));
        }
        return input.size() <= 4096;
    }

    bool exchange(const std::string &request, std::vector<std::string> &response, bool &rejected,
                  Clock::time_point deadline, bool cleanup)
    {
        response.clear();
        rejected         = false;
        const auto bytes = request + "\r\n";
        std::size_t sent = 0;
        while ((cleanup || !stopping) && Clock::now() < deadline && sent < bytes.size()) {
            pollfd item{fd, POLLOUT, 0};
            const auto ready = ::poll(&item, 1, 25);
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0 || (item.revents & (POLLERR | POLLHUP | POLLNVAL))) return false;
            if (ready == 0) continue;
            const auto count = ::write(fd, bytes.data() + sent, bytes.size() - sent);
            if (count < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (count <= 0) return false;
            sent += static_cast<std::size_t>(count);
        }
        if (sent != bytes.size()) return false;
        std::size_t total = 0;
        while ((cleanup || !stopping) && Clock::now() < deadline) {
            std::vector<std::string> lines;
            if (!read_lines(lines, 25)) return false;
            bool complete = false;
            for (const auto &line : lines) {
                total += line.size();
                if (total > 32768) return false;
                if (line == "ERROR" || line.rfind("+CME ERROR:", 0) == 0 || line.rfind("+CMS ERROR:", 0) == 0)
                    rejected = true;
                if (line == "OK") complete = true;
                response.push_back(line);
            }
            if (rejected) return false;
            if (complete) return true;
        }
        return false;
    }

    bool command(const std::string &request, std::vector<std::string> &response, bool &rejected,
                 Clock::time_point deadline = Clock::now() + 3s, bool cleanup = false)
    {
        if (debug) log("> " + request);
        const bool success = exchange(request, response, rejected, deadline, cleanup);
        if (!success && !cleanup && !stopping)
            log(request + (rejected ? " rejected by modem" : " failed: no complete response (timeout or UART error)"));
        return success;
    }

    bool command(const std::string &request)
    {
        std::vector<std::string> response;
        bool rejected = false;
        return command(request, response, rejected);
    }

    bool enable_nmea()
    {
        std::vector<std::string> response;
        bool rejected = false;
        bool was_disabled = false;
        if (command("AT#XNMEA?", response, rejected)) {
            const auto nmea = response_fields(response, "#XNMEA:");
            if (nmea.size() == 1 && nmea[0] == 1) {
                nmea_enabled = true;
                return true;
            }
            was_disabled = nmea.size() == 1 && nmea[0] == 0;
        } else if (!rejected) {
            return false;
        }
        // Some firmware exposes the set command without a readable state.
        // Only restore output to off when we observed that it was off before.
        own_nmea = own_nmea || was_disabled;
        nmea_enabled = command("AT#XNMEA=1", response, rejected);
        if (!nmea_enabled && rejected) {
            if (was_disabled) own_nmea = false;
            log("NMEA output unavailable; waiting for position notifications");
        }
        return nmea_enabled || rejected;
    }

    bool configure()
    {
        navigation_started_at = Clock::now();
        std::vector<std::string> response;
        bool rejected = false;
        if (!command("AT") || !command("AT%XSYSTEMMODE?", response, rejected)) return false;
        auto modes = response_fields(response, "%XSYSTEMMODE:");
        if (modes.size() < 4 || modes.size() > 5 || modes[0] > 1 || modes[1] > 1 || modes[2] > 1 || modes[3] > 4) {
            log("unsupported %XSYSTEMMODE response");
            return false;
        }
        if (!command("AT#XGNSS?", response, rejected)) return false;
        const auto state = response_fields(response, "#XGNSS:");
        if (state.size() != 2 || state[0] > 1 || state[1] > 4) {
            log("unsupported #XGNSS status response");
            return false;
        }
        // Read response is <service>,<status>; service=1 alone does not mean started.
        if (state[0] == 1 && state[1] != 0) {
            recover_sleeping = state[1] == 3 || state[1] == 4;
            attached_at      = Clock::now();
            return enable_nmea();
        }
        if (modes[2] == 0) {
            if (!command("AT+CFUN?", response, rejected)) return false;
            const auto function = response_fields(response, "+CFUN:");
            // XSYSTEMMODE can only change while inactive. Never take LTE offline for a map fix.
            if (function.size() != 1 || (function[0] != 0 && function[0] != 4)) {
                log("GNSS disabled in %XSYSTEMMODE; enable it in CFUN=0/4 before starting Maps (LTE kept active)");
                return false;
            }
            modes[2]            = 1;
            std::string request = "AT%XSYSTEMMODE=";
            for (std::size_t index = 0; index < modes.size(); ++index)
                request += (index ? "," : "") + std::to_string(modes[index]);
            if (!command(request)) return false;
        }
        if (state[0] == 0) {
            // CFUN=31 activates only GNSS, preserving whether LTE is powered on or off.
            own_activation = true;
            if (!command("AT+CFUN=31")) return false;
        }
        if (!enable_nmea()) return false;
        return start_navigation();
    }

    bool start_navigation()
    {
        std::vector<std::string> response;
        bool rejected = false;
        single_fix = rearm = false;
        own_session   = true;
        navigation_started_at = Clock::now();
        received_navigation = false;
        if (command("AT#XGNSS=1,0,1", response, rejected)) return true;
        if (!rejected) return false;
        // Some deployed SLM variants only accept the four-parameter single-fix form.
        // Re-arm on its sleep notification instead of treating one fix as a live stream.
        single_fix = true;
        return command("AT#XGNSS=1,0,0,0");
    }

    void cleanup()
    {
        if (fd < 0) return;
        std::vector<std::string> response;
        bool rejected = false;
        if (own_session) command("AT#XGNSS=0", response, rejected, Clock::now() + 250ms, true);
        if (own_nmea) command("AT#XNMEA=0", response, rejected, Clock::now() + 250ms, true);
        if (own_activation) command("AT+CFUN=30", response, rejected, Clock::now() + 250ms, true);
    }

    void run()
    {
        while (!stopping) {
            if (open_port() && configure()) {
                log("GNSS ready; waiting for a satellite fix");
                while (!stopping) {
                    std::vector<std::string> lines;
                    if (!read_lines(lines, 50)) break;
                    // A departed Cellular app can leave a completed single-fix session asleep.
                    // Preserve sessions that produce a fix or wake; recover only an initially
                    // sleeping session that remains silent for a full freshness window.
                    const bool silent_output = nmea_enabled && !received_navigation &&
                                               Clock::now() - navigation_started_at > 15s;
                    if (silent_output ||
                        (recover_sleeping && Clock::now() - attached_at > GnssService::fix_max_age)) {
                        log(silent_output ? "no NMEA or position output for 15 seconds; restarting GNSS"
                                          : "previous GNSS session still asleep; restarting GNSS");
                        recover_sleeping = false;
                        own_session      = true;
                        if (!command("AT#XGNSS=0") || !enable_nmea() || !start_navigation()) break;
                    }
                    if (single_fix && rearm && Clock::now() >= rearm_at) {
                        rearm = false;
                        // Stop resets the completed single-fix session before another start.
                        // This intentional stop does not make a recent position less accurate;
                        // retain its original timestamp so the normal freshness limit still applies.
                        preserve_rearm_fix   = true;
                        const bool restarted = command("AT#XGNSS=0") && command("AT#XGNSS=1,0,0,0");
                        preserve_rearm_fix   = false;
                        if (!restarted) break;
                    }
                }
            }
            cleanup();
            close_port();
            std::unique_lock<std::mutex> guard(mutex);
            wake.wait_for(guard, 5s, [this] { return stopping.load(); });
        }
    }
};

GnssService::GnssService() : impl_(std::make_unique<Impl>())
{
}
GnssService::~GnssService() = default;

void GnssService::start()
{
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (impl_->worker.joinable()) return;
    if (const auto *disabled = std::getenv("LILYGO_MAPS_DISABLE_GNSS"); disabled && std::string(disabled) == "1")
        return;
    impl_->port   = configured_port();
    const auto *debug = std::getenv("LILYGO_MAPS_GNSS_DEBUG");
    impl_->debug = debug && std::string(debug) == "1";
    impl_->worker = std::thread([this] { impl_->run(); });
}

std::optional<GnssFix> GnssService::current_fix() const
{
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (!impl_->fix || Clock::now() - impl_->fix->received_at > fix_max_age) return std::nullopt;
    return impl_->fix;
}
