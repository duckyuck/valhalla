#include "baldr/graphreader.h"
#include "gurka.h"
#include "test.h"

#include <gtest/gtest.h>

#include <optional>
#include <sstream>
#include <unordered_set>

using namespace valhalla;

namespace {

const std::string kWeatherMap = R"(
A----B----C
|         |
D----E----F
)";

class WeatherCosting : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    const gurka::ways ways = {{"AB", {{"highway", "primary"}, {"maxspeed", "80"}}},
                              {"BC", {{"highway", "primary"}, {"maxspeed", "80"}}},
                              {"AD", {{"highway", "primary"}, {"maxspeed", "130"}}},
                              {"DE", {{"highway", "primary"}, {"maxspeed", "130"}}},
                              {"EF", {{"highway", "primary"}, {"maxspeed", "130"}}},
                              {"FC", {{"highway", "primary"}, {"maxspeed", "130"}}}};

    const auto layout = gurka::detail::map_to_coordinates(kWeatherMap, 100);
    map_ = gurka::buildtiles(layout, ways, {}, {}, "test/data/gurka_weather_costing",
                             {{"mjolnir.concurrency", "1"}});
    map_.config.put("mjolnir.traffic_extract", "test/data/gurka_weather_costing/traffic.tar");
    test::build_live_traffic_data(map_.config);

    direct_edge_ids_ = CollectEdgeIds({{"AB", "B"}, {"BC", "C"}});

    // Keep predicted speeds present but neutral so the zero-knob baseline test is explicit about
    // weather being opt-in and not silently double-counted when time-dependent speeds are available.
    test::customize_historical_traffic(map_.config, [](baldr::DirectedEdge& edge) {
      std::array<float, baldr::kBucketsPerWeek> historical;
      historical.fill(static_cast<float>(edge.speed()));
      return historical;
    });
  }

  static std::vector<uint64_t>
  CollectEdgeIds(const std::vector<std::pair<std::string, std::string>>& segments) {
    baldr::GraphReader reader(map_.config.get_child("mjolnir"));
    std::vector<uint64_t> edge_ids;
    edge_ids.reserve(segments.size());

    for (const auto& segment : segments) {
      bool found = false;
      for (const auto& tile_id : reader.GetTileSet()) {
        auto edge = gurka::findEdge(reader, map_.nodes, segment.first, segment.second, tile_id);
        if (std::get<1>(edge) == nullptr) {
          continue;
        }

        edge_ids.push_back(std::get<0>(edge).value);
        found = true;
        break;
      }

      EXPECT_TRUE(found) << "Missing test edge for way " << segment.first << " ending at "
                         << segment.second;
    }

    return edge_ids;
  }

  static void SetDirectPathWeather(float precipitation, float wet_road) {
    const std::unordered_set<uint64_t> direct_edge_ids(direct_edge_ids_.begin(),
                                                       direct_edge_ids_.end());
    test::customize_weather_profiles(map_.config,
                                     [&](const baldr::GraphId& edge_id,
                                         baldr::DirectedEdge&) -> std::optional<test::EdgeWeather> {
                                       if (direct_edge_ids.count(edge_id.value) == 0) {
                                         return std::nullopt;
                                       }
                                       return test::EdgeWeather{precipitation, wet_road};
                                     });
  }

  static void SetAllWeather(float precipitation, float wet_road) {
    test::customize_weather_profiles(map_.config,
                                     [&](const baldr::GraphId&,
                                         baldr::DirectedEdge&) -> std::optional<test::EdgeWeather> {
                                       return test::EdgeWeather{precipitation, wet_road};
                                     });
  }

  static void SetWeatherProfileStart(uint32_t start_epoch) {
    map_.config.put("mjolnir.weather_profile.start_epoch", start_epoch);
    map_.config.put("mjolnir.weather_profile.valid_count", baldr::GraphTile::kWeatherProfileBuckets);
    map_.config.put("mjolnir.weather_profile.capacity", baldr::GraphTile::kWeatherProfileBuckets);
  }

  static void SetUniformLiveTraffic(uint8_t speed) {
    test::customize_live_traffic_data(map_.config, [&](baldr::GraphReader&, baldr::TrafficTile&, int,
                                                       baldr::TrafficSpeed* current) {
      current->breakpoint1 = 255;
      current->overall_encoded_speed = speed >> 1;
      current->encoded_speed1 = speed >> 1;
    });
  }

  static void SetSplitLiveTraffic(uint8_t direct_path_speed, uint8_t detour_speed) {
    test::customize_live_traffic_data(map_.config, [&](baldr::GraphReader&, baldr::TrafficTile&, int,
                                                       baldr::TrafficSpeed* current) {
      current->breakpoint1 = 255;
      current->overall_encoded_speed = detour_speed >> 1;
      current->encoded_speed1 = detour_speed >> 1;
    });

    const std::unordered_set<uint64_t> direct_edge_ids(direct_edge_ids_.begin(),
                                                       direct_edge_ids_.end());
    test::customize_live_traffic_data(map_.config, [&](baldr::GraphReader&, baldr::TrafficTile& tile,
                                                       int index, baldr::TrafficSpeed* current) {
      baldr::GraphId edge_id(tile.header->tile_id);
      edge_id.set_id(index);
      if (direct_edge_ids.count(edge_id.value) == 0) {
        return;
      }

      current->breakpoint1 = 255;
      current->overall_encoded_speed = direct_path_speed >> 1;
      current->encoded_speed1 = direct_path_speed >> 1;
    });
  }

  static void SetPredictedTraffic(std::optional<float> direct_path_speed) {
    test::customize_historical_traffic(map_.config, [&](baldr::DirectedEdge& edge) {
      std::array<float, baldr::kBucketsPerWeek> historical;
      historical.fill(static_cast<float>(edge.speed()));

      if (direct_path_speed && edge.speed() == 80) {
        historical.fill(*direct_path_speed);
      }

      return std::optional{historical};
    });
  }

  static valhalla::Api
  Route(const std::string& costing,
        const std::unordered_map<std::string, std::string>& request_options = {}) {
    return gurka::do_action(Options::route, map_, {"A", "C"}, costing, request_options);
  }

  static valhalla::Api RawRoute(const std::string& request_json) {
    return gurka::do_action(Options::route, map_, request_json);
  }

  static std::string ExplicitSpeedTypesRequest(const std::string& costing,
                                               const std::string& speed_types_json,
                                               const std::string& extra_costing_options_json = "",
                                               const std::string& extra_root_json = "") {
    std::stringstream request;
    request
        << R"({"locations":[{"lon":0.0,"lat":0.0,"type":"break"},{"lon":0.008983120447446022,"lat":0.0,"type":"break"}],"costing":")"
        << costing << R"(","costing_options":{")" << costing << R"(":{"speed_types":)"
        << speed_types_json;
    if (!extra_costing_options_json.empty()) {
      request << "," << extra_costing_options_json;
    }
    request << R"(}},"verbose":true)";
    if (!extra_root_json.empty()) {
      request << "," << extra_root_json;
    }
    request << R"(,"shape_match":"map_snap"})";
    return request.str();
  }

  static gurka::map map_;
  static std::vector<uint64_t> direct_edge_ids_;
};

