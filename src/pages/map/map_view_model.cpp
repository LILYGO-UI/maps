#include "pages/map/map_view_model.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace {

std::string cache_text(CacheStats stats)
{
    std::ostringstream stream;
    stream << "Cache ";
    if (stats.bytes < 1024U) {
        stream << stats.bytes << " B";
    } else if (stats.bytes < 1024U * 1024U) {
        stream << std::fixed << std::setprecision(1) << static_cast<double>(stats.bytes) / 1024.0 << " KB";
    } else {
        stream << std::fixed << std::setprecision(1) << static_cast<double>(stats.bytes) / (1024.0 * 1024.0) << " MB";
    }
    return stream.str();
}

std::string tile_key(const TileId &tile)
{
    return std::to_string(tile.zoom) + "/" + std::to_string(tile.x) + "/" + std::to_string(tile.y);
}

}  // namespace

MapViewModel::MapViewModel(std::filesystem::path cache_root, GnssFixProvider gnss_provider)
    : service_(std::move(cache_root), std::move(gnss_provider))
{
    publish_cache(service_.cache_stats());
}

void MapViewModel::start()
{
    locate();
}

void MapViewModel::locate()
{
    active_search_id_ = 0;
    follow_location_ = true;
    displayed_location_source_.reset();
    status_.set("Finding your current location...");
    service_.locate();
}

void MapViewModel::return_to_location()
{
    active_search_id_ = 0;
    follow_location_ = true;
    if (const auto fix = service_.current_gnss_fix()) {
        show_gnss_location(fix->point, true);
        return;
    }
    if (const auto cached = service_.cached_ip_location()) {
        ip_location_ = cached->point;
        ip_location_label_ = cached->label;
    }
    if (!ip_location_) {
        locate();
        return;
    }
    show_ip_location();
}

void MapViewModel::return_to_ip_location()
{
    return_to_location();
}

void MapViewModel::search(std::string query)
{
    query.erase(query.begin(), std::find_if(query.begin(), query.end(), [](unsigned char value) { return value != ' '; }));
    while (!query.empty() && query.back() == ' ') query.pop_back();
    if (query.empty()) {
        status_.set("Enter an address to search");
        return;
    }
    follow_location_ = false;
    status_.set("Searching for address...");
    active_search_id_ = service_.search(std::move(query));
}

void MapViewModel::clear_cache()
{
    status_.set("Clearing cached map data...");
    service_.clear_cache();
}

void MapViewModel::pan_pixels(double horizontal, double vertical)
{
    follow_location_ = false;
    map_ready_ = true;
    model_.pan_pixels(horizontal, vertical);
}

void MapViewModel::zoom_in()
{
    follow_location_ = false;
    map_ready_ = true;
    model_.zoom_in();
    publish_zoom();
}

void MapViewModel::zoom_out()
{
    follow_location_ = false;
    map_ready_ = true;
    model_.zoom_out();
    publish_zoom();
}

bool MapViewModel::poll(MapPresentationEvent &presentation)
{
    MapServiceEvent event;
    if (!service_.poll(event)) return false;
    presentation = {};

    switch (event.kind) {
        case MapServiceEventKind::LocationReady:
            if (event.location_source == LocationSource::Gnss) {
                if (follow_location_ &&
                    show_gnss_location(event.point, displayed_location_source_ != LocationSource::Gnss)) {
                    presentation.kind = MapPresentationEventKind::ViewportChanged;
                }
                return true;
            }
            ip_location_ = event.point;
            ip_location_label_ = std::move(event.label);
            if (!follow_location_ || service_.current_gnss_fix()) return true;
            show_ip_location();
            presentation.kind = MapPresentationEventKind::ViewportChanged;
            return true;
        case MapServiceEventKind::SearchReady:
            if (event.request_id != active_search_id_) return true;
            active_search_id_ = 0;
            model_.center_on(event.point, 14);
            map_ready_ = true;
            displayed_location_source_.reset();
            location_.set(event.label);
            status_.set("Address found");
            publish_zoom();
            presentation.kind = MapPresentationEventKind::ViewportChanged;
            return true;
        case MapServiceEventKind::TileReady:
            publish_cache(event.cache);
            presentation.kind       = MapPresentationEventKind::TileReady;
            presentation.generation = event.generation;
            presentation.tile       = event.tile;
            presentation.path       = std::move(event.path);
            return true;
        case MapServiceEventKind::CacheCleared:
            publish_cache({});
            status_.set("Cached map data cleared");
            presentation.kind = MapPresentationEventKind::CacheCleared;
            return true;
        case MapServiceEventKind::CacheStatsReady:
            publish_cache(event.cache);
            return true;
        case MapServiceEventKind::LocationError:
            if (!follow_location_ || service_.current_gnss_fix()) return true;
            status_.set(map_ready_ ? "Could not determine your location. Keeping the current map."
                                   : "Could not determine your location. Showing the world map.");
            map_ready_ = true;
            presentation.kind = MapPresentationEventKind::ViewportChanged;
            return true;
        case MapServiceEventKind::SearchError:
            if (event.request_id != active_search_id_) return true;
            active_search_id_ = 0;
            status_.set(event.error.empty() ? "Address search failed" : event.error);
            return true;
        case MapServiceEventKind::TileError:
            status_.set("Online map data is temporarily unavailable");
            return true;
        case MapServiceEventKind::CacheError:
            status_.set(event.error.empty() ? "Could not clear cached map data" : event.error);
            return true;
    }
    return true;
}

