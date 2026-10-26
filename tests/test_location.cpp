#include "services/map_service.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

const std::array<std::string, 3> kUrls{
    "https://ipapi.co/json/", "https://ipinfo.io/json", "http://ip-api.com/json/"};
const std::array<std::string, 3> kValidResponses{
    R"({"latitude":31.2304,"longitude":121.4737,"city":"Shanghai","country_name":"China"})",
    R"({"loc":"51.5074,-0.1278","city":"London","region":"England","country":"GB"})",
    R"({"status":"success","lat":48.8566,"lon":2.3522,"city":"Paris","regionName":"Ile-de-France","country":"France"})"};

void write_file(const std::filesystem::path &path, const std::string &contents)
{
    std::ofstream stream(path);
    stream << contents;
    assert(stream.good());
}

std::string read_file(const std::filesystem::path &path)
{
    std::ifstream stream(path);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void wait_until(const std::function<bool()> &condition)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!condition()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// The test binary doubles as curl, exercising the real process and HTTP policy code.
int fake_curl(int argc, char **argv)
{
    const char *directory = std::getenv("LILYGO_MAPS_TEST_HTTP_ROOT");
    assert(directory && argc > 1);
    const std::filesystem::path root(directory);
    std::string arguments;
    for (int index = 1; index < argc; ++index) {
        if (index > 1) arguments += '\t';
        arguments += argv[index];
    }
    arguments += '\n';
    const int log = ::open((root / "requests").c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    assert(log >= 0);
    assert(::write(log, arguments.data(), arguments.size()) == static_cast<ssize_t>(arguments.size()));
    ::close(log);

    const std::string url = argv[argc - 1];
    const auto provider = std::find(kUrls.begin(), kUrls.end(), url);
    assert(provider != kUrls.end());
    const auto index = std::to_string(std::distance(kUrls.begin(), provider));
    if (index == "0" && std::filesystem::exists(root / "block-first")) {
        const int claim = ::open((root / "blocked").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (claim >= 0) {
            ::close(claim);
            wait_until([&] { return std::filesystem::exists(root / "release"); });
            write_file(root / "finished", "");
            return 22;
        }
    }
    if (std::filesystem::exists(root / (index + ".error"))) {
        const auto error = read_file(root / (index + ".error"));
        assert(::write(STDERR_FILENO, error.data(), error.size()) == static_cast<ssize_t>(error.size()));
        return 22;
    }
    const auto body = read_file(root / (index + ".json"));
    assert(::write(STDOUT_FILENO, body.data(), body.size()) == static_cast<ssize_t>(body.size()));
    return 0;
}

class Fixture {
public:
    explicit Fixture(const char *executable)
    {
        std::string pattern = (std::filesystem::temp_directory_path() / "lilygo-maps-location-XXXXXX").string();
        assert(::mkdtemp(pattern.data()));
        root = pattern;
        std::filesystem::create_symlink(std::filesystem::canonical(executable), root / "curl");
        const char *path = std::getenv("PATH");
        old_path_ = path ? path : "";
        assert(::setenv("PATH", (root.string() + ":" + old_path_).c_str(), 1) == 0);
        assert(::setenv("LILYGO_MAPS_TEST_HTTP_ROOT", root.c_str(), 1) == 0);
        reset();
    }

    ~Fixture()
    {
        ::setenv("PATH", old_path_.c_str(), 1);
        ::unsetenv("LILYGO_MAPS_TEST_HTTP_ROOT");
        ::unsetenv("LILYGO_MAPS_DISABLE_NETWORK");
        std::filesystem::remove_all(root);
    }

    void reset()
    {
        ::unsetenv("LILYGO_MAPS_DISABLE_NETWORK");
        for (const auto &entry : std::filesystem::directory_iterator(root)) {
            if (entry.path().filename() != "curl") std::filesystem::remove_all(entry.path());
        }
        for (std::size_t index = 0; index < kUrls.size(); ++index) response(index, kValidResponses[index]);
    }

    void response(std::size_t provider, const std::string &body)
    {
        write_file(root / (std::to_string(provider) + ".json"), body);
    }

    void fail(std::size_t provider)
    {
        write_file(root / (std::to_string(provider) + ".error"), "fixture request failed");
    }

    std::vector<std::vector<std::string>> requests() const
    {
        std::vector<std::vector<std::string>> result;
        std::istringstream log(read_file(root / "requests"));
        std::string line;
        while (std::getline(log, line)) {
            std::istringstream arguments(line);
            std::vector<std::string> request;
            std::string argument;
            while (std::getline(arguments, argument, '\t')) request.push_back(argument);
            result.push_back(std::move(request));
        }
        return result;
    }

    void expect_order(std::initializer_list<std::size_t> providers) const
    {
        const auto recorded = requests();
        assert(recorded.size() == providers.size());
        std::size_t index = 0;
        for (const auto provider : providers) assert(recorded[index++].back() == kUrls[provider]);
    }

    std::filesystem::path root;

private:
    std::string old_path_;
};

bool is_location(const MapServiceEvent &event)
{
    return event.kind == MapServiceEventKind::LocationReady || event.kind == MapServiceEventKind::LocationError;
}

MapServiceEvent await_location(MapService &service, std::uint64_t request)
{
    MapServiceEvent result;
    wait_until([&] {
        MapServiceEvent event;
        while (service.poll(event)) {
            if (!is_location(event)) continue;
            assert(event.request_id == request);
            result = std::move(event);
            return true;
        }
        return false;
    });
    return result;
}

MapServiceEvent locate(const Fixture &fixture)
{
    MapService service(fixture.root / "openstreetmap", [] { return std::optional<GnssFix>{}; });
    auto event = await_location(service, service.locate_by_ip());
    MapServiceEvent extra;
    while (service.poll(extra)) assert(!is_location(extra));
    return event;
}

void expect_point(const MapServiceEvent &event, double latitude, double longitude)
{
    assert(event.kind == MapServiceEventKind::LocationReady);
    assert(event.error.empty());
    assert(std::abs(event.point.latitude - latitude) < 0.000001);
    assert(std::abs(event.point.longitude - longitude) < 0.000001);
}

void test_provider_order(Fixture &fixture)
{
    fixture.reset();
    auto event = locate(fixture);
    expect_point(event, 31.2304, 121.4737);
    assert(event.label.find("China") != std::string::npos);
    fixture.expect_order({0});

    fixture.reset();
    fixture.fail(0);
    event = locate(fixture);
    expect_point(event, 51.5074, -0.1278);
    assert(event.label.find("GB") != std::string::npos);
    fixture.expect_order({0, 1});

    fixture.reset();
    fixture.fail(0);
    fixture.fail(1);
    event = locate(fixture);
    expect_point(event, 48.8566, 2.3522);
    assert(event.label.find("Ile-de-France") != std::string::npos);
    assert(event.label.find("France") != std::string::npos);
    fixture.expect_order({0, 1, 2});

    fixture.reset();
    for (std::size_t index = 0; index < kUrls.size(); ++index) fixture.fail(index);
    event = locate(fixture);
    assert(event.kind == MapServiceEventKind::LocationError);
    for (const auto *provider : {"ipapi", "ipinfo", "ip-api"}) assert(event.error.find(provider) != std::string::npos);
    assert(event.error.find("fixture request failed") != std::string::npos);
    fixture.expect_order({0, 1, 2});
}

void test_invalid_responses(Fixture &fixture)
{
    for (const auto *body : {"invalid JSON", "[]", "{}",
                            R"({"latitude":1,"longitude":2} trailing garbage)",
                            R"({"error":true,"latitude":1,"longitude":2})",
                            R"({"latitude":91,"longitude":2})",
                            R"({"latitude":1,"longitude":181})",
                            R"({"latitude":null,"longitude":2})",
                            R"({"latitude":"NaN","longitude":2})"}) {
        fixture.reset();
        fixture.response(0, body);
        expect_point(locate(fixture), 51.5074, -0.1278);
        fixture.expect_order({0, 1});
    }

    for (const auto *body : {R"({"bogon":true,"loc":"1,2"})", R"({"error":{"message":"limit"},"loc":"1,2"})",
                            R"({"loc":"1,2,3"})", R"({"loc":"1,2junk"})", R"({"loc":"1,"})",
                            R"({"loc":",2"})", R"({"loc":"NaN,2"})", R"({"loc":"1,Infinity"})",
                            R"({"loc":"-91,2"})", R"({"loc":"1,-181"})", R"({"loc":[1,2]})"}) {
        fixture.reset();
        fixture.fail(0);
        fixture.response(1, body);
        expect_point(locate(fixture), 48.8566, 2.3522);
        fixture.expect_order({0, 1, 2});
    }

    for (const auto *body : {R"({"status":"fail","message":"private range","lat":1,"lon":2})",
                            R"({"lat":1,"lon":2})", R"({"status":"success","lat":1,"lon":181})",
                            R"({"status":"success","lat":1})"}) {
        fixture.reset();
        fixture.fail(0);
        fixture.fail(1);
        fixture.response(2, body);
        assert(locate(fixture).kind == MapServiceEventKind::LocationError);
        fixture.expect_order({0, 1, 2});
    }

    fixture.reset();
    fixture.response(0, R"({"latitude":"0","longitude":"0","error":false})");
    expect_point(locate(fixture), 0, 0);
    fixture.expect_order({0});
}

void test_network_disabled(Fixture &fixture)
{
    fixture.reset();
    ::setenv("LILYGO_MAPS_DISABLE_NETWORK", "1", 1);
    assert(locate(fixture).kind == MapServiceEventKind::LocationError);
    fixture.expect_order({});
}

std::string option_value(const std::vector<std::string> &arguments, const std::string &option)
{
    const auto found = std::find(arguments.begin(), arguments.end(), option);
    assert(found != arguments.end() && std::next(found) != arguments.end());
    return *std::next(found);
}

void test_http_policy(Fixture &fixture)
{
    fixture.reset();
    HttpClient http;
    for (const auto &url : kUrls) assert(http.get(url, "application/json", 4096U));
    const auto requests = fixture.requests();
    assert(requests.size() == 3U);
    for (std::size_t index = 0; index < requests.size(); ++index) {
        const auto &arguments = requests[index];
        assert(option_value(arguments, "--proto") == (index == 2 ? "=http" : "=https"));
        const bool redirects = std::find(arguments.begin(), arguments.end(), "--location") != arguments.end();
        assert(redirects == (index != 2));
        if (index != 2) assert(option_value(arguments, "--proto-redir") == "=https");
        else assert(std::find(arguments.begin(), arguments.end(), "--no-location") != arguments.end());
    }
    for (const auto *url : {"http://ip-api.com/json", "http://ip-api.com/json/?fields=lat,lon",
                           "http://ip-api.com/json/extra", "http://ip-api.com.evil/json/",
                           "http://ip-api.com@evil/json/", "http://example.com/", "file:///tmp/location.json"}) {
        assert(!http.get(url, "application/json", 4096U));
    }
    fixture.expect_order({0, 1, 2});
}

void test_cancelled_and_stale_requests(Fixture &fixture)
{
    fixture.reset();
    write_file(fixture.root / "block-first", "");
    {
        MapService service(fixture.root / "openstreetmap", [] { return std::optional<GnssFix>{}; });
        service.locate_by_ip();
        wait_until([&] { return std::filesystem::exists(fixture.root / "blocked"); });
    }
    fixture.expect_order({0});

    fixture.reset();
    write_file(fixture.root / "block-first", "");
    {
        MapService service(fixture.root / "openstreetmap", [] { return std::optional<GnssFix>{}; });
        const auto stale = service.locate_by_ip();
        wait_until([&] { return std::filesystem::exists(fixture.root / "blocked"); });
        const auto current = service.locate_by_ip();
        assert(current != stale);
        expect_point(await_location(service, current), 31.2304, 121.4737);
        write_file(fixture.root / "release", "");
        wait_until([&] { return std::filesystem::exists(fixture.root / "finished"); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        MapServiceEvent event;
        while (service.poll(event)) assert(!is_location(event));
    }
    fixture.expect_order({0, 0});
}

void drain(MapService &service)
{
    MapServiceEvent event;
    while (service.poll(event)) assert(!is_location(event));
}

void test_gnss_priority_and_expiry(Fixture &fixture)
{
    fixture.reset();
    std::optional<GnssFix> fix;
    MapService service(fixture.root / "openstreetmap", [&] { return fix; });
    const auto request = service.locate();
    auto event = await_location(service, request);
    expect_point(event, 31.2304, 121.4737);
    assert(event.location_source == LocationSource::Ip);

    fix = GnssFix{{22.5431, 114.0579}, 1, std::chrono::steady_clock::now()};
    event = await_location(service, request);
    expect_point(event, 22.5431, 114.0579);
    assert(event.location_source == LocationSource::Gnss);
    drain(service);

    fix->received_at -= GnssService::fix_max_age + std::chrono::seconds(1);
    event = await_location(service, request);
    expect_point(event, 31.2304, 121.4737);
    assert(event.location_source == LocationSource::Ip);
    assert(!service.current_gnss_fix());
    drain(service);

    fix = GnssFix{{0, 0}, 2, std::chrono::steady_clock::now()};
    event = await_location(service, request);
    expect_point(event, 0, 0);
    assert(event.location_source == LocationSource::Gnss);
    fix.reset();
    assert(await_location(service, request).location_source == LocationSource::Ip);
    fixture.expect_order({0});
}

void test_late_ip_cannot_override_gnss(Fixture &fixture)
{
    fixture.reset();
    write_file(fixture.root / "block-first", "");
    std::optional<GnssFix> fix;
    MapService service(fixture.root / "openstreetmap", [&] { return fix; });
    const auto request = service.locate();
    wait_until([&] { return std::filesystem::exists(fixture.root / "blocked"); });
    fix = GnssFix{{22.5431, 114.0579}, 1, std::chrono::steady_clock::now()};
    assert(await_location(service, request).location_source == LocationSource::Gnss);
    write_file(fixture.root / "release", "");
    wait_until([&] {
        drain(service);
        return service.cached_ip_location().has_value();
    });
    fix.reset();
    const auto event = await_location(service, request);
    expect_point(event, 51.5074, -0.1278);
    assert(event.location_source == LocationSource::Ip);
    fixture.expect_order({0, 1});
}

void test_offline_gnss_and_invalid_fixes(Fixture &fixture)
{
    fixture.reset();
    ::setenv("LILYGO_MAPS_DISABLE_NETWORK", "1", 1);
    std::optional<GnssFix> fix = GnssFix{{22.5431, 114.0579}, 1, std::chrono::steady_clock::now()};
    MapService service(fixture.root / "openstreetmap", [&] { return fix; });
    auto request = service.locate();
    assert(await_location(service, request).location_source == LocationSource::Gnss);
    request = service.locate();
    assert(await_location(service, request).location_source == LocationSource::Gnss);
    // A missing/invalid fix allows the pending network-disabled result through.
    fix->point.latitude = 91;
    assert(!service.current_gnss_fix());
    assert(await_location(service, request).kind == MapServiceEventKind::LocationError);
    fix->point.latitude = 22.5431;
    fix->received_at = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    assert(!service.current_gnss_fix());
    fixture.expect_order({});
}

}  // namespace

int main(int argc, char **argv)
{
    if (std::filesystem::path(argv[0]).filename() == "curl") return fake_curl(argc, argv);
    Fixture fixture(argv[0]);
    test_provider_order(fixture);
    test_invalid_responses(fixture);
    test_network_disabled(fixture);
    test_http_policy(fixture);
    test_cancelled_and_stale_requests(fixture);
    test_gnss_priority_and_expiry(fixture);
    test_late_ip_cannot_override_gnss(fixture);
    test_offline_gnss_and_invalid_fixes(fixture);
    return 0;
}