gurka::map WeatherCosting::map_ = {};
std::vector<uint64_t> WeatherCosting::direct_edge_ids_ = {};

TEST_F(WeatherCosting, AutoAvoidPrecipitationChangesRouteChoice) {
  SetPredictedTraffic(std::nullopt);
  SetDirectPathWeather(5.0f, 0.0f);

  auto default_route = Route("auto");
  gurka::assert::raw::expect_path(default_route, {"AB", "BC"});

  auto avoid_precipitation_route =
      Route("auto", {{"/costing_options/auto/avoid_precipitation", "1.0"}});
  gurka::assert::raw::expect_path(avoid_precipitation_route, {"AD", "DE", "EF", "FC"});
}

TEST_F(WeatherCosting, AutoAvoidPrecipitationUsesImplicitDefaultWhenSpeedTypesOmitted) {
  SetPredictedTraffic(std::nullopt);
  SetDirectPathWeather(5.0f, 0.0f);

  auto route = RawRoute(
      R"({"locations":[{"lon":0.0,"lat":0.0,"type":"break"},{"lon":0.008983120447446022,"lat":0.0,"type":"break"}],"costing":"auto","costing_options":{"auto":{"avoid_precipitation":1.0}},"verbose":true,"shape_match":"map_snap"})");
  gurka::assert::raw::expect_path(route, {"AD", "DE", "EF", "FC"});
}

