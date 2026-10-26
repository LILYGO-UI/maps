#ifndef LILYGO_UI_MAPS_SERVICES_GNSS_SERVICE_HPP
#define LILYGO_UI_MAPS_SERVICES_GNSS_SERVICE_HPP

#include "domain/map_model.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

struct GnssFix {
    GeoPoint point;
    std::uint64_t sequence = 0;
    std::chrono::steady_clock::time_point received_at;
};

class GnssService {
public:
    static constexpr auto fix_max_age = std::chrono::seconds(15);

    GnssService();
    ~GnssService();
    GnssService(const GnssService &)            = delete;
    GnssService &operator=(const GnssService &) = delete;

    void start();
    [[nodiscard]] std::optional<GnssFix> current_fix() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif
