#include "domain/map_model.hpp"

#include <algorithm>
#include <cmath>

namespace {

constexpr double kMaximumLatitude = 85.05112878;
constexpr double kPi              = 3.14159265358979323846;

double map_size(int zoom) noexcept
{
    return static_cast<double>(MapModel::tile_size) * static_cast<double>(1U << zoom);
}

double wrap_world_x(double value, double size) noexcept
{
    value = std::fmod(value, size);
    return value < 0.0 ? value + size : value;
}

int positive_modulo(int value, int divisor) noexcept
{
    const int result = value % divisor;
    return result < 0 ? result + divisor : result;
}

}  // namespace

GeoPoint MapModel::center() const noexcept
{
    return center_;
}

int MapModel::zoom() const noexcept
{
    return zoom_;
}

void MapModel::center_on(GeoPoint point, int zoom) noexcept
{
    if (!std::isfinite(point.latitude) || !std::isfinite(point.longitude)) return;
    center_.latitude = std::clamp(point.latitude, -kMaximumLatitude, kMaximumLatitude);
    double longitude = std::fmod(point.longitude + 180.0, 360.0);
    if (longitude < 0.0) longitude += 360.0;
    center_.longitude = longitude - 180.0;
    zoom_             = std::clamp(zoom, minimum_zoom, maximum_zoom);
}

void MapModel::pan_pixels(double horizontal, double vertical) noexcept
{
    const double size    = map_size(zoom_);
    const double world_x = wrap_world_x(longitude_to_world_x(center_.longitude, zoom_) + horizontal, size);
    const double world_y = std::clamp(latitude_to_world_y(center_.latitude, zoom_) + vertical, 0.0, size - 1.0);
    center_              = {world_y_to_latitude(world_y, zoom_), world_x_to_longitude(world_x, zoom_)};
}

void MapModel::zoom_in() noexcept
{
    zoom_ = std::min(zoom_ + 1, maximum_zoom);
}

void MapModel::zoom_out() noexcept
{
    zoom_ = std::max(zoom_ - 1, minimum_zoom);
}

std::vector<VisibleTile> MapModel::visible_tiles(int viewport_width, int viewport_height, int margin_tiles) const
{
    if (viewport_width <= 0 || viewport_height <= 0) return {};

    margin_tiles             = std::max(0, margin_tiles);
    const int tiles_per_axis = 1 << zoom_;
    const double center_x    = longitude_to_world_x(center_.longitude, zoom_);
    const double center_y    = latitude_to_world_y(center_.latitude, zoom_);
    const double origin_x    = center_x - static_cast<double>(viewport_width) / 2.0;
    const double origin_y    = center_y - static_cast<double>(viewport_height) / 2.0;
    const int first_x        = static_cast<int>(std::floor(origin_x / tile_size)) - margin_tiles;
    const int last_x = static_cast<int>(std::floor((origin_x + viewport_width - 1.0) / tile_size)) + margin_tiles;
    const int first_y = std::max(0, static_cast<int>(std::floor(origin_y / tile_size)) - margin_tiles);
    const int last_y  = std::min(tiles_per_axis - 1,
                                static_cast<int>(std::floor((origin_y + viewport_height - 1.0) / tile_size)) +
                                    margin_tiles);

    std::vector<VisibleTile> result;
    result.reserve(static_cast<std::size_t>(last_x - first_x + 1) * static_cast<std::size_t>(last_y - first_y + 1));
    for (int raw_y = first_y; raw_y <= last_y; ++raw_y) {
        for (int raw_x = first_x; raw_x <= last_x; ++raw_x) {
            result.push_back({{zoom_, positive_modulo(raw_x, tiles_per_axis), raw_y},
                              static_cast<int>(std::lround(raw_x * tile_size - origin_x)),
                              static_cast<int>(std::lround(raw_y * tile_size - origin_y))});
        }
    }
    return result;
}

double MapModel::longitude_to_world_x(double longitude, int zoom) noexcept
{
    return (longitude + 180.0) / 360.0 * map_size(zoom);
}

double MapModel::latitude_to_world_y(double latitude, int zoom) noexcept
{
    latitude           = std::clamp(latitude, -kMaximumLatitude, kMaximumLatitude);
    const double angle = latitude * kPi / 180.0;
    return (1.0 - std::asinh(std::tan(angle)) / kPi) * 0.5 * map_size(zoom);
}

double MapModel::world_x_to_longitude(double world_x, int zoom) noexcept
{
    return wrap_world_x(world_x, map_size(zoom)) / map_size(zoom) * 360.0 - 180.0;
}

double MapModel::world_y_to_latitude(double world_y, int zoom) noexcept
{
    const double normalized = 1.0 - 2.0 * std::clamp(world_y / map_size(zoom), 0.0, 1.0);
    return std::atan(std::sinh(kPi * normalized)) * 180.0 / kPi;
}