TEST_F(WeatherCosting, MotorcycleAvoidWetRoadsChangesRouteChoice) {
  SetPredictedTraffic(std::nullopt);
  SetDirectPathWeather(0.0f, 1.0f);

  auto default_route = Route("motorcycle");
  gurka::assert::raw::expect_path(default_route, {"AB", "BC"});

  auto avoid_wet_route =
      Route("motorcycle", {{"/costing_options/motorcycle/avoid_wet_roads", "1.0"}});
  gurka::assert::raw::expect_path(avoid_wet_route, {"AD", "DE", "EF", "FC"});
}

TEST_F(WeatherCosting, MotorcycleAvoidPrecipitationHalfSliderChangesRouteChoice) {
  SetPredictedTraffic(std::nullopt);
  SetDirectPathWeather(2.0f, 0.0f);

  auto avoid_precipitation_route =
      Route("motorcycle", {{"/costing_options/motorcycle/avoid_precipitation", "0.5"}});
  gurka::assert::raw::expect_path(avoid_precipitation_route, {"AD", "DE", "EF", "FC"});
}

TEST_F(WeatherCosting, MotorcycleAvoidWetRoadsHalfSliderChangesRouteChoice) {
  SetPredictedTraffic(std::nullopt);
  SetDirectPathWeather(0.0f, 0.15f);

  auto avoid_wet_route =
      Route("motorcycle", {{"/costing_options/motorcycle/avoid_wet_roads", "0.5"}});
  gurka::assert::raw::expect_path(avoid_wet_route, {"AD", "DE", "EF", "FC"});
}

TEST_F(WeatherCosting, MotorcycleSaturatedPrecipitationDropsBelowLowTopSpeed) {
  SetPredictedTraffic(std::nullopt);
  SetDirectPathWeather(2.0f, 0.0f);

  auto avoid_precipitation_route =
      Route("motorcycle", {{"/costing_options/motorcycle/top_speed", "10"},
                           {"/costing_options/motorcycle/avoid_precipitation", "1.0"}});
  gurka::assert::raw::expect_path(avoid_precipitation_route, {"AD", "DE", "EF", "FC"});
}

TEST_F(WeatherCosting, MotorcycleSaturatedWetRoadsDropBelowLowTopSpeed) {
  SetPredictedTraffic(std::nullopt);
  SetDirectPathWeather(0.0f, 0.3f);

  auto avoid_wet_route =
      Route("motorcycle", {{"/costing_options/motorcycle/top_speed", "10"},
                           {"/costing_options/motorcycle/avoid_wet_roads", "1.0"}});
  gurka::assert::raw::expect_path(avoid_wet_route, {"AD", "DE", "EF", "FC"});
}

TEST_F(WeatherCosting, MotorcycleWeatherAvoidanceDoesNotChangeDurationWhenPathIsUnchanged) {
  SetPredictedTraffic(std::nullopt);
  SetAllWeather(0.0f, 0.0f);

  auto baseline_route =
      Route("motorcycle", {{"/costing_options/motorcycle/avoid_wet_roads", "0.001"}});
  gurka::assert::raw::expect_path(baseline_route, {"AB", "BC"});
  const auto baseline_time = baseline_route.directions().routes(0).legs(0).summary().time();

  SetDirectPathWeather(0.0f, 0.3f);

  auto route = Route("motorcycle", {{"/costing_options/motorcycle/avoid_wet_roads", "0.001"}});
  gurka::assert::raw::expect_path(route, {"AB", "BC"});
  EXPECT_NEAR(route.directions().routes(0).legs(0).summary().time(), baseline_time, 0.01f);
}

TEST_F(WeatherCosting, MotorcycleMaxAvoidanceTreatsTinyWetnessAsHighCost) {
  SetPredictedTraffic(std::nullopt);
  SetDirectPathWeather(0.0f, 0.001f);

  auto avoid_wet_route =
      Route("motorcycle", {{"/costing_options/motorcycle/avoid_wet_roads", "1.0"}});
  gurka::assert::raw::expect_path(avoid_wet_route, {"AD", "DE", "EF", "FC"});
}

TEST_F(WeatherCosting, MotorcycleMaxAvoidanceTreatsTinyPrecipitationAsHighCost) {
  SetPredictedTraffic(std::nullopt);
  SetDirectPathWeather(0.02f, 0.0f);

  auto avoid_precipitation_route =
      Route("motorcycle", {{"/costing_options/motorcycle/avoid_precipitation", "1.0"}});
  gurka::assert::raw::expect_path(avoid_precipitation_route, {"AD", "DE", "EF", "FC"});
}

