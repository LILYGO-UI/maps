#include "services/gnss_protocol.hpp"
#include "services/gnss_service.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <functional>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/file.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using namespace std::chrono_literals;
using Kind = gnss_protocol::PositionKind;

std::string nmea(const std::string &body)
{
    unsigned checksum = 0;
    for (unsigned char ch : body) checksum ^= ch;
    char hex[4];
    std::snprintf(hex, sizeof(hex), "%02X", checksum);
    return "$" + body + "*" + hex;
}

void wait_until(const std::function<bool()> &condition, std::chrono::milliseconds timeout = 3s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(2ms);
    }
}

class Environment {
public:
    Environment(const char *name, const std::string &value) : name_(name)
    {
        if (const auto *old = std::getenv(name)) previous_ = old;
        ::setenv(name, value.c_str(), 1);
    }
    ~Environment()
    {
        if (previous_)
            ::setenv(name_.c_str(), previous_->c_str(), 1);
        else
            ::unsetenv(name_.c_str());
    }

private:
    std::string name_;
    std::optional<std::string> previous_;
};

class Modem {
public:
    std::string path;
    std::string system_mode = "0,1,1,2";
    std::string gnss_state  = "1,0";
    int function_mode       = 1;
    int nmea_enabled        = 0;
    bool reject_continuous  = false;
    bool reject_nmea_query  = false;
    bool reject_nmea_set    = false;
    bool ignore_commands    = false;
    bool ignore_stop_reply  = false;

    Modem()
    {
        master_ = ::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
        assert(master_ >= 0);
        assert(::grantpt(master_) == 0 && ::unlockpt(master_) == 0);
        path   = ::ptsname(master_);
        slave_ = ::open(path.c_str(), O_RDWR | O_NOCTTY);
        assert(slave_ >= 0);
    }
    ~Modem()
    {
        disconnect();
    }

    void start()
    {
        worker_ = std::thread([this] {
            std::string input;
            while (!stopping_) {
                pollfd item{master_, POLLIN, 0};
                if (::poll(&item, 1, 20) <= 0) continue;
                char bytes[1024];
                const auto count = ::read(master_, bytes, sizeof(bytes));
                if (count <= 0) continue;
                input.append(bytes, static_cast<std::size_t>(count));
                std::size_t end;
                while ((end = input.find('\n')) != std::string::npos) {
                    auto request = input.substr(0, end);
                    input.erase(0, end + 1);
                    if (!request.empty() && request.back() == '\r') request.pop_back();
                    {
                        std::lock_guard<std::mutex> guard(mutex_);
                        requests_.push_back(request);
                    }
                    if (ignore_commands) continue;
                    if (request == "AT#XGNSS=0" && ignore_stop_reply) continue;
                    std::string reply;
                    if (request == "AT%XSYSTEMMODE?") reply = "%XSYSTEMMODE: " + system_mode;
                    if (request == "AT#XGNSS?") reply = "#XGNSS: " + gnss_state;
                    if (request == "AT+CFUN?") reply = "+CFUN: " + std::to_string(function_mode);
                    if ((request == "AT#XNMEA?" && reject_nmea_query) ||
                        (request.rfind("AT#XNMEA=", 0) == 0 && reject_nmea_set)) {
                        send("ERROR\r\n");
                        continue;
                    }
                    if (request == "AT#XNMEA?") reply = "#XNMEA: " + std::to_string(nmea_enabled);
                    if (request == "AT#XNMEA=1") nmea_enabled = 1;
                    if (request == "AT#XNMEA=0") nmea_enabled = 0;
                    if (request == "AT#XGNSS=1,0,1" && reject_continuous) {
                        send("ERROR\r\n");
                        continue;
                    }
                    if (request.rfind("AT#XGNSS=1,", 0) == 0) {
                        gnss_state = "1,1";
                        reply      = "#XGNSS: " + gnss_state;
                    }
                    if (request == "AT#XGNSS=0") {
                        gnss_state = "1,0";
                        reply      = "#XGNSS: " + gnss_state;
                    }
                    send(reply + "\r\nOK\r\n");
                }
            }
        });
    }

    void disconnect()
    {
        stopping_ = true;
        if (worker_.joinable()) worker_.join();
        if (master_ >= 0) ::close(master_);
        if (slave_ >= 0) ::close(slave_);
        master_ = slave_ = -1;
    }

