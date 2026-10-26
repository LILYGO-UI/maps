#include "services/map_service.hpp"

#include <json-c/json.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <locale>
#include <sstream>
#include <unordered_set>

namespace {

constexpr std::size_t kMaximumJsonBytes = 256U * 1024U;
constexpr std::size_t kMaximumTileBytes = 1024U * 1024U;

enum class LocationProviderKind { Ipapi, Ipinfo, IpApi };

struct LocationProvider {
    const char *name;
    const char *url;
    LocationProviderKind kind;
};

constexpr std::array<LocationProvider, 3> kLocationProviders{{
    {"ipapi.co", "https://ipapi.co/json/", LocationProviderKind::Ipapi},
    {"ipinfo.io", "https://ipinfo.io/json", LocationProviderKind::Ipinfo},
    {"ip-api.com", "http://ip-api.com/json/", LocationProviderKind::IpApi},
}};

bool network_disabled() noexcept
{
    const char *value = std::getenv("LILYGO_MAPS_DISABLE_NETWORK");
    return value && std::string_view(value) == "1";
}

bool json_member(json_object *object, const char *name, json_object **value) noexcept
{
    return object && json_object_get_type(object) == json_type_object && json_object_object_get_ex(object, name, value);
}

bool json_number(json_object *object, const char *name, double &value) noexcept
{
    json_object *member = nullptr;
    if (!json_member(object, name, &member)) return false;
    const auto type = json_object_get_type(member);
    if (type == json_type_double || type == json_type_int) {
        value = json_object_get_double(member);
        return std::isfinite(value);
    }
    if (type != json_type_string) return false;
    const char *text = json_object_get_string(member);
    if (!text || !*text) return false;
    char *end        = nullptr;
    const double parsed = std::strtod(text, &end);
    if (!end || *end != '\0' || !std::isfinite(parsed)) return false;
    value = parsed;
    return true;
}

std::string json_string(json_object *object, const char *name)
{
    json_object *member = nullptr;
    if (!json_member(object, name, &member) || json_object_get_type(member) != json_type_string) return {};
    const char *value = json_object_get_string(member);
    return value ? value : "";
}

std::string location_error(json_object *root, LocationProviderKind provider)
{
    if (!root || json_object_get_type(root) != json_type_object) return "invalid JSON response";
    json_object *error = nullptr;
    if (json_member(root, "error", &error) && error &&
        (json_object_get_type(error) != json_type_boolean || json_object_get_boolean(error))) {
        auto reason = json_string(root, "reason");
        if (reason.empty()) reason = json_string(error, "message");
        return reason.empty() ? "service reported an error" : reason;
    }
    json_object *bogon = nullptr;
    if (json_member(root, "bogon", &bogon) && json_object_get_boolean(bogon)) return "IP has no public location";
    if (provider == LocationProviderKind::IpApi && json_string(root, "status") != "success") {
        const auto message = json_string(root, "message");
        return message.empty() ? "service did not report success" : message;
    }
    return {};
}

bool location_coordinates(json_object *root, LocationProviderKind provider, GeoPoint &point)
{
    if (provider == LocationProviderKind::Ipinfo) {
        std::istringstream coordinates(json_string(root, "loc"));
        coordinates.imbue(std::locale::classic());
        char separator = '\0';
        if (!(coordinates >> point.latitude >> separator >> point.longitude) || separator != ',') return false;
        coordinates >> std::ws;
        if (!coordinates.eof()) return false;
    } else {
        const bool ip_api = provider == LocationProviderKind::IpApi;
        if (!json_number(root, ip_api ? "lat" : "latitude", point.latitude) ||
            !json_number(root, ip_api ? "lon" : "longitude", point.longitude)) return false;
    }
    return std::isfinite(point.latitude) && std::isfinite(point.longitude) &&
           point.latitude >= -90.0 && point.latitude <= 90.0 &&
           point.longitude >= -180.0 && point.longitude <= 180.0;
}

std::string location_label(json_object *root, LocationProviderKind provider)
{
    const std::array<const char *, 3> fields{
        "city", provider == LocationProviderKind::IpApi ? "regionName" : "region",
        provider == LocationProviderKind::Ipapi ? "country_name" : "country"};
    std::string result;
    for (const char *field : fields) {
        const auto value = json_string(root, field);
        if (value.empty() || result.find(value) != std::string::npos) continue;
        if (!result.empty()) result += ", ";
        result += value;
    }
    return result.empty() ? "Current IP location" : result;
}

}  // namespace

