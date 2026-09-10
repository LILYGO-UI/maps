#ifndef LILYGO_UI_MAPS_DOMAIN_TILE_CACHE_HPP
#define LILYGO_UI_MAPS_DOMAIN_TILE_CACHE_HPP

#include "domain/map_model.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

struct CacheStats {
    std::uint64_t bytes = 0;
    std::uint64_t tiles = 0;
};

class TileCache {
public:
    explicit TileCache(std::filesystem::path root);

    [[nodiscard]] const std::filesystem::path &root() const noexcept;
    [[nodiscard]] std::filesystem::path path_for(const TileId &tile) const;
    [[nodiscard]] bool find(const TileId &tile, std::filesystem::path &path) const;
    [[nodiscard]] bool store(const TileId &tile, std::string_view png, std::filesystem::path &path,
                             std::string &error);
    [[nodiscard]] CacheStats stats() const;
    [[nodiscard]] bool clear(std::string &error);

    [[nodiscard]] static bool valid_tile_png(std::string_view bytes) noexcept;
    [[nodiscard]] static std::filesystem::path default_root();

private:
    [[nodiscard]] static bool valid_tile_file(const std::filesystem::path &path);

    std::filesystem::path root_;
    mutable std::mutex mutex_;
};

#endif
