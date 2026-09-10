#include "domain/map_model.hpp"
#include "domain/tile_cache.hpp"
#include "services/map_service.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

bool close_to(double left, double right, double tolerance = 0.000001)
{
    return std::abs(left - right) <= tolerance;
}

std::filesystem::path temporary_directory()
{
    const auto path = std::filesystem::temp_directory_path() /
                      ("lilygo-maps-core-" + std::to_string(static_cast<long long>(::getpid())));
    std::error_code error;
    std::filesystem::remove_all(path, error);
    assert(std::filesystem::create_directories(path, error));
    return path;
}

std::string test_png()
{
    std::string png(45, '\0');
    const unsigned char signature[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
    for (std::size_t index = 0; index < sizeof(signature); ++index) png[index] = static_cast<char>(signature[index]);
    png[11] = 13;
    png[12] = 'I';
    png[13] = 'H';
    png[14] = 'D';
    png[15] = 'R';
    png[18] = 1;
    png[22] = 1;
    png[37] = 'I';
    png[38] = 'E';
    png[39] = 'N';
    png[40] = 'D';
    return png;
}

void test_projection_and_navigation()
{
    MapModel model;
    model.center_on({31.2304, 121.4737}, 12);
    const double x = MapModel::longitude_to_world_x(model.center().longitude, model.zoom());
    const double y = MapModel::latitude_to_world_y(model.center().latitude, model.zoom());
    assert(close_to(MapModel::world_x_to_longitude(x, model.zoom()), 121.4737));
    assert(close_to(MapModel::world_y_to_latitude(y, model.zoom()), 31.2304));

    model.center_on({100.0, 540.0}, 99);
    assert(model.center().latitude < 85.052);
    assert(close_to(model.center().longitude, -180.0));
    assert(model.zoom() == MapModel::maximum_zoom);
    for (int index = 0; index < 30; ++index) model.zoom_out();
    assert(model.zoom() == MapModel::minimum_zoom);

    model.center_on({0.0, 179.9}, 4);
    model.pan_pixels(100.0, 0.0);
    assert(model.center().longitude < -170.0);

    const auto valid_center = model.center();
    model.center_on({std::numeric_limits<double>::quiet_NaN(), 0.0}, 8);
    assert(close_to(model.center().latitude, valid_center.latitude));
    assert(close_to(model.center().longitude, valid_center.longitude));
    assert(model.zoom() == 4);
}

void test_visible_tiles()
{
    MapModel model;
    model.center_on({51.5074, -0.1278}, 10);
    const auto tiles = model.visible_tiles(568, 900);
    assert(!tiles.empty());
    for (const auto &tile : tiles) {
        assert(tile.id.zoom == 10);
        assert(tile.id.x >= 0 && tile.id.x < (1 << tile.id.zoom));
        assert(tile.id.y >= 0 && tile.id.y < (1 << tile.id.zoom));
    }
}

void test_tile_cache()
{
    const auto temporary_root = temporary_directory();
    const auto root           = temporary_root / "openstreetmap";
    TileCache cache(root);
    const TileId tile{12, 3346, 1731};
    std::filesystem::path path;
    assert(!cache.find(tile, path));
    auto png = test_png();
    assert(TileCache::valid_tile_png(png));
    png[18] = 2;
    assert(!TileCache::valid_tile_png(png));
    png[18] = 1;
    std::string error;
    assert(cache.store(tile, png, path, error));
    assert(path == root / "12/3346/1731.png");
    assert(cache.find(tile, path));
    const auto stats = cache.stats();
    assert(stats.tiles == 1);
    assert(stats.bytes == png.size());
    assert(cache.clear(error));
    assert(cache.stats().tiles == 0);
    TileCache cache_with_trailing_separator(root / "");
    assert(cache_with_trailing_separator.clear(error));
    std::filesystem::remove_all(temporary_root);

    const auto unsafe_root = temporary_directory();
    TileCache unsafe_cache(unsafe_root);
    assert(!unsafe_cache.clear(error));
    assert(std::filesystem::exists(unsafe_root));
    std::filesystem::remove_all(unsafe_root);
}

void test_urls()
{
    assert(MapService::tile_url({5, 26, 13}) == "https://tile.openstreetmap.de/5/26/13.png");
    assert(MapService::url_encode("Shanghai Pudong") == "Shanghai%20Pudong");
    assert(MapService::url_encode("北京") == "%E5%8C%97%E4%BA%AC");
}

void test_stale_search_events_are_discarded()
{
    ::setenv("LILYGO_MAPS_DISABLE_NETWORK", "1", 1);
    const auto temporary_root = temporary_directory();
    const auto root           = temporary_root / "openstreetmap";
    MapService service(root);

    const auto stale_request = service.search("first request");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto current_request = service.search("second request");

    bool received_current = false;
    for (int attempt = 0; attempt < 100 && !received_current; ++attempt) {
        MapServiceEvent event;
        while (service.poll(event)) {
            if (event.kind != MapServiceEventKind::SearchError) continue;
            assert(event.request_id != stale_request);
            if (event.request_id == current_request) received_current = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(received_current);
    std::filesystem::remove_all(temporary_root);
}

}  // namespace

int main()
{
    test_projection_and_navigation();
    test_visible_tiles();
    test_tile_cache();
    test_urls();
    test_stale_search_events_are_discarded();
    return 0;
}