std::vector<VisibleTile> MapViewModel::prepare_viewport(int width, int height, bool download)
{
    auto visible = model_.visible_tiles(width, height);
    ++viewport_generation_;
    if (!download || !map_ready_) return visible;

    auto download_order = visible;
    const int center_x   = width / 2;
    const int center_y   = height / 2;
    std::stable_sort(download_order.begin(), download_order.end(), [center_x, center_y](const auto &left,
                                                                                        const auto &right) {
        const auto left_x  = static_cast<long long>(left.screen_x + MapModel::tile_size / 2 - center_x);
        const auto left_y  = static_cast<long long>(left.screen_y + MapModel::tile_size / 2 - center_y);
        const auto right_x = static_cast<long long>(right.screen_x + MapModel::tile_size / 2 - center_x);
        const auto right_y = static_cast<long long>(right.screen_y + MapModel::tile_size / 2 - center_y);
        return left_x * left_x + left_y * left_y < right_x * right_x + right_y * right_y;
    });
    std::unordered_set<std::string> seen;
    std::vector<TileId> unique;
    unique.reserve(download_order.size());
    for (const auto &item : download_order) {
        if (seen.insert(tile_key(item.id)).second) unique.push_back(item.id);
    }
    service_.request_tiles(viewport_generation_, unique);
    return visible;
}

bool MapViewModel::find_tile(const TileId &tile, std::filesystem::path &path) const
{
    return service_.find_tile(tile, path);
}

GeoPoint MapViewModel::center() const noexcept
{
    return model_.center();
}

int MapViewModel::zoom() const noexcept
{
    return model_.zoom();
}

std::uint64_t MapViewModel::viewport_generation() const noexcept
{
    return viewport_generation_;
}

lv_subject_t *MapViewModel::status_subject() noexcept
{
    return status_.get();
}

lv_subject_t *MapViewModel::location_subject() noexcept
{
    return location_.get();
}

lv_subject_t *MapViewModel::cache_subject() noexcept
{
    return cache_.get();
}

lv_subject_t *MapViewModel::zoom_subject() noexcept
{
    return zoom_.get();
}

void MapViewModel::publish_cache(CacheStats stats)
{
    cache_.set(cache_text(stats));
}

void MapViewModel::publish_zoom() noexcept
{
    zoom_.set(model_.zoom());
}

void MapViewModel::show_ip_location()
{
    model_.center_on(*ip_location_, 11);
    map_ready_ = true;
    displayed_location_source_ = LocationSource::Ip;
    location_.set(ip_location_label_);
    status_.set("Showing approximate IP location");
    publish_zoom();
}

bool MapViewModel::show_gnss_location(GeoPoint point, bool reset_zoom)
{
    const auto previous = model_.center();
    const int target_zoom = reset_zoom ? 16 : model_.zoom();
    const int previous_zoom = model_.zoom();
    model_.center_on(point, target_zoom);
    const auto current = model_.center();
    const bool changed = !map_ready_ || previous.latitude != current.latitude ||
                         previous.longitude != current.longitude || previous_zoom != model_.zoom();
    map_ready_ = true;
    displayed_location_source_ = LocationSource::Gnss;
    std::ostringstream label;
    label << "GNSS: " << std::fixed << std::setprecision(6) << point.latitude << ", " << point.longitude;
    location_.set(label.str());
    status_.set("Showing GNSS location");
    publish_zoom();
    return changed;
}