    void send(const std::string &bytes)
    {
        std::lock_guard<std::mutex> guard(output_mutex_);
        std::size_t sent    = 0;
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (sent < bytes.size()) {
            const auto count = ::write(master_, bytes.data() + sent, bytes.size() - sent);
            if (count > 0)
                sent += static_cast<std::size_t>(count);
            else {
                assert(std::chrono::steady_clock::now() < deadline);
                std::this_thread::sleep_for(1ms);
            }
        }
    }

    std::vector<std::string> requests()
    {
        std::lock_guard<std::mutex> guard(mutex_);
        return requests_;
    }

    int count(const std::string &request)
    {
        const auto values = requests();
        return static_cast<int>(std::count(values.begin(), values.end(), request));
    }

private:
    int master_ = -1;
    int slave_  = -1;
    std::atomic<bool> stopping_{false};
    std::mutex mutex_;
    std::mutex output_mutex_;
    std::vector<std::string> requests_;
    std::thread worker_;
};

const std::string position = "#XGNSSPOS: 22.5431,114.0579,18.6,2,0,0,\"2026-10-16T09:41:00\"\r\n";

void test_protocol()
{
    const auto valid  = nmea("GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,");
    const auto parsed = gnss_protocol::parse_position("#XGNSSNMEA: " + valid + "\r\n");
    assert(parsed.kind == Kind::Fix && std::abs(parsed.point.latitude - 48.1173) < 0.00001);
    assert(std::abs(parsed.point.longitude - 11.5166667) < 0.00001);
    assert(gnss_protocol::parse_position(valid.substr(0, valid.size() - 2) + "00").kind == Kind::Ignored);
    assert(gnss_protocol::parse_position(valid.substr(0, valid.size() - 3)).kind == Kind::Ignored);
    assert(gnss_protocol::parse_position(nmea("GNGGA,123519,,,,,0,00,,,,,,,")).kind == Kind::Invalid);
    assert(gnss_protocol::parse_position(nmea("GNGGA,123519,4860.001,N,01131.000,E,1,08,1,0,M,,M,,")).kind ==
           Kind::Invalid);
    assert(gnss_protocol::parse_position(nmea("GNGGA,123519,9100.000,N,01131.000,E,1,08,1,0,M,,M,,")).kind ==
           Kind::Invalid);
    assert(gnss_protocol::parse_position(nmea("GNGGA,123519,4807.038,E,01131.000,N,1,08,1,0,M,,M,,")).kind ==
           Kind::Invalid);
    assert(gnss_protocol::parse_position(nmea("GNGGA,123519,4807.038,N,01131.000,E,6,08,1,0,M,,M,,")).kind ==
           Kind::Invalid);
    const auto south = gnss_protocol::parse_position(nmea("GNRMC,123519,A,4807.038,S,01131.000,W,0,0,161026,,,A"));
    assert(south.kind == Kind::Fix && south.point.latitude < 0 && south.point.longitude < 0);
    assert(gnss_protocol::parse_position(nmea("GNRMC,123519,V,,,,,,,161026,,,N")).kind == Kind::Invalid);
    assert(gnss_protocol::parse_position(nmea("GNRMC,123519,A,4807.038,N,01131.000,E,0,0,161026,,,E")).kind ==
           Kind::Invalid);
    assert(gnss_protocol::parse_position(nmea("GPGSV,1,1,01,03,64,32,42")).kind == Kind::Ignored);
    assert(gnss_protocol::parse_position(position).kind == Kind::Fix);
    for (const auto *bad :
         {"#XGNSSPOS: nan,1", "#XGNSSPOS: 1,inf", "#XGNSSPOS: 91,1", "#XGNSSPOS: 1,181", "#XGNSSPOS: 1x,1"})
        assert(gnss_protocol::parse_position(bad).kind == Kind::Ignored);
    const auto origin = gnss_protocol::parse_position("#XGNSSPOS: 0,0");
    assert(origin.kind == Kind::Fix && origin.point.latitude == 0 && origin.point.longitude == 0);
    assert(gnss_protocol::integer_response("#XGNSS: 1,0", "#XGNSS:") == (std::vector<int>{1, 0}));
}