MapService::MapService(std::filesystem::path cache_root, GnssFixProvider gnss_provider)
    : cache_(std::move(cache_root)), gnss_provider_(std::move(gnss_provider))
{
    constexpr int kWorkerCount = 2;
    workers_.reserve(kWorkerCount);
    for (int index = 0; index < kWorkerCount; ++index) workers_.emplace_back(&MapService::worker_main, this);
    Job job;
    job.kind = JobKind::Stats;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push_back(std::move(job));
    }
    condition_.notify_one();
}

MapService::~MapService()
{
    stopping_.store(true);
    condition_.notify_all();
    http_.cancel();
    for (auto &worker : workers_) {
        if (worker.joinable()) worker.join();
    }
}

std::uint64_t MapService::locate()
{
    if (!gnss_provider_) gnss_.start();
    const auto request = next_request_id_.fetch_add(1);
    latest_location_id_.store(request);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Job job;
        job.kind       = JobKind::Locate;
        job.request_id = request;
        jobs_.push_front(std::move(job));
    }
    condition_.notify_one();
    return request;
}

std::uint64_t MapService::search(std::string query)
{
    const auto request = next_request_id_.fetch_add(1);
    latest_search_id_.store(request);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Job job;
        job.kind        = JobKind::Search;
        job.request_id  = request;
        job.cache_epoch = cache_epoch_.load();
        job.query       = std::move(query);
        jobs_.push_front(std::move(job));
    }
    condition_.notify_one();
    return request;
}

void MapService::request_tiles(std::uint64_t generation, const std::vector<TileId> &tiles)
{
    latest_tile_generation_.store(generation);
    std::unordered_set<std::string> wanted;
    wanted.reserve(tiles.size());
    for (const auto &tile : tiles) wanted.insert(tile_key(tile));

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto iterator = jobs_.begin(); iterator != jobs_.end();) {
        if (iterator->kind == JobKind::Tile && wanted.count(tile_key(iterator->tile)) == 0U) {
            pending_tiles_.erase(tile_key(iterator->tile));
            iterator = jobs_.erase(iterator);
        } else {
            ++iterator;
        }
    }

    const auto epoch = cache_epoch_.load();
    for (const auto &tile : tiles) {
        std::filesystem::path cached;
        if (cache_.find(tile, cached)) continue;
        const auto key = tile_key(tile);
        auto pending   = pending_tiles_.find(key);
        if (pending != pending_tiles_.end()) {
            pending->second = generation;
            continue;
        }
        pending_tiles_.emplace(key, generation);
        Job job;
        job.kind        = JobKind::Tile;
        job.generation  = generation;
        job.cache_epoch = epoch;
        job.tile        = tile;
        jobs_.push_back(std::move(job));
    }
    condition_.notify_all();
}

void MapService::clear_cache()
{
    const auto epoch = cache_epoch_.fetch_add(1) + 1U;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto iterator = jobs_.begin(); iterator != jobs_.end();) {
            if (iterator->kind == JobKind::Tile) {
                iterator = jobs_.erase(iterator);
            } else {
                ++iterator;
            }
        }
        pending_tiles_.clear();
        Job job;
        job.kind        = JobKind::Clear;
        job.cache_epoch = epoch;
        jobs_.push_front(std::move(job));
    }
    condition_.notify_all();
}

