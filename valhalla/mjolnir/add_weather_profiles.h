#pragma once

#include <boost/property_tree/ptree_fwd.hpp>

#include <filesystem>
#include <string>

namespace valhalla {
namespace mjolnir {

void ProcessWeatherTiles(const std::string& tile_dir,
                         const std::filesystem::path& weather_tile_dir,
                         const boost::property_tree::ptree& config);

} // namespace mjolnir
} // namespace valhalla
