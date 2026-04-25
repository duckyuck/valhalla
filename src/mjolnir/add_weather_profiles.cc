#include "mjolnir/add_weather_profiles.h"

#include "baldr/graphid.h"
#include "midgard/util.h"
#include "mjolnir/graphtilebuilder.h"

#include <boost/property_tree/ptree.hpp>
#include <boost/tokenizer.hpp>

#include <filesystem>
#include <fstream>
#include <future>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vj = valhalla::mjolnir;
using namespace valhalla::baldr;

namespace valhalla {
namespace mjolnir {
namespace {

struct WeatherStats {
  uint32_t precipitation_count = 0;
  uint32_t wet_road_count = 0;
  uint32_t updated_count = 0;
  uint32_t duplicate_count = 0;

  WeatherStats& operator+=(const WeatherStats& other) {
    precipitation_count += other.precipitation_count;
    wet_road_count += other.wet_road_count;
    updated_count += other.updated_count;
    duplicate_count += other.duplicate_count;
    return *this;
  }
};

struct WeatherProfiles {
  std::array<uint8_t, GraphTile::kWeatherBucketsPerWeek> precipitation;
  std::array<uint8_t, GraphTile::kWeatherBucketsPerWeek> wet_road;
};

std::array<uint8_t, GraphTile::kWeatherBucketsPerWeek>
decode_compact_weather_profile(const std::string& encoded) {
  auto decoded = valhalla::midgard::decode64(encoded);
  if (decoded.size() != GraphTile::kWeatherBucketsPerWeek) {
    throw std::runtime_error("Unexpected decoded weather profile size: " +
                             std::to_string(decoded.size()) + " bytes (expected " +
                             std::to_string(GraphTile::kWeatherBucketsPerWeek) + ")");
  }

  std::array<uint8_t, GraphTile::kWeatherBucketsPerWeek> buckets{};
  for (uint32_t i = 0; i < GraphTile::kWeatherBucketsPerWeek; ++i) {
    buckets[i] = static_cast<uint8_t>(decoded[i]);
  }
  return buckets;
}

std::unordered_map<uint32_t, WeatherProfiles>
ParseWeatherFile(const std::vector<std::string>& filenames, WeatherStats& stat) {
  typedef boost::tokenizer<boost::char_separator<char>> tokenizer;
  boost::char_separator<char> sep{","};
  std::unordered_map<uint32_t, WeatherProfiles> weather_by_edge;

  for (const auto& full_filename : filenames) {
    std::string line;
    std::ifstream file(full_filename);
    uint32_t line_num = 0;
    if (!file.is_open()) {
      LOG_ERROR("Could not open file: " + full_filename);
      continue;
    }

    while (getline(file, line) && ++line_num) {
      decltype(weather_by_edge)::iterator weather = weather_by_edge.end();
      tokenizer tok{line, sep};
      uint32_t field_num = 0;
      bool has_error = false;

      for (const auto& t : tok) {
        if (has_error) {
          break;
        }

        switch (field_num) {
          case 0: {
            try {
              auto inserted = weather_by_edge.insert(decltype(weather_by_edge)::value_type(
                  GraphId(t).id(), WeatherProfiles{}));
              weather = inserted.first;
              if (!inserted.second) {
                LOG_WARN("Duplicate GraphId in file: " + full_filename + " line number " +
                         std::to_string(line_num));
                ++stat.duplicate_count;
                has_error = true;
                weather = weather_by_edge.end();
              }
            } catch (std::exception&) {
              LOG_WARN("Invalid GraphId in file: " + full_filename + " line number " +
                       std::to_string(line_num));
              has_error = true;
            }
          } break;
          case 1: {
            try {
              weather->second.precipitation = decode_compact_weather_profile(t);
              ++stat.precipitation_count;
            } catch (std::exception& e) {
              LOG_WARN("Invalid precipitation profile in file: " + full_filename + " line number " +
                       std::to_string(line_num) + "; error='" + e.what() + "'");
              has_error = true;
            }
          } break;
          case 2: {
            try {
              weather->second.wet_road = decode_compact_weather_profile(t);
              ++stat.wet_road_count;
            } catch (std::exception& e) {
              LOG_WARN("Invalid wet-road profile in file: " + full_filename + " line number " +
                       std::to_string(line_num) + "; error='" + e.what() + "'");
              has_error = true;
            }
          } break;
          default:
            break;
        }
        ++field_num;
      }

      if (has_error && weather != weather_by_edge.end()) {
        weather_by_edge.erase(weather);
      }
    }
  }

  return weather_by_edge;
}

void UpdateTile(const std::string& tile_dir,
                const GraphId& tile_id,
                const std::unordered_map<uint32_t, WeatherProfiles>& weather_by_edge,
                WeatherStats& stat) {
  std::filesystem::path tile_path{tile_dir};
  tile_path.append(GraphTile::FileSuffix(tile_id));
  if (!std::filesystem::exists(tile_path)) {
    LOG_ERROR("No tile at " + tile_path.string());
    return;
  }

  vj::GraphTileBuilder tile_builder(tile_dir, tile_id, false);
  bool updated = false;

  for (uint32_t j = 0; j < tile_builder.header()->directededgecount(); ++j) {
    auto found = weather_by_edge.find(j);
    if (found == weather_by_edge.end()) {
      continue;
    }

    tile_builder.AddWeatherProfile(j, found->second.precipitation, found->second.wet_road);
    ++stat.updated_count;
    updated = true;
  }

  if (updated) {
    tile_builder.UpdateWeatherProfiles();
  }
}

void UpdateTiles(const std::string& tile_dir,
                 std::vector<std::pair<GraphId, std::vector<std::string>>>::const_iterator tile_start,
                 std::vector<std::pair<GraphId, std::vector<std::string>>>::const_iterator tile_end,
                 std::promise<WeatherStats>& result) {
  std::stringstream thread_name;
  thread_name << std::this_thread::get_id();

  [[maybe_unused]] size_t total = tile_end - tile_start;
  [[maybe_unused]] double count = 0;
  WeatherStats stat{};
  for (; tile_start != tile_end; ++tile_start) {
    LOG_INFO(thread_name.str() + " parsing weather data for " + std::to_string(tile_start->first));
    auto weather = ParseWeatherFile(tile_start->second, stat);
    LOG_INFO(thread_name.str() + " add weather data to " + std::to_string(tile_start->first));
    UpdateTile(tile_dir, tile_start->first, weather, stat);
    LOG_INFO(thread_name.str() + " finished " + std::to_string(tile_start->first) + "(" +
             std::to_string(++count / total * 100.0) + ")");
  }

  result.set_value(stat);
}

std::vector<std::pair<GraphId, std::vector<std::string>>>
PrepareWeatherTiles(const std::filesystem::path& weather_tile_dir) {
  std::unordered_map<GraphId, std::vector<std::string>> files_per_tile;
  for (std::filesystem::recursive_directory_iterator i(weather_tile_dir), end; i != end; ++i) {
    if (i->is_regular_file()) {
      auto file_name = i->path().string();
      auto pos = file_name.rfind(std::filesystem::path::preferred_separator);
      file_name = file_name.substr(0, file_name.find('.', pos == std::string::npos ? 0 : pos));
      try {
        auto id = GraphTile::GetTileId(file_name);
        files_per_tile[id].push_back(i->path().string());
      } catch (...) {
      }
    }
  }

  std::vector<std::pair<GraphId, std::vector<std::string>>> weather_tiles(files_per_tile.begin(),
                                                                           files_per_tile.end());
  std::random_device rd;
  std::shuffle(weather_tiles.begin(), weather_tiles.end(), std::mt19937(rd()));
  return weather_tiles;
}

} // namespace

void ProcessWeatherTiles(const std::string& tile_dir,
                         const std::filesystem::path& weather_tile_dir,
                         const boost::property_tree::ptree& config) {
  auto weather_tiles = PrepareWeatherTiles(weather_tile_dir);
  if (weather_tiles.empty()) {
    return;
  }

  auto threads = std::min<size_t>(config.get<unsigned int>("mjolnir.concurrency", 1),
                                  std::max<size_t>(1, weather_tiles.size()));
  auto chunk_size = (weather_tiles.size() + threads - 1) / threads;

  std::vector<std::thread> workers;
  std::vector<std::promise<WeatherStats>> promises(threads);
  std::vector<std::future<WeatherStats>> futures;
  workers.reserve(threads);
  futures.reserve(threads);

  for (size_t i = 0; i < threads; ++i) {
    auto begin = weather_tiles.begin() + std::min(i * chunk_size, weather_tiles.size());
    auto end = weather_tiles.begin() + std::min((i + 1) * chunk_size, weather_tiles.size());
    if (begin == end) {
      break;
    }

    futures.emplace_back(promises[i].get_future());
    workers.emplace_back(UpdateTiles, tile_dir, begin, end, std::ref(promises[i]));
  }

  for (auto& worker : workers) {
    worker.join();
  }

  WeatherStats total{};
  for (auto& future : futures) {
    total += future.get();
  }

  LOG_INFO("Processed weather profiles: precipitation=" + std::to_string(total.precipitation_count) +
           " wet_road=" + std::to_string(total.wet_road_count) +
           " updated=" + std::to_string(total.updated_count) +
           " duplicate=" + std::to_string(total.duplicate_count));
}

} // namespace mjolnir
} // namespace valhalla