bool MapService::poll(MapServiceEvent &event)
{
    const auto fix = current_gnss_fix();
    const auto request = latest_location_id_.load();
    if (request != 0 && fix) {
        if (!gnss_active_ || fix->sequence != gnss_sequence_ || request != gnss_request_id_) {
            gnss_active_ = true;
            gnss_sequence_ = fix->sequence;
            gnss_request_id_ = request;
            event = {};
            event.kind = MapServiceEventKind::LocationReady;
            event.request_id = request;
            event.location_source = LocationSource::Gnss;
            event.point = fix->point;
            std::ostringstream label;
            label.imbue(std::locale::classic());
            label << "GNSS " << std::fixed << std::setprecision(6)
                  << fix->point.latitude << ", " << fix->point.longitude;
            event.label = label.str();
            return true;
        }
    } else if (gnss_active_) {
        gnss_active_ = false;
        const auto &fallback = ip_location_ ? ip_location_ : location_failure_;
        if (fallback) {
            event = *fallback;
            event.request_id = request;
            return true;
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    while (!events_.empty()) {
        event = std::move(events_.front());
        events_.pop_front();

        const bool stale_location =
            (event.kind == MapServiceEventKind::LocationReady || event.kind == MapServiceEventKind::LocationError) &&
            event.request_id != latest_location_id_.load();
        const bool stale_search =
            (event.kind == MapServiceEventKind::SearchReady || event.kind == MapServiceEventKind::SearchError) &&
            event.request_id != latest_search_id_.load();
        const bool stale_tile =
            (event.kind == MapServiceEventKind::TileReady || event.kind == MapServiceEventKind::TileError) &&
            event.generation != latest_tile_generation_.load();
        if (stale_location || stale_search || stale_tile) continue;
        if (event.kind == MapServiceEventKind::LocationReady) {
            ip_location_ = event;
            location_failure_.reset();
            if (fix) continue;
        } else if (event.kind == MapServiceEventKind::LocationError) {
            location_failure_ = event;
            if (fix) continue;
        }
        return true;
    }
    return false;
}

std::optional<GnssFix> MapService::current_gnss_fix() const
{
    const auto fix = gnss_provider_ ? gnss_provider_() : gnss_.current_fix();
    if (!fix) return std::nullopt;
    const auto age = std::chrono::steady_clock::now() - fix->received_at;
    if (age < std::chrono::steady_clock::duration::zero() || age > GnssService::fix_max_age ||
        !std::isfinite(fix->point.latitude) || !std::isfinite(fix->point.longitude) ||
        fix->point.latitude < -90.0 || fix->point.latitude > 90.0 ||
        fix->point.longitude < -180.0 || fix->point.longitude > 180.0) return std::nullopt;
    return fix;
}

std::optional<MapServiceEvent> MapService::cached_ip_location() const
{
    return ip_location_;
}

bool MapService::find_tile(const TileId &tile, std::filesystem::path &path) const
{
    return cache_.find(tile, path);
}

CacheStats MapService::cache_stats() const noexcept
{
    return {cache_bytes_.load(), cache_tiles_.load()};
}

const std::filesystem::path &MapService::cache_root() const noexcept
{
    return cache_.root();
}

std::string MapService::tile_url(const TileId &tile)
{
    return "https://tile.openstreetmap.de/" + std::to_string(tile.zoom) + "/" + std::to_string(tile.x) + "/" +
           std::to_string(tile.y) + ".png";
}

std::string MapService::url_encode(std::string_view value)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size() * 3U);
    for (const unsigned char character : value) {
        if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '-' || character == '_' || character == '.' ||
            character == '~') {
            result.push_back(static_cast<char>(character));
        } else {
            result.push_back('%');
            result.push_back(hex[character >> 4U]);
            result.push_back(hex[character & 0x0fU]);
        }
    }
    return result;
}

