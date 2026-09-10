#ifndef LILYGO_UI_MAPS_SERVICES_MAP_SERVICE_HPP
#define LILYGO_UI_MAPS_SERVICES_MAP_SERVICE_HPP

#include "domain/map_model.hpp"
#include "domain/tile_cache.hpp"
#include "services/http_client.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

enum class MapServiceEventKind {
    LocationReady,
    SearchReady,
    TileReady,
    CacheCleared,
    CacheStatsReady,
    LocationError,
    SearchError,
    TileError,
    CacheError,
};

struct MapServiceEvent {
    MapServiceEventKind kind = MapServiceEventKind::TileError;
    std::uint64_t request_id = 0;
    std::uint64_t generation = 0;
    GeoPoint point;
    TileId tile;
    std::filesystem::path path;
    std::string label;
    std::string error;
    CacheStats cache;
};

class MapService {
public:
    explicit MapService(std::filesystem::path cache_root = TileCache::default_root());
    ~MapService();

    MapService(const MapService &)            = delete;
    MapService &operator=(const MapService &) = delete;

    std::uint64_t locate_by_ip();
    std::uint64_t search(std::string query);
    void request_tiles(std::uint64_t generation, const std::vector<TileId> &tiles);
    void clear_cache();

    [[nodiscard]] bool poll(MapServiceEvent &event);
    [[nodiscard]] bool find_tile(const TileId &tile, std::filesystem::path &path) const;
    [[nodiscard]] CacheStats cache_stats() const noexcept;
    [[nodiscard]] const std::filesystem::path &cache_root() const noexcept;

    [[nodiscard]] static std::string tile_url(const TileId &tile);
    [[nodiscard]] static std::string url_encode(std::string_view value);

private:
    enum class JobKind { Locate, Search, Tile, Clear, Stats };

    struct Job {
        JobKind kind = JobKind::Tile;
        std::uint64_t request_id = 0;
        std::uint64_t generation = 0;
        std::uint64_t cache_epoch = 0;
        std::string query;
        TileId tile;
    };

    void worker_main();
    void run_location(const Job &job);
    void run_search(const Job &job);
    void run_tile(const Job &job);
    void run_clear(const Job &job);
    void run_stats();
    void push_event(MapServiceEvent event);
    void finish_tile(const Job &job, std::filesystem::path path, std::string error);
    [[nodiscard]] static std::string tile_key(const TileId &tile);

    TileCache cache_;
    HttpClient http_;
    std::vector<std::thread> workers_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Job> jobs_;
    std::deque<MapServiceEvent> events_;
    std::unordered_map<std::string, std::uint64_t> pending_tiles_;
    std::mutex cache_commit_mutex_;
    std::atomic_bool stopping_{false};
    std::atomic<std::uint64_t> next_request_id_{1};
    std::atomic<std::uint64_t> latest_location_id_{0};
    std::atomic<std::uint64_t> latest_search_id_{0};
    std::atomic<std::uint64_t> latest_tile_generation_{0};
    std::atomic<std::uint64_t> cache_epoch_{0};
    std::atomic<std::uint64_t> cache_bytes_{0};
    std::atomic<std::uint64_t> cache_tiles_{0};
    std::uint64_t reported_tile_failure_generation_ = 0;
};

#endif
