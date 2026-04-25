#include "argparse_utils.h"
#include "mjolnir/add_weather_profiles.h"

#include <boost/property_tree/ptree.hpp>
#include <cxxopts.hpp>

#include <filesystem>
#include <iostream>
#include <string>

namespace vj = valhalla::mjolnir;

int main(int argc, char** argv) {
  const auto program = std::filesystem::path(__FILE__).stem().string();
  std::filesystem::path weather_tile_dir;
  boost::property_tree::ptree config;
  try {
    cxxopts::Options options(
        program, program + " " + VALHALLA_PRINT_VERSION + "\n\nadds weather profiles to valhalla tiles.\n");
    options.add_options()("h,help", "Print this help message.")
        ("v,version", "Print the version of this software.")
        ("j,concurrency", "Number of threads to use.", cxxopts::value<unsigned int>())
        ("c,config", "Path to the json configuration file.", cxxopts::value<std::string>())
        ("i,inline-config", "Inline json config.", cxxopts::value<std::string>())
        ("t,weather-tile-dir", "positional argument", cxxopts::value<std::string>());
    options.parse_positional({"weather-tile-dir"});
    options.positional_help("Weather tile dir");
    auto result = options.parse(argc, argv);
    if (!parse_common_args(program, options, result, &config, true)) {
      return EXIT_SUCCESS;
    }
    if (!result.count("weather-tile-dir")) {
      std::cout << "You must provide a tile directory to read the csv tiles from.\n";
      return EXIT_SUCCESS;
    }
    weather_tile_dir = std::filesystem::path(result["weather-tile-dir"].as<std::string>());
  } catch (cxxopts::exceptions::exception& e) {
    std::cerr << e.what() << std::endl;
    return EXIT_FAILURE;
  } catch (std::exception& e) {
    std::cerr << "Unable to parse command line options because: " << e.what() << "\n"
              << "This is a bug, please report it at " PACKAGE_BUGREPORT << "\n";
    return EXIT_FAILURE;
  }

  auto tile_dir = config.get<std::string>("mjolnir.tile_dir");
  vj::ProcessWeatherTiles(tile_dir, weather_tile_dir, config);
  return EXIT_SUCCESS;
}