void MapService::worker_main()
{
    while (!stopping_.load()) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] { return stopping_.load() || !jobs_.empty(); });
            if (stopping_.load()) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
            if (job.kind == JobKind::Tile) {
                const auto pending = pending_tiles_.find(tile_key(job.tile));
                if (pending == pending_tiles_.end()) continue;
                job.generation = pending->second;
                if (job.generation != latest_tile_generation_.load()) {
                    pending_tiles_.erase(pending);
                    continue;
                }
            }
        }

        switch (job.kind) {
            case JobKind::Locate:
                run_location(job);
                break;
            case JobKind::Search:
                run_search(job);
                break;
            case JobKind::Tile:
                run_tile(job);
                break;
            case JobKind::Clear:
                run_clear(job);
                break;
            case JobKind::Stats:
                run_stats();
                break;
        }
    }
}

void MapService::run_location(const Job &job)
{
    if (network_disabled()) {
        if (job.request_id != latest_location_id_.load()) return;
        MapServiceEvent event;
        event.kind       = MapServiceEventKind::LocationError;
        event.request_id = job.request_id;
        event.error      = "Current-location lookup is unavailable while network access is disabled";
        push_event(std::move(event));
        return;
    }
    std::string failures;
    for (const auto &provider : kLocationProviders) {
        if (stopping_.load() || job.request_id != latest_location_id_.load()) return;
        const auto response = http_.get(provider.url, "application/json", kMaximumJsonBytes);
        if (stopping_.load() || job.request_id != latest_location_id_.load()) return;

        std::string error = response.error;
        if (response) {
            json_tokener *tokener = json_tokener_new();
            json_object *root = nullptr;
            if (tokener) {
                json_tokener_set_flags(tokener, JSON_TOKENER_STRICT);
                root = json_tokener_parse_ex(tokener, response.body.c_str(), static_cast<int>(response.body.size() + 1U));
                json_tokener_free(tokener);
            }
            GeoPoint point;
            error = location_error(root, provider.kind);
            if (error.empty() && !location_coordinates(root, provider.kind, point)) error = "invalid coordinates";
            const auto label = error.empty() ? location_label(root, provider.kind) : std::string{};
            if (root) json_object_put(root);
            if (error.empty()) {
                if (stopping_.load() || job.request_id != latest_location_id_.load()) return;
                MapServiceEvent event;
                event.kind       = MapServiceEventKind::LocationReady;
                event.request_id = job.request_id;
                event.point      = point;
                event.label      = label;
                push_event(std::move(event));
                return;
            }
        }

        if (!failures.empty()) failures += "; ";
        failures += std::string(provider.name) + ": " + error;
    }

    if (!stopping_.load() && job.request_id == latest_location_id_.load()) {
        MapServiceEvent event;
        event.kind       = MapServiceEventKind::LocationError;
        event.request_id = job.request_id;
        event.error      = "Current-location lookup failed: " + failures;
        push_event(std::move(event));
    }
}

void MapService::run_search(const Job &job)
{
    if (network_disabled()) {
        if (job.request_id != latest_search_id_.load()) return;
        MapServiceEvent event;
        event.kind       = MapServiceEventKind::SearchError;
        event.request_id = job.request_id;
        event.error      = "Address search is unavailable while network access is disabled";
        push_event(std::move(event));
        return;
    }
    const auto url = "https://nominatim.openstreetmap.org/search?format=jsonv2&limit=1&q=" + url_encode(job.query);
    const auto response = http_.get(url, "application/json", kMaximumJsonBytes);
    if (!response) {
        if (job.request_id == latest_search_id_.load()) {
            MapServiceEvent event;
            event.kind       = MapServiceEventKind::SearchError;
            event.request_id = job.request_id;
            event.error      = response.error;
            push_event(std::move(event));
        }
        return;
    }

    json_object *root = json_tokener_parse(response.body.c_str());
    json_object *first = nullptr;
    if (root && json_object_get_type(root) == json_type_array && json_object_array_length(root) > 0U) {
        first = json_object_array_get_idx(root, 0U);
    }
    double latitude  = 0.0;
    double longitude = 0.0;
    if (!first || !json_number(first, "lat", latitude) || !json_number(first, "lon", longitude)) {
        if (root) json_object_put(root);
        if (job.request_id != latest_search_id_.load()) return;
        MapServiceEvent event;
        event.kind       = MapServiceEventKind::SearchError;
        event.request_id = job.request_id;
        event.error      = "No matching address found";
        push_event(std::move(event));
        return;
    }
    auto label = json_string(first, "display_name");
    json_object_put(root);
    if (job.request_id == latest_search_id_.load()) {
        MapServiceEvent event;
        event.kind       = MapServiceEventKind::SearchReady;
        event.request_id = job.request_id;
        event.point      = {latitude, longitude};
        event.label      = std::move(label);
        push_event(std::move(event));
    }
}