void test_session_and_expiry()
{
    Modem modem;
    modem.start();
    Environment configured("LILYGO_MAPS_GNSS_PORT", modem.path);
    {
        GnssService service;
        assert(!service.current_fix());
        std::this_thread::sleep_for(25ms);
        assert(modem.requests().empty());  // Construction alone never touches hardware.
        service.start();
        service.start();
        wait_until([&] { return modem.count("AT#XGNSS=1,0,1") == 1; });
        for (char ch : position) modem.send(std::string(1, ch));
        wait_until([&] { return service.current_fix().has_value(); });
        const auto first = *service.current_fix();
        assert(std::abs(first.point.latitude - 22.5431) < 0.00001);
        modem.send(nmea("GPGGA,123519,,,,,0,00,,,,,,,") + "\r\n");
        wait_until([&] { return !service.current_fix(); });
        modem.send(position);
        wait_until([&] { return service.current_fix().has_value(); });
        assert(service.current_fix()->sequence > first.sequence);
        const auto received = service.current_fix()->received_at;
        std::this_thread::sleep_until(received + GnssService::fix_max_age + 50ms);
        assert(!service.current_fix());
        modem.send(position);
        wait_until([&] { return service.current_fix().has_value(); });
        modem.send("#XGNSS: 1,0\r\n");
        wait_until([&] { return !service.current_fix(); });
    }
    assert(modem.count("AT#XGNSS=0") == 1);
    assert(modem.count("AT#XNMEA=0") == 1);
    for (const auto &request : modem.requests()) {
        assert(request != "AT+CFUN=1" && request != "AT+CFUN=0");
        assert(request.rfind("AT%XSYSTEMMODE=", 0) != 0);
    }
}

void test_existing_session_and_disconnect()
{
    Modem modem;
    modem.gnss_state   = "1,1";
    modem.nmea_enabled = 1;
    modem.start();
    Environment configured("LILYGO_MAPS_GNSS_PORT", modem.path);
    {
        GnssService service;
        service.start();
        wait_until([&] { return modem.count("AT#XNMEA?") == 1; });
        modem.send(position);
        wait_until([&] { return service.current_fix().has_value(); });
    }
    assert(modem.count("AT#XGNSS=0") == 0 && modem.count("AT#XNMEA=0") == 0);
    GnssService service;
    service.start();
    wait_until([&] { return modem.count("AT#XNMEA?") == 2; });
    modem.send(position);
    wait_until([&] { return service.current_fix().has_value(); });
    modem.disconnect();
    wait_until([&] { return !service.current_fix(); });
}

void test_existing_session_enables_nmea()
{
    Modem modem;
    modem.gnss_state = "1,1";
    modem.start();
    Environment configured("LILYGO_MAPS_GNSS_PORT", modem.path);
    {
        GnssService service;
        service.start();
        wait_until([&] { return modem.count("AT#XNMEA=1") == 1; });
        modem.send(nmea("GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,") + "\r\n");
        wait_until([&] { return service.current_fix().has_value(); });
        assert(modem.count("AT#XNMEA?") == 1);
        assert(modem.count("AT#XGNSS=1,0,1") == 0);
    }
    assert(modem.count("AT#XGNSS=0") == 0);
    assert(modem.count("AT#XNMEA=0") == 1);
}

void test_optional_nmea_commands()
{
    for (const bool reject_set : {false, true}) {
        Modem modem;
        modem.reject_nmea_query = true;
        modem.reject_nmea_set   = reject_set;
        modem.start();
        Environment configured("LILYGO_MAPS_GNSS_PORT", modem.path);
        GnssService service;
        service.start();
        wait_until([&] { return modem.count("AT#XGNSS=1,0,1") == 1; });
        assert(modem.count("AT#XNMEA?") == 1);
        assert(modem.count("AT#XNMEA=1") == 1);
        // A firmware without XNMEA still reports positions through XGNSSPOS.
        modem.send(position);
        wait_until([&] { return service.current_fix().has_value(); });
    }
}