TEST_F(WeatherCosting, AutoAvoidPrecipitationDoesNotDoubleApplyWeatherPenalty) {
  SetPredictedTraffic(std::nullopt);
  SetDirectPathWeather(1.0f, 0.0f);

  auto moderate_avoidance_route =
      Route("auto", {{"/costing_options/auto/avoid_precipitation", "0.02"}});
  gurka::assert::raw::expect_path(moderate_avoidance_route, {"AB", "BC"});
}

TEST_F(WeatherCosting, AutoAvoidPrecipitationKeepsTrafficSpeedSelectionSeparate) {
  SetUniformLiveTraffic(80);
  SetPredictedTraffic(130.0f);
  SetDirectPathWeather(0.0f, 0.0f);

  auto clean_reader = test::make_clean_graphreader(map_.config.get_child("mjolnir"));
  auto route = gurka::do_action(Options::route, map_, {"A", "C"}, "auto",
                                {{"/date_time/type", "0"},
                                 {"/costing_options/auto/avoid_precipitation", "1.0"},
                                 {"/costing_options/auto/top_speed", "90"}},
                                clean_reader);
  gurka::assert::raw::expect_path(route, {"AB", "BC"});
}

TEST_F(WeatherCosting, AutoAvoidPrecipitationAppliesWithPredictedOnlySpeedTypes) {
  SetPredictedTraffic(130.0f);
  SetWeatherProfileStart(1750579200); // 2025-06-22T08:00:00Z
  SetDirectPathWeather(5.0f, 0.0f);

  auto route =
      RawRoute(ExplicitSpeedTypesRequest("auto", R"(["predicted"])", R"("avoid_precipitation":1.0)",
                                         R"("date_time":{"type":1,"value":"2025-06-22T08:00"})"));
  gurka::assert::raw::expect_path(route, {"AD", "DE", "EF", "FC"});
}

TEST_F(WeatherCosting, AutoAvoidPrecipitationAppliesWithExplicitAllSpeedTypes) {
  SetPredictedTraffic(130.0f);
  SetWeatherProfileStart(1750579200); // 2025-06-22T08:00:00Z
  SetDirectPathWeather(5.0f, 0.0f);

  auto route = RawRoute(
      ExplicitSpeedTypesRequest("auto", R"(["freeflow","constrained","predicted","current"])",
                                R"("avoid_precipitation":1.0)",
                                R"("date_time":{"type":1,"value":"2025-06-22T08:00"})"));
  gurka::assert::raw::expect_path(route, {"AD", "DE", "EF", "FC"});
}

TEST_F(WeatherCosting, AutoAvoidPrecipitationPreservesEmptySpeedTypes) {
  SetSplitLiveTraffic(10, 130);
  SetPredictedTraffic(std::nullopt);
  SetDirectPathWeather(0.0f, 0.0f);

  auto route = RawRoute(ExplicitSpeedTypesRequest("auto", R"([])", R"("avoid_precipitation":1.0)",
                                                  R"("date_time":{"type":0})"));
  gurka::assert::raw::expect_path(route, {"AB", "BC"});
}

TEST_F(WeatherCosting, ZeroWeatherKnobsPreserveBaselineWithPredictedSpeedsPresent) {
  SetPredictedTraffic(std::nullopt);
  SetDirectPathWeather(5.0f, 1.0f);

  const std::unordered_map<std::string, std::string> auto_options =
      {{"/date_time/type", "1"},
       {"/date_time/value", "2025-06-22T08:00"},
       {"/costing_options/auto/avoid_precipitation", "0.0"},
       {"/costing_options/auto/avoid_wet_roads", "0.0"}};
  auto auto_route = Route("auto", auto_options);
  gurka::assert::raw::expect_path(auto_route, {"AB", "BC"});

  const std::unordered_map<std::string, std::string> motorcycle_options =
      {{"/date_time/type", "1"},
       {"/date_time/value", "2025-06-22T08:00"},
       {"/costing_options/motorcycle/avoid_precipitation", "0.0"},
       {"/costing_options/motorcycle/avoid_wet_roads", "0.0"}};
  auto motorcycle_route = Route("motorcycle", motorcycle_options);
  gurka::assert::raw::expect_path(motorcycle_route, {"AB", "BC"});
}

} // namespace

int main(int argc, char* argv[]) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
