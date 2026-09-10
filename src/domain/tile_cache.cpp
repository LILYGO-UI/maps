#include "domain/tile_cache.hpp"

#include <array>
#include <cstdlib>
#include <fstream>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace {

constexpr std::size_t kMaximumTileBytes = 1024U * 1024U;
constexpr std::array<unsigned char, 8> kPngSignature{0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};

std::uint32_t read_big_endian(const unsigned char *value) noexcept
{
    return (static_cast<std::uint32_t>(value[0]) << 24U) | (static_cast<std::uint32_t>(value[1]) << 16U) |
           (static_cast<std::uint32_t>(value[2]) << 8U) | static_cast<std::uint32_t>(value[3]);
}

}  // namespace

TileCache::TileCache(std::filesystem::path root) : root_(std::move(root))
{
}

const std::filesystem::path &TileCache::root() const noexcept
{
    return root_;
}

std::filesystem::path TileCache::path_for(const TileId &tile) const
{
    return root_ / std::to_string(tile.zoom) / std::to_string(tile.x) / (std::to_string(tile.y) + ".png");
}

bool TileCache::find(const TileId &tile, std::filesystem::path &path) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    path = path_for(tile);
    return valid_tile_file(path);
}

bool TileCache::store(const TileId &tile, std::string_view png, std::filesystem::path &path, std::string &error)
{
    if (!valid_tile_png(png)) {
        error = "Tile server returned an invalid 256 x 256 PNG";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    path = path_for(tile);
    std::error_code filesystem_error;
    std::filesystem::create_directories(path.parent_path(), filesystem_error);
    if (filesystem_error) {
        error = "Could not create the map cache directory";
        return false;
    }

    auto temporary = path;
    temporary += ".part";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            error = "Could not write a map tile";
            return false;
        }
        file.write(png.data(), static_cast<std::streamsize>(png.size()));
        if (!file.good()) {
            file.close();
            std::filesystem::remove(temporary, filesystem_error);
            error = "Could not finish writing a map tile";
            return false;
        }
    }

    std::filesystem::rename(temporary, path, filesystem_error);
    if (filesystem_error) {
        std::filesystem::remove(path, filesystem_error);
        filesystem_error.clear();
        std::filesystem::rename(temporary, path, filesystem_error);
    }
    if (filesystem_error) {
        std::filesystem::remove(temporary, filesystem_error);
        error = "Could not commit a map tile to the cache";
        return false;
    }
    return true;
}

CacheStats TileCache::stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    CacheStats result;
    std::error_code error;
    if (!std::filesystem::exists(root_, error)) return result;
    for (std::filesystem::recursive_directory_iterator iterator(root_, error), end; iterator != end && !error;
         iterator.increment(error)) {
        if (!iterator->is_regular_file(error) || iterator->path().extension() != ".png") continue;
        const auto size = iterator->file_size(error);
        if (error) break;
        ++result.tiles;
        result.bytes += size;
    }
    return result;
}

bool TileCache::clear(std::string &error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto normalized_root = root_.lexically_normal();
    if (normalized_root.filename().empty()) normalized_root = normalized_root.parent_path();
    if (normalized_root.filename() != "openstreetmap") {
        error = "Map cache path must end with openstreetmap before it can be cleared";
        return false;
    }
    std::error_code filesystem_error;
    std::filesystem::remove_all(root_, filesystem_error);
    if (filesystem_error) {
        error = "Could not clear the map cache";
        return false;
    }
    std::filesystem::create_directories(root_, filesystem_error);
    if (filesystem_error) {
        error = "Could not recreate the map cache directory";
        return false;
    }
    return true;
}

bool TileCache::valid_tile_png(std::string_view bytes) noexcept
{
    if (bytes.size() < 45U || bytes.size() > kMaximumTileBytes) return false;
    const auto *data = reinterpret_cast<const unsigned char *>(bytes.data());
    for (std::size_t index = 0; index < kPngSignature.size(); ++index) {
        if (data[index] != kPngSignature[index]) return false;
    }
    const auto end = bytes.size() - 12U;
    if (read_big_endian(data + 8) != 13U || data[12] != 'I' || data[13] != 'H' || data[14] != 'D' ||
        data[15] != 'R' || read_big_endian(data + end) != 0U || data[end + 4U] != 'I' || data[end + 5U] != 'E' ||
        data[end + 6U] != 'N' || data[end + 7U] != 'D') {
        return false;
    }
    return read_big_endian(data + 16) == MapModel::tile_size && read_big_endian(data + 20) == MapModel::tile_size;
}

std::filesystem::path TileCache::default_root()
{
    if (const char *configured = std::getenv("LILYGO_MAPS_CACHE_DIR"); configured && *configured) return configured;
    if (::geteuid() == 0) return "/root/maps/openstreetmap";
    if (const char *xdg_cache = std::getenv("XDG_CACHE_HOME"); xdg_cache && *xdg_cache) {
        return std::filesystem::path(xdg_cache) / "lilygo-ui-maps/maps/openstreetmap";
    }
    if (const char *home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".cache/lilygo-ui-maps/maps/openstreetmap";
    }
    return "/tmp/lilygo-ui-maps/maps/openstreetmap";
}

bool TileCache::valid_tile_file(const std::filesystem::path &path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size < 45U || size > kMaximumTileBytes) return false;
    std::array<char, 45> probe{};
    std::ifstream file(path, std::ios::binary);
    file.read(probe.data(), 24);
    if (file.gcount() != 24) return false;
    file.seekg(-12, std::ios::end);
    file.read(probe.data() + 33, 12);
    return file.gcount() == 12 && valid_tile_png(std::string_view(probe.data(), probe.size()));
}
