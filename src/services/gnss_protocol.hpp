#ifndef LILYGO_UI_MAPS_SERVICES_GNSS_PROTOCOL_HPP
#define LILYGO_UI_MAPS_SERVICES_GNSS_PROTOCOL_HPP

#include "domain/map_model.hpp"

#include <string>
#include <vector>

namespace gnss_protocol {

enum class PositionKind { Ignored, Fix, Invalid };

struct Position {
    PositionKind kind = PositionKind::Ignored;
    GeoPoint point;
};

// A malformed sentence cannot refresh a fix. A valid no-fix sentence invalidates it.
[[nodiscard]] Position parse_position(const std::string &line);
[[nodiscard]] std::vector<int> integer_response(const std::string &line, const std::string &prefix);

}  // namespace gnss_protocol

#endif