void test_modes_and_legacy()
{
    Modem modem;
    modem.system_mode       = "0,1,0,2";
    modem.gnss_state        = "0,0";
    modem.function_mode     = 0;
    modem.reject_continuous = true;
    modem.start();
    Environment configured("LILYGO_MAPS_GNSS_PORT", modem.path);
    {
        GnssService service;
        service.start();
        wait_until([&] { return modem.count("AT#XGNSS=1,0,0,0") == 1; });
        assert(modem.count("AT%XSYSTEMMODE=0,1,1,2") == 1);
        assert(modem.count("AT+CFUN=31") == 1);
        modem.send(position + "#XGNSS: 1,4\r\n");
        wait_until([&] { return service.current_fix().has_value(); });
        const auto original = *service.current_fix();
        wait_until([&] { return modem.count("AT#XGNSS=1,0,0,0") == 2; });
        assert(modem.count("AT#XGNSS=0") == 1);
        std::this_thread::sleep_for(30ms);
        assert(service.current_fix() && service.current_fix()->sequence == original.sequence);
        assert(service.current_fix()->received_at == original.received_at);
        modem.send(position);
        wait_until([&] { return service.current_fix().has_value(); });
    }
    assert(modem.count("AT+CFUN=30") == 1);
    assert(modem.count("AT+CFUN=1") == 0);
}

void test_active_lte_not_reconfigured()
{
    Modem modem;
    modem.system_mode   = "0,1,0,2";
    modem.gnss_state    = "0,0";
    modem.function_mode = 1;
    modem.start();
    Environment configured("LILYGO_MAPS_GNSS_PORT", modem.path);
    GnssService service;
    service.start();
    wait_until([&] { return modem.count("AT+CFUN?") == 1; });
    std::this_thread::sleep_for(40ms);
    for (const auto &request : modem.requests()) assert(request.find('=') == std::string::npos);
}

void test_locks_and_cancel()
{
    Modem modem;
    modem.ignore_commands = true;
    modem.start();
    Environment configured("LILYGO_MAPS_GNSS_PORT", modem.path);
    const auto lock_path = "/tmp/lilygo-cellular-" + std::to_string(std::hash<std::string>{}(modem.path)) + ".lock";
    const auto lock      = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0600);
    assert(lock >= 0 && ::flock(lock, LOCK_EX | LOCK_NB) == 0);
    {
        GnssService service;
        service.start();
        std::this_thread::sleep_for(80ms);
        assert(modem.requests().empty());
    }
    ::flock(lock, LOCK_UN);
    ::close(lock);
    const auto before = std::chrono::steady_clock::now();
    {
        GnssService service;
        service.start();
        wait_until([&] { return modem.count("AT") == 1; });
    }
    assert(std::chrono::steady_clock::now() - before < 1s);
    {
        Environment disabled("LILYGO_MAPS_DISABLE_GNSS", "1");
        GnssService service;
        service.start();
        std::this_thread::sleep_for(50ms);
        assert(modem.count("AT") == 1);
    }
}

void test_busy_retry()
{
    Modem modem;
    modem.start();
    Environment configured("LILYGO_MAPS_GNSS_PORT", modem.path);
    const auto lock_path = "/tmp/lilygo-cellular-" + std::to_string(std::hash<std::string>{}(modem.path)) + ".lock";
    const auto lock      = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0600);
    assert(lock >= 0 && ::flock(lock, LOCK_EX | LOCK_NB) == 0);
    GnssService service;
    service.start();
    std::this_thread::sleep_for(80ms);
    assert(modem.requests().empty());
    ::flock(lock, LOCK_UN);
    ::close(lock);
    wait_until([&] { return modem.count("AT#XGNSS=1,0,1") == 1; }, 6s);
    modem.send(position);
    wait_until([&] { return service.current_fix().has_value(); });
}