void MapService::run_tile(const Job &job)
{
    if (network_disabled()) {
        finish_tile(job, {}, "Online tiles are unavailable while network access is disabled");
        return;
    }
    const auto response = http_.get(tile_url(job.tile), "image/png", kMaximumTileBytes);
    if (!response) {
        finish_tile(job, {}, response.error);
        return;
    }

    std::filesystem::path path;
    std::string error;
    {
        std::lock_guard<std::mutex> lock(cache_commit_mutex_);
        if (job.cache_epoch != cache_epoch_.load()) {
            finish_tile(job, {}, {});
            return;
        }
        if (!cache_.store(job.tile, response.body, path, error)) {
            finish_tile(job, {}, std::move(error));
            return;
        }
    }
    cache_bytes_.fetch_add(response.body.size());
    cache_tiles_.fetch_add(1U);
    finish_tile(job, std::move(path), {});
}

void MapService::run_clear(const Job &job)
{
    std::string error;
    {
        std::lock_guard<std::mutex> lock(cache_commit_mutex_);
        if (job.cache_epoch != cache_epoch_.load()) return;
        if (!cache_.clear(error)) {
            MapServiceEvent event;
            event.kind  = MapServiceEventKind::CacheError;
            event.error = std::move(error);
            push_event(std::move(event));
            return;
        }
    }
    cache_bytes_.store(0);
    cache_tiles_.store(0);
    MapServiceEvent event;
    event.kind  = MapServiceEventKind::CacheCleared;
    event.cache = {};
    push_event(std::move(event));
}

void MapService::run_stats()
{
    std::lock_guard<std::mutex> lock(cache_commit_mutex_);
    const auto stats = cache_.stats();
    cache_bytes_.store(stats.bytes);
    cache_tiles_.store(stats.tiles);
    MapServiceEvent event;
    event.kind  = MapServiceEventKind::CacheStatsReady;
    event.cache = stats;
    push_event(std::move(event));
}

void MapService::push_event(MapServiceEvent event)
{
    if (stopping_.load()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back(std::move(event));
}

void MapService::finish_tile(const Job &job, std::filesystem::path path, std::string error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto key     = tile_key(job.tile);
    const auto pending = pending_tiles_.find(key);
    if (pending == pending_tiles_.end()) return;
    const auto generation = pending->second;
    pending_tiles_.erase(pending);
    if (generation != latest_tile_generation_.load() || job.cache_epoch != cache_epoch_.load()) return;

    if (!error.empty()) {
        if (reported_tile_failure_generation_ == generation) return;
        reported_tile_failure_generation_ = generation;
        MapServiceEvent event;
        event.kind       = MapServiceEventKind::TileError;
        event.generation = generation;
        event.error      = std::move(error);
        events_.push_back(std::move(event));
        return;
    }
    if (path.empty()) return;

    MapServiceEvent event;
    event.kind       = MapServiceEventKind::TileReady;
    event.generation = generation;
    event.tile       = job.tile;
    event.path       = std::move(path);
    event.cache      = cache_stats();
    events_.push_back(std::move(event));
}

std::string MapService::tile_key(const TileId &tile)
{
    return std::to_string(tile.zoom) + "/" + std::to_string(tile.x) + "/" + std::to_string(tile.y);
}
