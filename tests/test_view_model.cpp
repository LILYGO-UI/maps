#include "pages/map/map_view_model.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr GeoPoint kIpLocation{31.2304, 121.4737};
constexpr GeoPoint kSearchLocation{48.8566, 2.3522};
constexpr GeoPoint kGnssLocation{22.5431, 114.0579};
constexpr GeoPoint kNextGnssLocation{22.5432, 114.0580};

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

template <typename Condition>
void wait_until(Condition condition)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!condition()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// The test executable also serves deterministic, optionally delayed curl responses.
int fake_curl(int argc, char **argv)
{
    const char *directory = std::getenv("LILYGO_MAPS_TEST_HTTP_ROOT");
    assert(directory && argc > 1);
    const std::filesystem::path root(directory);
    const std::string url = argv[argc - 1];
    const bool search = url.find("https://nominatim.openstreetmap.org/search?") == 0U;
    assert(search || url == "https://ipapi.co/json/" || url == "https://ipinfo.io/json" ||
           url == "http://ip-api.com/json/");
    const std::string kind = search ? "search" : "location";
    const auto line = url + '\n';
    const int log = ::open((root / "requests").c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    assert(log >= 0);
    assert(::write(log, line.data(), line.size()) == static_cast<ssize_t>(line.size()));
    ::close(log);

    if (std::filesystem::exists(root / (kind + ".block"))) {
        const int claim = ::open((root / (kind + ".started")).c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (claim >= 0) {
            ::close(claim);
            wait_until([&] { return std::filesystem::exists(root / (kind + ".release")); });
        }
    }
    if (std::filesystem::exists(root / (kind + ".error"))) return 22;
    const auto body = read_file(root / (kind + ".json"));
    assert(::write(STDOUT_FILENO, body.data(), body.size()) == static_cast<ssize_t>(body.size()));
    return 0;
}

class Fixture {
public:
    explicit Fixture(const char *executable)
    {
        std::string pattern = (std::filesystem::temp_directory_path() / "lilygo-maps-view-model-XXXXXX").string();
        assert(::mkdtemp(pattern.data()));
        root = pattern;
        std::filesystem::create_symlink(std::filesystem::canonical(executable), root / "curl");
        const char *path = std::getenv("PATH");
        old_path_ = path ? path : "";
        assert(::setenv("PATH", (root.string() + ":" + old_path_).c_str(), 1) == 0);
        assert(::setenv("LILYGO_MAPS_TEST_HTTP_ROOT", root.c_str(), 1) == 0);
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
        write_file(root / "location.json",
                   R"({"latitude":31.2304,"longitude":121.4737,"city":"Shanghai","country_name":"China"})");
        write_file(root / "search.json", R"([{"lat":"48.8566","lon":"2.3522","display_name":"Paris, France"}])");
    }

    void block(const std::string &kind) const { write_file(root / (kind + ".block"), ""); }
    void release(const std::string &kind) const { write_file(root / (kind + ".release"), ""); }
    void fail(const std::string &kind) const { write_file(root / (kind + ".error"), ""); }
    void await_request(const std::string &kind) const
    {
        wait_until([&] { return std::filesystem::exists(root / (kind + ".started")); });
    }

    std::size_t request_count() const
    {
        const auto log = read_file(root / "requests");
        return static_cast<std::size_t>(std::count(log.begin(), log.end(), '\n'));
    }

    std::filesystem::path root;

private:
    std::string old_path_;
};

MapPresentationEvent await_event(MapViewModel &view_model)
{
    MapPresentationEvent result;
    wait_until([&] { return view_model.poll(result); });
    return result;
}

std::string status(MapViewModel &view_model)
{
    return lv_subject_get_string(view_model.status_subject());
}

void expect_point(const MapViewModel &view_model, GeoPoint point, int zoom)
{
    assert(std::abs(view_model.center().latitude - point.latitude) < 0.000001);
    assert(std::abs(view_model.center().longitude - point.longitude) < 0.000001);
    assert(view_model.zoom() == zoom);
}

void expect_ip_location(MapViewModel &view_model)
{
    expect_point(view_model, kIpLocation, 11);
    assert(lv_subject_get_int(view_model.zoom_subject()) == 11);
    assert(std::string(lv_subject_get_string(view_model.location_subject())) == "Shanghai, China");
    assert(status(view_model) == "Showing approximate IP location");
}

GnssFix fresh_fix(GeoPoint point, std::uint64_t sequence)
{
    return {point, sequence, std::chrono::steady_clock::now()};
}

void expect_gnss_location(MapViewModel &view_model, GeoPoint point, int zoom = 16)
{
    expect_point(view_model, point, zoom);
    assert(lv_subject_get_int(view_model.zoom_subject()) == zoom);
    assert(std::string(lv_subject_get_string(view_model.location_subject())).find("GNSS:") == 0U);
    assert(status(view_model) == "Showing GNSS location");
}

void test_offline_basics(Fixture &fixture)
{
    fixture.reset();
    ::setenv("LILYGO_MAPS_DISABLE_NETWORK", "1", 1);
    MapViewModel view_model(fixture.root / "openstreetmap", [] { return std::optional<GnssFix>{}; });
    assert(await_event(view_model).kind == MapPresentationEventKind::None);
    assert(view_model.zoom() == 3);
    assert(status(view_model) == "Finding your current location...");
    view_model.start();
    assert(await_event(view_model).kind == MapPresentationEventKind::ViewportChanged);
    assert(status(view_model).find("world map") != std::string::npos);

    const auto initial = view_model.center();
    view_model.zoom_in();
    assert(view_model.zoom() == 4);
    assert(lv_subject_get_int(view_model.zoom_subject()) == 4);
    view_model.pan_pixels(256.0, 0.0);
    assert(view_model.center().longitude != initial.longitude);
    assert(!view_model.prepare_viewport(568, 900, false).empty());
    view_model.search("   ");
    assert(status(view_model) == "Enter an address to search");
    view_model.clear_cache();
    assert(await_event(view_model).kind == MapPresentationEventKind::CacheCleared);
    assert(fixture.request_count() == 0U);
}

void test_cached_return(Fixture &fixture)
{
    fixture.reset();
    MapViewModel view_model(fixture.root / "openstreetmap", [] { return std::optional<GnssFix>{}; });
    assert(await_event(view_model).kind == MapPresentationEventKind::None);
    view_model.start();
    assert(await_event(view_model).kind == MapPresentationEventKind::ViewportChanged);
    expect_ip_location(view_model);

    view_model.pan_pixels(256.0, 128.0);
    view_model.zoom_in();
    ::setenv("LILYGO_MAPS_DISABLE_NETWORK", "1", 1);
    view_model.return_to_ip_location();
    expect_ip_location(view_model);
    assert(fixture.request_count() == 1U);

    ::unsetenv("LILYGO_MAPS_DISABLE_NETWORK");
    view_model.search("Paris");
    assert(await_event(view_model).kind == MapPresentationEventKind::ViewportChanged);
    expect_point(view_model, kSearchLocation, 14);
    ::setenv("LILYGO_MAPS_DISABLE_NETWORK", "1", 1);
    view_model.return_to_ip_location();
    expect_ip_location(view_model);
    assert(fixture.request_count() == 2U);

    view_model.locate();
    assert(status(view_model) == "Finding your current location...");
    assert(await_event(view_model).kind == MapPresentationEventKind::ViewportChanged);
    assert(status(view_model) == "Could not determine your location. Keeping the current map.");
    view_model.pan_pixels(256.0, 0.0);
    view_model.return_to_ip_location();
    expect_ip_location(view_model);
}

void test_late_location_is_cached(Fixture &fixture)
{
    fixture.reset();
    fixture.block("location");
    MapViewModel view_model(fixture.root / "openstreetmap", [] { return std::optional<GnssFix>{}; });
    assert(await_event(view_model).kind == MapPresentationEventKind::None);
    view_model.start();
    fixture.await_request("location");
    view_model.pan_pixels(256.0, 128.0);
    view_model.zoom_in();
    const auto browsing = view_model.center();
    const auto previous_status = status(view_model);
    fixture.release("location");
    assert(await_event(view_model).kind == MapPresentationEventKind::None);
    expect_point(view_model, browsing, 4);
    assert(status(view_model) == previous_status);

    ::setenv("LILYGO_MAPS_DISABLE_NETWORK", "1", 1);
    view_model.return_to_ip_location();
    expect_ip_location(view_model);
    assert(fixture.request_count() == 1U);
}

void test_return_ignores_pending_search(Fixture &fixture, bool search_fails)
{
    fixture.reset();
    fixture.block("search");
    if (search_fails) fixture.fail("search");
    MapViewModel view_model(fixture.root / "openstreetmap", [] { return std::optional<GnssFix>{}; });
    assert(await_event(view_model).kind == MapPresentationEventKind::None);
    view_model.start();
    assert(await_event(view_model).kind == MapPresentationEventKind::ViewportChanged);
    view_model.search("Paris");
    fixture.await_request("search");
    view_model.return_to_ip_location();
    expect_ip_location(view_model);
    fixture.release("search");
    assert(await_event(view_model).kind == MapPresentationEventKind::None);
    expect_ip_location(view_model);

    std::filesystem::remove(fixture.root / "search.error");
    view_model.search("Paris again");
    assert(await_event(view_model).kind == MapPresentationEventKind::ViewportChanged);
    expect_point(view_model, kSearchLocation, 14);
    assert(status(view_model) == "Address found");
}

void test_return_without_cached_location(Fixture &fixture)
{
    fixture.reset();
    MapViewModel view_model(fixture.root / "openstreetmap", [] { return std::optional<GnssFix>{}; });
    assert(await_event(view_model).kind == MapPresentationEventKind::None);
    view_model.pan_pixels(256.0, 0.0);
    view_model.return_to_ip_location();
    assert(status(view_model) == "Finding your current location...");
    assert(await_event(view_model).kind == MapPresentationEventKind::ViewportChanged);
    expect_ip_location(view_model);
    assert(fixture.request_count() == 1U);
}

void test_location_failure_preserves_browsing(Fixture &fixture)
{
    fixture.reset();
    fixture.block("location");
    fixture.fail("location");
    MapViewModel view_model(fixture.root / "openstreetmap", [] { return std::optional<GnssFix>{}; });
    assert(await_event(view_model).kind == MapPresentationEventKind::None);
    view_model.start();
    fixture.await_request("location");
    view_model.pan_pixels(256.0, 128.0);
    const auto browsing = view_model.center();
    const auto previous_status = status(view_model);
    fixture.release("location");
    assert(await_event(view_model).kind == MapPresentationEventKind::None);
    expect_point(view_model, browsing, 3);
    assert(status(view_model) == previous_status);

    view_model.return_to_ip_location();
    assert(await_event(view_model).kind == MapPresentationEventKind::ViewportChanged);
    expect_point(view_model, browsing, 3);
    assert(status(view_model) == "Could not determine your location. Keeping the current map.");
    assert(fixture.request_count() == 6U);
}

void test_gnss_follow_and_fallback(Fixture &fixture)
{
    fixture.reset();
    std::optional<GnssFix> fix;
    MapViewModel view_model(fixture.root / "openstreetmap", [&] { return fix; });
    assert(await_event(view_model).kind == MapPresentationEventKind::None);
    view_model.start();
    assert(await_event(view_model).kind == MapPresentationEventKind::ViewportChanged);
    expect_ip_location(view_model);

    fix = fresh_fix(kGnssLocation, 1);
    assert(await_event(view_model).kind == MapPresentationEventKind::ViewportChanged);
    expect_gnss_location(view_model, kGnssLocation);

    fix = fresh_fix(kGnssLocation, 2);
    assert(await_event(view_model).kind == MapPresentationEventKind::None);
    expect_gnss_location(view_model, kGnssLocation);
    fix = fresh_fix(kNextGnssLocation, 3);
    assert(await_event(view_model).kind == MapPresentationEventKind::ViewportChanged);
    expect_gnss_location(view_model, kNextGnssLocation);

    fix->received_at -= GnssService::fix_max_age + std::chrono::seconds(1);
    assert(await_event(view_model).kind == MapPresentationEventKind::ViewportChanged);
    expect_ip_location(view_model);
    assert(fixture.request_count() == 1U);
}

void test_gnss_precedes_late_ip(Fixture &fixture, bool ip_fails)
{
    fixture.reset();
    fixture.block("location");
    if (ip_fails) fixture.fail("location");
    std::optional<GnssFix> fix;
    MapViewModel view_model(fixture.root / "openstreetmap", [&] { return fix; });
    assert(await_event(view_model).kind == MapPresentationEventKind::None);
    view_model.start();
    fixture.await_request("location");
    fix = fresh_fix(kGnssLocation, 1);
    assert(await_event(view_model).kind == MapPresentationEventKind::ViewportChanged);
    expect_gnss_location(view_model, kGnssLocation);
    fixture.release("location");

    // Drain the delayed HTTP outcome while GNSS is valid: neither a result nor an
    // error may move the map or replace its GNSS status.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    do {
        MapPresentationEvent event;
        if (view_model.poll(event)) assert(event.kind == MapPresentationEventKind::None);
        expect_gnss_location(view_model, kGnssLocation);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);

    if (ip_fails) {
        fix.reset();
        assert(await_event(view_model).kind == MapPresentationEventKind::ViewportChanged);
        expect_point(view_model, kGnssLocation, 16);
        assert(status(view_model) == "Could not determine your location. Keeping the current map.");
    } else {
        // The service caches the suppressed IP result. Return must use that cache
        // immediately, even before the next poll observes the lost fix.
        view_model.pan_pixels(256.0, 128.0);
        fix.reset();
        ::setenv("LILYGO_MAPS_DISABLE_NETWORK", "1", 1);
        view_model.return_to_location();
        expect_ip_location(view_model);
        assert(fixture.request_count() == 1U);
    }
}

void test_gnss_respects_browsing_and_search(Fixture &fixture)
{
    fixture.reset();
    fixture.block("search");
    std::optional<GnssFix> fix;
    MapViewModel view_model(fixture.root / "openstreetmap", [&] { return fix; });
    assert(await_event(view_model).kind == MapPresentationEventKind::None);
    view_model.start();
    assert(await_event(view_model).kind == MapPresentationEventKind::ViewportChanged);
    fix = fresh_fix(kGnssLocation, 1);
    assert(await_event(view_model).kind == MapPresentationEventKind::ViewportChanged);
    view_model.pan_pixels(256.0, 128.0);
    view_model.zoom_out();
    const auto browsing = view_model.center();
    fix = fresh_fix(kNextGnssLocation, 2);
    assert(await_event(view_model).kind == MapPresentationEventKind::None);
    expect_point(view_model, browsing, 15);
    view_model.return_to_location();
    expect_gnss_location(view_model, kNextGnssLocation);
    assert(fixture.request_count() == 1U);

    view_model.search("Paris");
    fixture.await_request("search");
    fix = fresh_fix(kGnssLocation, 3);
    assert(await_event(view_model).kind == MapPresentationEventKind::None);
    expect_point(view_model, kNextGnssLocation, 16);
    assert(status(view_model) == "Searching for address...");
    view_model.return_to_location();
    expect_gnss_location(view_model, kGnssLocation);
    fixture.release("search");
    assert(await_event(view_model).kind == MapPresentationEventKind::None);
    expect_gnss_location(view_model, kGnssLocation);

    view_model.search("Paris again");
    assert(await_event(view_model).kind == MapPresentationEventKind::ViewportChanged);
    expect_point(view_model, kSearchLocation, 14);
    fix = fresh_fix(kNextGnssLocation, 4);
    assert(await_event(view_model).kind == MapPresentationEventKind::None);
    expect_point(view_model, kSearchLocation, 14);
    assert(status(view_model) == "Address found");
    view_model.return_to_location();
    expect_gnss_location(view_model, kNextGnssLocation);

    view_model.pan_pixels(256.0, 0.0);
    const auto next_browsing = view_model.center();
    fix.reset();
    assert(await_event(view_model).kind == MapPresentationEventKind::None);
    expect_point(view_model, next_browsing, 16);
    view_model.return_to_location();
    expect_ip_location(view_model);
    assert(fixture.request_count() == 3U);
}

}  // namespace

int main(int argc, char **argv)
{
    if (std::filesystem::path(argv[0]).filename() == "curl") return fake_curl(argc, argv);
    Fixture fixture(argv[0]);
    lv_init();
    test_offline_basics(fixture);
    test_cached_return(fixture);
    test_late_location_is_cached(fixture);
    test_return_ignores_pending_search(fixture, false);
    test_return_ignores_pending_search(fixture, true);
    test_return_without_cached_location(fixture);
    test_location_failure_preserves_browsing(fixture);
    test_gnss_follow_and_fallback(fixture);
    test_gnss_precedes_late_ip(fixture, false);
    test_gnss_precedes_late_ip(fixture, true);
    test_gnss_respects_browsing_and_search(fixture);
    lv_deinit();
    return 0;
}