void test_session_recovery_and_nmea_watchdog()
{
    struct Scenario {
        Modem modem;
        std::unique_ptr<GnssService> service;

        void start()
        {
            modem.start();
            Environment configured("LILYGO_MAPS_GNSS_PORT", modem.path);
            service = std::make_unique<GnssService>();
            service->start();
        }
    };
    Scenario completed, timed_out, periodic, silent_new, silent_existing, no_fix, satellites, first_fix, unsupported;
    completed.modem.gnss_state          = "1,4";
    timed_out.modem.gnss_state          = "1,3";
    periodic.modem.gnss_state           = "1,4";
    silent_existing.modem.gnss_state    = "1,1";
    unsupported.modem.reject_nmea_query = true;
    unsupported.modem.reject_nmea_set   = true;
    const auto scenarios = {&completed, &timed_out, &periodic, &silent_new, &silent_existing,
                            &no_fix, &satellites, &first_fix, &unsupported};
    for (auto *scenario : scenarios) scenario->start();
    for (auto *scenario : scenarios)
        wait_until([&] { return scenario->modem.count("AT#XNMEA=1") == 1; });
    wait_until([&] {
        return silent_new.modem.count("AT#XGNSS=1,0,1") == 1 && no_fix.modem.count("AT#XGNSS=1,0,1") == 1 &&
               satellites.modem.count("AT#XGNSS=1,0,1") == 1 && first_fix.modem.count("AT#XGNSS=1,0,1") == 1 &&
               unsupported.modem.count("AT#XGNSS=1,0,1") == 1;
    });
    // These cases share one real 15-second recovery window to keep the suite bounded.
    periodic.modem.send("#XGNSS: 1,2\r\n" + position + "#XGNSS: 1,4\r\n");
    first_fix.modem.send(position);
    wait_until([&] { return periodic.service->current_fix() && first_fix.service->current_fix(); });
    const auto received           = first_fix.service->current_fix()->received_at;
    const auto no_fix_gga          = nmea("GNGGA,123519,,,,,0,00,,,,,,,") + "\r\n";
    const auto visible_satellites = "#XGNSSNMEA: " + nmea("GPGSV,1,1,01,03,64,32,42") + "\r\n";
    auto next_sentence = std::chrono::steady_clock::now();
    wait_until([&] {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_sentence) {
            no_fix.modem.send(no_fix_gga);
            satellites.modem.send(visible_satellites);
            next_sentence = now + 100ms;
        }
        return now > received + GnssService::fix_max_age + 100ms &&
               completed.modem.count("AT#XGNSS=1,0,1") == 1 &&
               timed_out.modem.count("AT#XGNSS=1,0,1") == 1 &&
               silent_new.modem.count("AT#XGNSS=1,0,1") == 2 &&
               silent_existing.modem.count("AT#XGNSS=1,0,1") == 1;
    }, 17s);
    for (auto *scenario : {&completed, &timed_out, &silent_new, &silent_existing}) {
        assert(scenario->modem.count("AT#XGNSS=0") == 1);
        scenario->modem.send(position);
        wait_until([&] { return scenario->service->current_fix().has_value(); });
    }
    for (auto *scenario : {&periodic, &no_fix, &satellites, &first_fix, &unsupported})
        assert(scenario->modem.count("AT#XGNSS=0") == 0);
    assert(periodic.modem.count("AT#XGNSS=1,0,1") == 0);
    assert(!no_fix.service->current_fix() && !satellites.service->current_fix());
    // Expiry alone must not turn a working session into a silent-start recovery.
    assert(!first_fix.service->current_fix());
    for (auto *scenario : {&no_fix, &satellites, &first_fix, &unsupported})
        assert(scenario->modem.count("AT#XGNSS=1,0,1") == 1);
    for (auto *scenario : scenarios) scenario->service.reset();
    for (auto *scenario : {&completed, &timed_out, &silent_new, &silent_existing})
        assert(scenario->modem.count("AT#XGNSS=0") == 2);
    assert(periodic.modem.count("AT#XGNSS=0") == 0);
}

void test_cleanup_after_stop_timeout()
{
    Modem modem;
    modem.gnss_state        = "0,0";
    modem.ignore_stop_reply = true;
    modem.start();
    Environment configured("LILYGO_MAPS_GNSS_PORT", modem.path);
    auto service = std::make_unique<GnssService>();
    service->start();
    wait_until([&] { return modem.count("AT#XGNSS=1,0,1") == 1; });
    const auto before = std::chrono::steady_clock::now();
    service.reset();
    assert(std::chrono::steady_clock::now() - before < 1s);
    assert(modem.count("AT#XGNSS=0") == 1);
    assert(modem.count("AT#XNMEA=0") == 1 && modem.count("AT+CFUN=30") == 1);
}
}  // namespace

int main()
{
    Environment enabled("LILYGO_MAPS_DISABLE_GNSS", "0");
    test_protocol();
    test_session_and_expiry();
    test_existing_session_and_disconnect();
    test_existing_session_enables_nmea();
    test_optional_nmea_commands();
    test_modes_and_legacy();
    test_active_lte_not_reconfigured();
    test_locks_and_cancel();
    test_busy_retry();
    test_session_recovery_and_nmea_watchdog();
    test_cleanup_after_stop_timeout();
}
