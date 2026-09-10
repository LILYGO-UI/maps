#ifndef LILYGO_UI_MAPS_PAGES_MAP_MAP_VIEW_MODEL_HPP
#define LILYGO_UI_MAPS_PAGES_MAP_MAP_VIEW_MODEL_HPP

#include "components/lv_subject.hpp"
#include "domain/map_model.hpp"
#include "services/map_service.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

enum class MapPresentationEventKind { None, ViewportChanged, TileReady, CacheCleared };

struct MapPresentationEvent {
    MapPresentationEventKind kind = MapPresentationEventKind::None;
    std::uint64_t generation      = 0;
    TileId tile;
    std::filesystem::path path;
};

class MapViewModel {
public:
    explicit MapViewModel(std::filesystem::path cache_root = TileCache::default_root());

    void start();
    void locate();
    void return_to_ip_location();
    void search(std::string query);
    void clear_cache();
    void pan_pixels(double horizontal, double vertical);
    void zoom_in();
    void zoom_out();

    [[nodiscard]] bool poll(MapPresentationEvent &event);
    [[nodiscard]] std::vector<VisibleTile> prepare_viewport(int width, int height, bool download = true);
    [[nodiscard]] bool find_tile(const TileId &tile, std::filesystem::path &path) const;
    [[nodiscard]] GeoPoint center() const noexcept;
    [[nodiscard]] int zoom() const noexcept;
    [[nodiscard]] std::uint64_t viewport_generation() const noexcept;

    [[nodiscard]] lv_subject_t *status_subject() noexcept;
    [[nodiscard]] lv_subject_t *location_subject() noexcept;
    [[nodiscard]] lv_subject_t *cache_subject() noexcept;
    [[nodiscard]] lv_subject_t *zoom_subject() noexcept;

private:
    void publish_cache(CacheStats stats);
    void publish_zoom() noexcept;
    void show_ip_location();

    MapModel model_;
    MapService service_;
    StringSubject<192> status_{"Finding your current location..."};
    StringSubject<192> location_{"IP location pending"};
    StringSubject<64> cache_{"Cache 0 B"};
    IntSubject zoom_{model_.zoom(), MapModel::minimum_zoom, MapModel::maximum_zoom};
    std::uint64_t viewport_generation_ = 0;
    std::uint64_t active_search_id_     = 0;
    std::optional<GeoPoint> ip_location_;
    std::string ip_location_label_;
    bool map_ready_                    = false;
    bool accept_ip_location_           = true;
};

#endif
