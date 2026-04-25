#include "baldr/rapidjson_utils.h"
#include "gurka.h"
#include "midgard/encoded.h"
#include "test.h"

#include <gtest/gtest.h>

using namespace valhalla;

/*************************************************************/
TEST(Standalone, SacScaleAttributes) {

  const std::string ascii_map = R"(
      1
    A---2B-3-4C
              |
              |5
              D
         )";

  const gurka::ways ways = {{"AB", {{"highway", "track"}, {"sac_scale", "hiking"}}},
                            {"BC", {{"highway", "track"}, {"sac_scale", "alpine_hiking"}}},
                            {"CD", {{"highway", "track"}}}};

  const double gridsize = 10;
  const auto layout = gurka::detail::map_to_coordinates(ascii_map, gridsize);
  auto map = gurka::buildtiles(layout, ways, {}, {}, "test/data/sac_scale_attributes");

  std::string trace_json;
  [[maybe_unused]] auto api =
      gurka::do_action(valhalla::Options::trace_attributes, map, {"1", "2", "3", "4", "5"},
                       "pedestrian", {{"/costing_options/pedestrian/max_hiking_difficulty", "5"}}, {},
                       &trace_json, "via");

  rapidjson::Document result;
  result.Parse(trace_json.c_str());

  auto edges = result["edges"].GetArray();
  ASSERT_EQ(edges.Size(), 3);

  EXPECT_TRUE(edges[0].HasMember("sac_scale"));
  EXPECT_EQ(edges[0]["sac_scale"].GetInt(), 1);
  EXPECT_TRUE(edges[1].HasMember("sac_scale"));
  EXPECT_EQ(edges[1]["sac_scale"].GetInt(), 4);
  EXPECT_TRUE(edges[2].HasMember("sac_scale"));
  EXPECT_EQ(edges[2]["sac_scale"].GetInt(), 0);
}

TEST(Standalone, ShoulderAttributes) {

  const std::string ascii_map = R"(
      1
    A---2B-3-4C)";

  const gurka::ways ways = {{"AB", {{"highway", "primary"}, {"shoulder", "both"}}},
                            {"BC", {{"highway", "primary"}}}};

  const double gridsize = 10;
  const auto layout = gurka::detail::map_to_coordinates(ascii_map, gridsize);
  auto map = gurka::buildtiles(layout, ways, {}, {}, "test/data/shoulder_attributes");

  std::string trace_json;
  [[maybe_unused]] auto api =
      gurka::do_action(valhalla::Options::trace_attributes, map, {"1", "2", "3", "4"}, "bicycle", {},
                       {}, &trace_json, "via");

  rapidjson::Document result;
  result.Parse(trace_json.c_str());

  auto edges = result["edges"].GetArray();
  ASSERT_EQ(edges.Size(), 2);
  EXPECT_TRUE(edges[0].HasMember("shoulder"));
  EXPECT_TRUE(edges[0]["shoulder"].GetBool());
  EXPECT_TRUE(edges[1].HasMember("shoulder"));
  EXPECT_FALSE(edges[1]["shoulder"].GetBool());
}

TEST(Standalone, InterpolatedPoints) {
  const std::string ascii_map = R"(
         3
    A--12B4--56C)";

  const gurka::ways ways = {{"AB", {{"highway", "primary"}}}, {"BC", {{"highway", "secondary"}}}};

  const double gridsize = 2;
  const auto layout = gurka::detail::map_to_coordinates(ascii_map, gridsize);
  auto map = gurka::buildtiles(layout, ways, {}, {}, "test/data/shoulder_attributes");

  std::string trace_json;
  [[maybe_unused]] auto api =
      gurka::do_action(valhalla::Options::trace_attributes, map, {"1", "2", "3", "4", "5", "6"},
                       "bicycle", {}, {}, &trace_json, "via");

  // confirm one of the interpolated points has the right edge index
  rapidjson::Document result_doc;
  result_doc.Parse(trace_json);
  ASSERT_EQ(result_doc["matched_points"].GetArray().Size(), 6);
  ASSERT_EQ(result_doc["edges"].GetArray().Size(), 2);

  // we have all the right points set as interpolated & matched
  const std::unordered_map<std::string, std::vector<int>> wp_pairs{{"matched", {0, 4, 5}},
                                                                   {"interpolated", {1, 2, 3}}};
  for (const auto& wp_pair : wp_pairs) {
    for (const auto& wp : wp_pair.second) {
      ASSERT_EQ(static_cast<std::string>(result_doc["matched_points"][wp]["type"].GetString()),
                wp_pair.first);
    }
  }

  // make sure the relation of points to edge is correct
  ASSERT_EQ(result_doc["matched_points"][0]["edge_index"].GetInt(), 0);
  ASSERT_EQ(result_doc["matched_points"][1]["edge_index"].GetInt(), 0);

  // since WP 3 projects on the last edge, it should have distance_along_edge = 0
  ASSERT_EQ(result_doc["matched_points"][2]["edge_index"].GetInt(), 1);
  ASSERT_EQ(result_doc["matched_points"][2]["distance_along_edge"].GetFloat(), 0.f);

  ASSERT_EQ(result_doc["matched_points"][3]["edge_index"].GetInt(), 1);
  ASSERT_EQ(result_doc["matched_points"][4]["edge_index"].GetInt(), 1);
  ASSERT_EQ(result_doc["matched_points"][5]["edge_index"].GetInt(), 1);
}

TEST(Standalone, RetrieveNodeTrafficSignal) {
  const std::string ascii_map = R"(
    A---B---C
        |
        D
  )";

  const gurka::ways ways = {{"AB", {{"highway", "primary"}}},
                            {"BC", {{"highway", "primary"}}},
                            {"BD", {{"highway", "primary"}}}};

  const gurka::nodes nodes = {{"B", {{"highway", "traffic_signals"}}}};

  const double gridsize = 10;
  const auto layout = gurka::detail::map_to_coordinates(ascii_map, gridsize);
  auto map = gurka::buildtiles(layout, ways, nodes, {}, "test/data/traffic_signal_node_attributes");

  std::string trace_json;
  [[maybe_unused]] auto api = gurka::do_action(valhalla::Options::trace_attributes, map,
                                               {"A", "B", "C"}, "auto", {}, {}, &trace_json, "via");

  rapidjson::Document result;
  result.Parse(trace_json.c_str());

  auto edges = result["edges"].GetArray();
  ASSERT_EQ(edges.Size(), 2);

  EXPECT_TRUE(edges[0]["end_node"].HasMember("traffic_signal"));
  EXPECT_TRUE(edges[0]["end_node"]["traffic_signal"].GetBool());

  EXPECT_TRUE(edges[1]["end_node"].HasMember("traffic_signal"));
  EXPECT_FALSE(edges[1]["end_node"]["traffic_signal"].GetBool());
}

TEST(Standalone, SpeedTypes) {
  const std::string ascii_map = R"(
    A---B---C
  )";

  gurka::ways ways = {
      {"AB", {{"highway", "primary"}}},
      {"BC", {{"highway", "primary"}, {"maxspeed", "60"}}},
  };

  const double gridsize = 10;
  const auto layout = gurka::detail::map_to_coordinates(ascii_map, gridsize);
  auto map = gurka::buildtiles(layout, ways, {}, {}, "test/data/speed_types");

  std::string trace_json;
  [[maybe_unused]] auto api = gurka::do_action(valhalla::Options::trace_attributes, map,
                                               {"A", "B", "C"}, "auto", {}, {}, &trace_json, "via");

  rapidjson::Document result;
  result.Parse(trace_json.c_str());

  auto edges = result["edges"].GetArray();
  ASSERT_EQ(edges.Size(), 2);

  EXPECT_EQ(edges[0]["speed_type"].GetString(), to_string(baldr::SpeedType::kClassified));
  EXPECT_EQ(edges[1]["speed_type"].GetString(), to_string(baldr::SpeedType::kTagged));
}

TEST(Standalone, AdditionalSpeedAttributes) {
  // set all speeds in kph
  uint64_t current = 20;
  uint64_t constrained = 40;
  uint64_t free = 100;
  uint64_t predicted = 10;
  uint64_t base = 60;

  const std::string ascii_map = R"(
    A---B---C
  )";

  gurka::ways ways = {
      {"AB", {{"highway", "primary"}}},
      {"BC", {{"highway", "primary"}, {"maxspeed", std::to_string(base)}}},
  };

  // gridsize 2500 = 10km length per edge
  const double gridsize = 2500;
  const auto layout = gurka::detail::map_to_coordinates(ascii_map, gridsize);
  auto map = gurka::buildtiles(layout, ways, {}, {}, "test/data/speed_attributes");
  map.config.put("mjolnir.traffic_extract", "test/data/speed_attributes/traffic.tar");

  // add live traffic
  test::build_live_traffic_data(map.config);
  test::customize_live_traffic_data(map.config, [&](baldr::GraphReader&, baldr::TrafficTile&, int,
                                                    valhalla::baldr::TrafficSpeed* traffic_speed) {
    traffic_speed->overall_encoded_speed = current >> 1;
    traffic_speed->encoded_speed1 = current >> 1;
    traffic_speed->breakpoint1 = 255;
  });
  // set all historical speed buckets to 10 to simulate uniform historical traffic speeds for testing.
  test::customize_historical_traffic(map.config, [&](baldr::DirectedEdge& e) {
    e.set_constrained_flow_speed(constrained);
    e.set_free_flow_speed(free);

    // speeds for every 5 min bucket of the week
    std::array<float, baldr::kBucketsPerWeek> historical;
    historical.fill(predicted);
    return historical;
  });

  std::string trace_json;
  [[maybe_unused]] auto api =
      gurka::do_action(valhalla::Options::trace_attributes, map, {"A", "B", "C"}, "auto",
                       {{"/shape_match", "edge_walk"},
                        {"/date_time/type", "0"},
                        {"/date_time/value", "current"},
                        {"/costing_options/auto/speed_types/0", "current"},
                        {"/costing_options/auto/speed_types/1", "predicted"},
                        {"/trace_options/breakage_distance", "10000"}},
                       {}, &trace_json, "via");
  rapidjson::Document result;
  result.Parse(trace_json.c_str());
  auto edges = result["edges"].GetArray();
  ASSERT_EQ(edges.Size(), 2);

  EXPECT_EQ(edges[0]["speeds_non_faded"]["current_flow"].GetInt(), current);
  EXPECT_EQ(edges[0]["speeds_non_faded"]["constrained_flow"].GetInt(), constrained);
  EXPECT_EQ(edges[0]["speeds_non_faded"]["free_flow"].GetInt(), free);
  EXPECT_EQ(edges[0]["speeds_non_faded"]["predicted_flow"].GetInt(), predicted);
  EXPECT_EQ(edges[0]["speeds_non_faded"]["no_flow"].GetInt(),
            75); // speed is 75 because its inferred by the primary road class

  EXPECT_EQ(edges[0]["speeds_faded"]["current_flow"].GetInt(), current);
  EXPECT_EQ(edges[0]["speeds_faded"]["constrained_flow"].GetInt(), current);
  EXPECT_EQ(edges[0]["speeds_faded"]["free_flow"].GetInt(), current);
  EXPECT_EQ(edges[0]["speeds_faded"]["predicted_flow"].GetInt(), current);
  EXPECT_EQ(edges[0]["speeds_faded"]["no_flow"].GetInt(), current);

  EXPECT_EQ(edges[1]["speeds_non_faded"]["current_flow"].GetInt(), current);
  EXPECT_EQ(edges[1]["speeds_non_faded"]["constrained_flow"].GetInt(), constrained);
  EXPECT_EQ(edges[1]["speeds_non_faded"]["free_flow"].GetInt(), free);
  EXPECT_EQ(edges[1]["speeds_non_faded"]["predicted_flow"].GetInt(), predicted);
  EXPECT_EQ(edges[1]["speeds_non_faded"]["no_flow"].GetInt(), base);

  // current_flow fades to predicted flow because its next up from the speed types in the request
  float multiplier = 480.f / 3600.f;
  float multiplier_inverse = 1.f - multiplier;
  EXPECT_NEAR(edges[1]["speeds_faded"]["current_flow"].GetInt(),
              current * multiplier_inverse + predicted * multiplier, 1);
  EXPECT_NEAR(edges[1]["speeds_faded"]["constrained_flow"].GetInt(),
              current * multiplier_inverse + constrained * multiplier, 1);
  EXPECT_NEAR(edges[1]["speeds_faded"]["free_flow"].GetInt(),
              current * multiplier_inverse + free * multiplier, 1);
  EXPECT_NEAR(edges[1]["speeds_faded"]["predicted_flow"].GetInt(),
              current * multiplier_inverse + predicted * multiplier, 1);
  EXPECT_NEAR(edges[1]["speeds_faded"]["no_flow"].GetInt(),
              current * multiplier_inverse + base * multiplier, 1);

  api = gurka::do_action(valhalla::Options::trace_attributes, map, {"A", "B", "C"}, "auto",
                         {{"/shape_match", "edge_walk"},
                          {"/date_time/type", "1"},
                          {"/date_time/value", "2025-06-27T08:00"},
                          {"/trace_options/breakage_distance", "10000"}},
                         {}, &trace_json, "via");
  result.Parse(trace_json.c_str());
  edges = result["edges"].GetArray();

  for (rapidjson::SizeType i = 0; i < edges.Size(); i++) {
    EXPECT_FALSE(edges[i].HasMember("speeds_faded"));
    EXPECT_TRUE(edges[i]["speeds_non_faded"].HasMember("current_flow"));
    EXPECT_TRUE(edges[i]["speeds_non_faded"].HasMember("predicted_flow"));
    EXPECT_TRUE(edges[i]["speeds_non_faded"].HasMember("constrained_flow"));
    EXPECT_TRUE(edges[i]["speeds_non_faded"].HasMember("free_flow"));
    EXPECT_TRUE(edges[i]["speeds_non_faded"].HasMember("no_flow"));
  }

  api =
      gurka::do_action(valhalla::Options::trace_attributes, map, {"A", "B", "C"}, "auto",
                       {{"/shape_match", "edge_walk"}, {"/trace_options/breakage_distance", "10000"}},
                       {}, &trace_json, "via");
  result.Parse(trace_json.c_str());
  edges = result["edges"].GetArray();

  for (rapidjson::SizeType i = 0; i < edges.Size(); i++) {
    EXPECT_FALSE(edges[i].HasMember("speeds_faded"));
    EXPECT_FALSE(edges[i]["speeds_non_faded"].HasMember("predicted_flow"));
    EXPECT_TRUE(edges[i]["speeds_non_faded"].HasMember("current_flow"));
    EXPECT_TRUE(edges[i]["speeds_non_faded"].HasMember("constrained_flow"));
    EXPECT_TRUE(edges[i]["speeds_non_faded"].HasMember("free_flow"));
    EXPECT_TRUE(edges[i]["speeds_non_faded"].HasMember("no_flow"));
  }

  // reset historical traffic
  test::customize_historical_traffic(map.config, [&](baldr::DirectedEdge& e) {
    e.set_constrained_flow_speed(0);
    e.set_free_flow_speed(0);

    return std::nullopt;
  });
  // invalidate traffic speed
  test::customize_live_traffic_data(map.config, [&](baldr::GraphReader&, baldr::TrafficTile&, int,
                                                    valhalla::baldr::TrafficSpeed* traffic_speed) {
    traffic_speed->overall_encoded_speed = baldr::UNKNOWN_TRAFFIC_SPEED_RAW;
    traffic_speed->encoded_speed1 = baldr::UNKNOWN_TRAFFIC_SPEED_RAW;
    traffic_speed->breakpoint1 = 0;
  });

  // this request should not have current, predicted, constrained nor free flows
  api = gurka::do_action(valhalla::Options::trace_attributes, map, {"A", "B", "C"}, "auto",
                         {{"/shape_match", "edge_walk"},
                          {"/date_time/type", "0"},
                          {"/date_time/value", "current"},
                          {"/trace_options/breakage_distance", "10000"}},
                         {}, &trace_json, "via");

  result.Parse(trace_json.c_str());
  edges = result["edges"].GetArray();

  for (rapidjson::SizeType i = 0; i < edges.Size(); i++) {
    // no faded speeds because edges don't have traffic speed anymore
    EXPECT_FALSE(edges[i].HasMember("speeds_faded"));
    EXPECT_FALSE(edges[i]["speeds_non_faded"].HasMember("current_flow"));
    EXPECT_FALSE(edges[i]["speeds_non_faded"].HasMember("predicted_flow"));
    EXPECT_FALSE(edges[i]["speeds_non_faded"].HasMember("constrained_flow"));
    EXPECT_FALSE(edges[i]["speeds_non_faded"].HasMember("free_flow"));
  }
}

TEST(Standalone, RetrieveEdgeTrafficSignal) {
  const std::string ascii_map = R"(
    A--B--C--D
  )";

  const gurka::ways ways = {{"ABC", {{"highway", "primary"}}}, {"CD", {{"highway", "primary"}}}};

  const gurka::nodes nodes = {
      {"B", {{"highway", "traffic_signals"}, {"traffic_signals:direction", "forward"}}}};

  const double gridsize = 10;
  const auto layout = gurka::detail::map_to_coordinates(ascii_map, gridsize);
  auto map = gurka::buildtiles(layout, ways, nodes, {}, "test/data/traffic_signal_edge_attributes");

  std::string trace_json;
  [[maybe_unused]] auto api =
      gurka::do_action(valhalla::Options::trace_attributes, map, {"A", "B", "C", "D"}, "auto", {}, {},
                       &trace_json, "via");

  rapidjson::Document result;
  result.Parse(trace_json.c_str());

  auto edges = result["edges"].GetArray();
  ASSERT_EQ(edges.Size(), 2);

  EXPECT_TRUE(edges[0].HasMember("traffic_signal"));
  EXPECT_TRUE(edges[0]["traffic_signal"].GetBool());

  EXPECT_TRUE(edges[1].HasMember("traffic_signal"));
  EXPECT_FALSE(edges[1]["traffic_signal"].GetBool());
}

TEST(Standalone, ViaFerrataNoSacScale) {
  const std::string ascii_map = R"(
    A--B--C
  )";

  const gurka::ways ways = {{"AB", {{"highway", "via_ferrata"}}},
                            {"BC", {{"highway", "via_ferrata"}}}};

  const double gridsize = 10;
  const auto layout = gurka::detail::map_to_coordinates(ascii_map, gridsize);
  auto map = gurka::buildtiles(layout, ways, {}, {}, "test/data/sac_scale_attributes");

  std::string trace_json;
  [[maybe_unused]] auto api =
      gurka::do_action(valhalla::Options::trace_attributes, map, {"A", "B", "C"}, "pedestrian",
                       {{"/costing_options/pedestrian/max_hiking_difficulty", "6"}}, {}, &trace_json,
                       "via");

  rapidjson::Document result;
  result.Parse(trace_json.c_str());

  auto edges = result["edges"].GetArray();
  ASSERT_EQ(edges.Size(), 2);

  EXPECT_TRUE(edges[0].HasMember("sac_scale"));
  EXPECT_EQ(edges[0]["sac_scale"].GetInt(), 6);
  EXPECT_TRUE(edges[1].HasMember("sac_scale"));
  EXPECT_EQ(edges[1]["sac_scale"].GetInt(), 6);
}

TEST(Standalone, BeginShapeIndexAtDiscontinuity) {
  // Two not connected edges. 1245 should match both with a discontinuity between 2 and 3.
  const std::string ascii_map = R"(
                   D
                  /
    A--1--B--2---C
           F---3--G--4--H
          /
         E
  )";

  const gurka::ways ways = {
      {"ABCD", {{"highway", "primary"}}},
      {"EFGH", {{"highway", "primary"}}},
  };

  const double gridsize = 100;
  const auto layout = gurka::detail::map_to_coordinates(ascii_map, gridsize);
  auto map = gurka::buildtiles(layout, ways, {}, {}, "test/data/trace_discontinuity");

  std::string trace_json;
  [[maybe_unused]] auto api =
      gurka::do_action(valhalla::Options::trace_attributes, map, {"1", "2", "3", "4"}, "auto",
                       {{"/shape_match", "map_snap"}}, {}, &trace_json, "via");

  rapidjson::Document result;
  result.Parse(trace_json.c_str());
  ASSERT_FALSE(result.HasParseError()) << "Failed to parse trace_attributes response";
  ASSERT_TRUE(result.HasMember("edges"));
  ASSERT_TRUE(result.HasMember("shape"));
  ASSERT_TRUE(result.HasMember("matched_points"));

  const auto edges = result["edges"].GetArray();
  ASSERT_EQ(edges.Size(), 2) << "Expected 2 edges: ABCD, EFGH";
  EXPECT_STREQ(edges[0]["names"][0].GetString(), "ABCD");
  EXPECT_STREQ(edges[1]["names"][0].GetString(), "EFGH");

  const auto matched_points = result["matched_points"].GetArray();
  ASSERT_EQ(matched_points.Size(), 4);

  const uint32_t abcd_end_idx = edges[0]["end_shape_index"].GetUint();
  const uint32_t efgh_begin_idx = edges[1]["begin_shape_index"].GetUint();
  EXPECT_EQ(abcd_end_idx + 1, efgh_begin_idx) << "Discontinuity between BC and DE along the shape";

  const auto shape =
      midgard::decode<std::vector<midgard::PointLL>>(std::string(result["shape"].GetString()));
  const auto bc_match = shape[abcd_end_idx];
  const auto fg_match = shape[efgh_begin_idx];
  EXPECT_GT(bc_match.Distance(fg_match), 1000.0) << "Discontinuity is big enough";
}

TEST(Standalone, WeatherSignalsDefaultToZero) {
  const std::string ascii_map = R"(
    A---B---C
  )";

  const gurka::ways ways = {{"AB", {{"highway", "primary"}}},
                            {"BC", {{"highway", "primary"}}}};

  const double gridsize = 10;
  const auto layout = gurka::detail::map_to_coordinates(ascii_map, gridsize);
  auto map = gurka::buildtiles(layout, ways, {}, {}, "test/data/weather_trace_attributes_defaults");

  std::string trace_json;
  [[maybe_unused]] auto api = gurka::do_action(valhalla::Options::trace_attributes, map,
                                               {"A", "B", "C"}, "auto", {}, {}, &trace_json,
                                               "via");

  rapidjson::Document result;
  result.Parse(trace_json.c_str());

  auto edges = result["edges"].GetArray();
  ASSERT_EQ(edges.Size(), 2);
  for (rapidjson::SizeType i = 0; i < edges.Size(); ++i) {
    ASSERT_TRUE(edges[i].HasMember("precipitation"));
    EXPECT_DOUBLE_EQ(edges[i]["precipitation"].GetDouble(), 0.0);
    ASSERT_TRUE(edges[i].HasMember("wet_road"));
    EXPECT_DOUBLE_EQ(edges[i]["wet_road"].GetDouble(), 0.0);
  }
}

TEST(Standalone, WeatherSignalsPbfOut) {
  const std::string ascii_map = R"(
      1     2
    A----B------C
  )";

  const gurka::ways ways = {{"AB", {{"highway", "primary"}}},
                            {"BC", {{"highway", "primary"}}}};

  const double gridsize = 10;
  const auto layout = gurka::detail::map_to_coordinates(ascii_map, gridsize);
  auto map = gurka::buildtiles(layout, ways, {}, {}, "test/data/weather_trace_attributes_pbf");

  std::string trace_result;
  [[maybe_unused]] auto api = gurka::do_action(valhalla::Options::trace_attributes, map,
                                               {"1", "2"}, "auto", {{"/format", "pbf"}}, {},
                                               &trace_result);

  Api initial_response;
  ASSERT_TRUE(initial_response.ParseFromString(trace_result));
  ASSERT_TRUE(initial_response.has_trip());
  ASSERT_EQ(initial_response.trip().routes_size(), 1);
  ASSERT_EQ(initial_response.trip().routes(0).legs_size(), 1);

  const auto& initial_leg = initial_response.trip().routes(0).legs(0);
  ASSERT_EQ(initial_leg.node_size(), 3);
  ASSERT_TRUE(initial_leg.node(0).has_edge());
  ASSERT_TRUE(initial_leg.node(1).has_edge());

  const auto first_edge_id = initial_leg.node(0).edge().id();
  const auto second_edge_id = initial_leg.node(1).edge().id();

  // Test values must stay within the per-signal caps (precipitation 5.0,
  // wet_road 0.5) or lookup saturates. The quantized round-trip is lossy to
  // within one uint8 step (precip ~0.02, wet_road ~0.002), so assertions use
  // EXPECT_NEAR rather than EXPECT_FLOAT_EQ.
  test::customize_weather_profiles(
      map.config, [&](const baldr::GraphId& edge_id, baldr::DirectedEdge&) -> std::optional<test::EdgeWeather> {
        if (edge_id.value == first_edge_id) {
          return test::EdgeWeather{1.5f, 0.25f};
        }
        if (edge_id.value == second_edge_id) {
          return test::EdgeWeather{3.25f, 0.4f};
        }
        return std::nullopt;
      });

  auto reader = test::make_clean_graphreader(map.config.get_child("mjolnir"));
  std::vector<float> precipitation_values;
  std::vector<float> wet_road_values;
  for (const auto& tile_id : reader->GetTileSet()) {
    auto tile = reader->GetGraphTile(tile_id);
    for (uint32_t i = 0; i < tile->header()->directededgecount(); ++i) {
      const auto* edge = tile->directededge(i);
      if (!edge->forward()) {
        continue;
      }
      precipitation_values.push_back(tile->precipitation(edge));
      wet_road_values.push_back(tile->wet_road(edge));
    }
  }
  auto near = [](float expected, float tolerance) {
    return ::testing::Truly([expected, tolerance](float v) {
      return std::fabs(v - expected) <= tolerance;
    });
  };
  EXPECT_THAT(precipitation_values, ::testing::Contains(near(1.5f, 0.02f)));
  EXPECT_THAT(precipitation_values, ::testing::Contains(near(3.25f, 0.02f)));
  EXPECT_THAT(wet_road_values, ::testing::Contains(near(0.25f, 0.01f)));
  EXPECT_THAT(wet_road_values, ::testing::Contains(near(0.4f, 0.01f)));

  api = gurka::do_action(valhalla::Options::trace_attributes, map, {"1", "2"}, "auto",
                         {{"/format", "pbf"}}, {}, &trace_result);

  Api response;
  ASSERT_TRUE(response.ParseFromString(trace_result));
  ASSERT_TRUE(response.has_trip());
  ASSERT_EQ(response.trip().routes_size(), 1);
  ASSERT_EQ(response.trip().routes(0).legs_size(), 1);

  const auto& leg = response.trip().routes(0).legs(0);
  ASSERT_EQ(leg.node_size(), 3);

  ASSERT_TRUE(leg.node(0).has_edge());
  EXPECT_NEAR(leg.node(0).edge().precipitation(), 1.5f, 0.02f);
  EXPECT_NEAR(leg.node(0).edge().wet_road(), 0.25f, 0.01f);

  ASSERT_TRUE(leg.node(1).has_edge());
  EXPECT_NEAR(leg.node(1).edge().precipitation(), 3.25f, 0.02f);
  EXPECT_NEAR(leg.node(1).edge().wet_road(), 0.4f, 0.01f);
}

TEST(Standalone, WeatherSignalsRewritePreservesPredictedSpeeds) {
  const std::string ascii_map = R"(
    A---B---C
  )";

  const gurka::ways ways = {{"AB", {{"highway", "primary"}}},
                            {"BC", {{"highway", "primary"}}}};

  const double gridsize = 2500;
  const auto layout = gurka::detail::map_to_coordinates(ascii_map, gridsize);
  auto map = gurka::buildtiles(layout, ways, {}, {},
                               "test/data/weather_trace_attributes_with_predicted_speeds");

  std::string trace_result;
  [[maybe_unused]] auto api = gurka::do_action(valhalla::Options::trace_attributes, map,
                                               {"A", "B", "C"}, "auto",
                                               {{"/shape_match", "edge_walk"},
                                                {"/trace_options/breakage_distance", "10000"}},
                                               {}, &trace_result, "via");

  rapidjson::Document initial_result;
  initial_result.Parse(trace_result.c_str());
  ASSERT_FALSE(initial_result.HasParseError());

  auto initial_edges = initial_result["edges"].GetArray();
  ASSERT_EQ(initial_edges.Size(), 2);
  const auto first_edge_id = initial_edges[0]["id"].GetUint64();
  const auto second_edge_id = initial_edges[1]["id"].GetUint64();

  test::customize_weather_profiles(
      map.config, [&](const baldr::GraphId& edge_id, baldr::DirectedEdge&) -> std::optional<test::EdgeWeather> {
        if (edge_id.value == first_edge_id) {
          return test::EdgeWeather{1.0f, 0.1f};
        }
        if (edge_id.value == second_edge_id) {
          return test::EdgeWeather{2.0f, 0.2f};
        }
        return std::nullopt;
      });

  test::customize_historical_traffic(map.config, [&](baldr::DirectedEdge& e) {
    e.set_constrained_flow_speed(40);
    e.set_free_flow_speed(100);

    std::array<float, baldr::kBucketsPerWeek> historical;
    historical.fill(e.forward() ? 25.0f : 35.0f);
    return historical;
  });

  // wet_road values must stay within the 0.5 cap to avoid quantization
  // saturation; precipitation tolerance accounts for the 5/255 uint8 step.
  test::customize_weather_profiles(
      map.config, [&](const baldr::GraphId& edge_id, baldr::DirectedEdge&) -> std::optional<test::EdgeWeather> {
        if (edge_id.value == first_edge_id) {
          return test::EdgeWeather{1.5f, 0.25f};
        }
        if (edge_id.value == second_edge_id) {
          return test::EdgeWeather{3.25f, 0.4f};
        }
        return std::nullopt;
      });

  api = gurka::do_action(valhalla::Options::trace_attributes, map, {"A", "B", "C"}, "auto",
                         {{"/shape_match", "edge_walk"},
                          {"/date_time/type", "1"},
                          {"/date_time/value", "2025-06-27T08:00"},
                          {"/trace_options/breakage_distance", "10000"}},
                         {}, &trace_result, "via");

  rapidjson::Document result;
  result.Parse(trace_result.c_str());
  ASSERT_FALSE(result.HasParseError());

  auto edges = result["edges"].GetArray();
  ASSERT_EQ(edges.Size(), 2);

  EXPECT_NEAR(edges[0]["precipitation"].GetFloat(), 1.5f, 0.02f);
  EXPECT_NEAR(edges[0]["wet_road"].GetFloat(), 0.25f, 0.01f);
  EXPECT_NEAR(edges[1]["precipitation"].GetFloat(), 3.25f, 0.02f);
  EXPECT_NEAR(edges[1]["wet_road"].GetFloat(), 0.4f, 0.01f);

  EXPECT_TRUE(edges[0]["speeds_non_faded"].HasMember("predicted_flow"));
  EXPECT_TRUE(edges[1]["speeds_non_faded"].HasMember("predicted_flow"));
  EXPECT_NEAR(edges[0]["speeds_non_faded"]["predicted_flow"].GetFloat(), 25.0f, 1.0f);
  EXPECT_NEAR(edges[1]["speeds_non_faded"]["predicted_flow"].GetFloat(), 25.0f, 1.0f);
}

TEST(Standalone, WeatherSignalsFollowRouteTime) {
  const std::string ascii_map = R"(
    A---B---C
  )";

  const gurka::ways ways = {{"AB", {{"highway", "primary"}}},
                            {"BC", {{"highway", "primary"}}}};

  const double gridsize = 2500;
  const auto layout = gurka::detail::map_to_coordinates(ascii_map, gridsize);
  auto map = gurka::buildtiles(layout, ways, {}, {}, "test/data/weather_trace_attributes_route_time");

  std::string trace_result;
  [[maybe_unused]] auto api = gurka::do_action(valhalla::Options::trace_attributes, map,
                                               {"A", "B", "C"}, "auto",
                                               {{"/shape_match", "edge_walk"},
                                                {"/trace_options/breakage_distance", "10000"}},
                                               {}, &trace_result, "via");

  rapidjson::Document initial_result;
  initial_result.Parse(trace_result.c_str());
  ASSERT_FALSE(initial_result.HasParseError());

  auto initial_edges = initial_result["edges"].GetArray();
  ASSERT_EQ(initial_edges.Size(), 2);
  const auto first_edge_id = initial_edges[0]["id"].GetUint64();
  const auto second_edge_id = initial_edges[1]["id"].GetUint64();

  test::customize_weather_profile_buckets(
      map.config, [&](const baldr::GraphId& edge_id,
                      baldr::DirectedEdge&) -> std::optional<test::EdgeWeatherProfile> {
        test::EdgeWeatherProfile profile{};
        profile.precipitation.fill(0.f);
        profile.wet_road.fill(0.f);

        auto fill_hour = [&](uint32_t start_bucket, float precipitation, float wet_road) {
          for (uint32_t bucket = start_bucket; bucket < start_bucket + 12; ++bucket) {
            profile.precipitation[bucket] = precipitation;
            profile.wet_road[bucket] = wet_road;
          }
        };

        // Wet-road values stay within the 0.5 cap so dequantization does not
        // saturate; precipitation values are well under the 5.0 cap.
        if (edge_id.value == first_edge_id) {
          fill_hour(96, 1.5f, 0.25f);
          fill_hour(108, 3.25f, 0.4f);
          return profile;
        }

        if (edge_id.value == second_edge_id) {
          fill_hour(96, 2.0f, 0.3f);
          fill_hour(108, 4.0f, 0.45f);
          return profile;
        }

        return std::nullopt;
      });

  api = gurka::do_action(valhalla::Options::trace_attributes, map, {"A", "B", "C"}, "auto",
                         {{"/shape_match", "edge_walk"},
                          {"/date_time/type", "1"},
                          {"/date_time/value", "2025-06-22T08:00"},
                          {"/trace_options/breakage_distance", "10000"}},
                         {}, &trace_result, "via");

  rapidjson::Document morning_result;
  morning_result.Parse(trace_result.c_str());
  ASSERT_FALSE(morning_result.HasParseError());

  auto morning_edges = morning_result["edges"].GetArray();
  ASSERT_EQ(morning_edges.Size(), 2);
  EXPECT_NEAR(morning_edges[0]["precipitation"].GetFloat(), 1.5f, 0.02f);
  EXPECT_NEAR(morning_edges[0]["wet_road"].GetFloat(), 0.25f, 0.01f);
  EXPECT_NEAR(morning_edges[1]["precipitation"].GetFloat(), 2.0f, 0.02f);
  EXPECT_NEAR(morning_edges[1]["wet_road"].GetFloat(), 0.3f, 0.01f);

  api = gurka::do_action(valhalla::Options::trace_attributes, map, {"A", "B", "C"}, "auto",
                         {{"/shape_match", "edge_walk"},
                          {"/date_time/type", "1"},
                          {"/date_time/value", "2025-06-22T09:00"},
                          {"/trace_options/breakage_distance", "10000"}},
                         {}, &trace_result, "via");

  rapidjson::Document later_result;
  later_result.Parse(trace_result.c_str());
  ASSERT_FALSE(later_result.HasParseError());

  auto later_edges = later_result["edges"].GetArray();
  ASSERT_EQ(later_edges.Size(), 2);
  EXPECT_NEAR(later_edges[0]["precipitation"].GetFloat(), 3.25f, 0.02f);
  EXPECT_NEAR(later_edges[0]["wet_road"].GetFloat(), 0.4f, 0.01f);
  EXPECT_NEAR(later_edges[1]["precipitation"].GetFloat(), 4.0f, 0.02f);
  EXPECT_NEAR(later_edges[1]["wet_road"].GetFloat(), 0.45f, 0.01f);
}

TEST(Standalone, WeatherSignalsRespectTraceAttributeFilters) {
  const std::string ascii_map = R"(
    A---B---C
  )";

  const gurka::ways ways = {{"AB", {{"highway", "primary"}}},
                            {"BC", {{"highway", "primary"}}}};

  const double gridsize = 2500;
  const auto layout = gurka::detail::map_to_coordinates(ascii_map, gridsize);
  auto map = gurka::buildtiles(layout, ways, {}, {},
                               "test/data/weather_trace_attributes_filters");

  std::string trace_result;
  [[maybe_unused]] auto api = gurka::do_action(valhalla::Options::trace_attributes, map,
                                               {"A", "B", "C"}, "auto",
                                               {{"/shape_match", "edge_walk"},
                                                {"/trace_options/breakage_distance", "10000"}},
                                               {}, &trace_result, "via");

  rapidjson::Document initial_result;
  initial_result.Parse(trace_result.c_str());
  ASSERT_FALSE(initial_result.HasParseError());

  auto initial_edges = initial_result["edges"].GetArray();
  ASSERT_EQ(initial_edges.Size(), 2);
  const auto first_edge_id = initial_edges[0]["id"].GetUint64();

  test::customize_weather_profiles(
      map.config, [&](const baldr::GraphId& edge_id,
                      baldr::DirectedEdge&) -> std::optional<test::EdgeWeather> {
        if (edge_id.value == first_edge_id) {
          return test::EdgeWeather{1.5f, 0.25f};
        }
        return std::nullopt;
      });

  api = gurka::do_action(valhalla::Options::trace_attributes, map, {"A", "B", "C"}, "auto",
                         {{"/shape_match", "edge_walk"},
                          {"/trace_options/breakage_distance", "10000"}},
                         {}, &trace_result, "via");

  rapidjson::Document default_result;
  default_result.Parse(trace_result.c_str());
  ASSERT_FALSE(default_result.HasParseError());
  auto default_edges = default_result["edges"].GetArray();
  ASSERT_EQ(default_edges.Size(), 2);
  EXPECT_TRUE(default_edges[0].HasMember("precipitation"));
  EXPECT_TRUE(default_edges[0].HasMember("wet_road"));

  api = gurka::do_action(valhalla::Options::trace_attributes, map, {"A", "B", "C"}, "auto",
                         {{"/shape_match", "edge_walk"},
                          {"/trace_options/breakage_distance", "10000"},
                          {"/filters/action", "exclude"},
                          {"/filters/attributes/0", "edge.precipitation"},
                          {"/filters/attributes/1", "edge.wet_road"}},
                         {}, &trace_result, "via");

  rapidjson::Document exclude_result;
  exclude_result.Parse(trace_result.c_str());
  ASSERT_FALSE(exclude_result.HasParseError());
  auto exclude_edges = exclude_result["edges"].GetArray();
  ASSERT_EQ(exclude_edges.Size(), 2);
  EXPECT_FALSE(exclude_edges[0].HasMember("precipitation"));
  EXPECT_FALSE(exclude_edges[0].HasMember("wet_road"));

  api = gurka::do_action(valhalla::Options::trace_attributes, map, {"A", "B", "C"}, "auto",
                         {{"/shape_match", "edge_walk"},
                          {"/trace_options/breakage_distance", "10000"},
                          {"/filters/action", "include"},
                          {"/filters/attributes/0", "edge.length"}},
                         {}, &trace_result, "via");

  rapidjson::Document include_result;
  include_result.Parse(trace_result.c_str());
  ASSERT_FALSE(include_result.HasParseError());
  auto include_edges = include_result["edges"].GetArray();
  ASSERT_EQ(include_edges.Size(), 2);
  EXPECT_TRUE(include_edges[0].HasMember("length"));
  EXPECT_FALSE(include_edges[0].HasMember("precipitation"));
  EXPECT_FALSE(include_edges[0].HasMember("wet_road"));
}

TEST(Standalone, PbfOut) {
  const std::string ascii_map = R"(
      1     2
    A----B------C------D
                  3   4
  )";

  const gurka::ways ways = {{"ABC", {{"highway", "primary"}}}, {"CD", {{"highway", "primary"}}}};

  const gurka::nodes nodes = {
      {"B", {{"highway", "traffic_signals"}, {"traffic_signals:direction", "forward"}}}};

  const double gridsize = 10;
  const auto layout = gurka::detail::map_to_coordinates(ascii_map, gridsize);
  auto map = gurka::buildtiles(layout, ways, nodes, {}, "test/data/trace_attributes_pbf");

  std::string trace_result;
  [[maybe_unused]] auto api =
      gurka::do_action(valhalla::Options::trace_attributes, map, {"1", "2", "3", "4"}, "auto",
                       {{"/format", "pbf"},
                        {"/filters/action", "include"},
                        {"/filters/attributes/0", "matched.distance_from_trace_point"}},
                       {}, &trace_result);

  Api response;

  EXPECT_TRUE(response.ParseFromString(trace_result));

  EXPECT_TRUE(response.has_trip());
  EXPECT_EQ(response.trip().routes_size(), 1);
  EXPECT_EQ(response.trip().routes(0).matched_points_size(), 4);
  for (auto it = response.trip().routes(0).matched_points().begin();
       it != response.trip().routes(0).matched_points().end(); ++it) {
    EXPECT_TRUE(it->has_distance_from_trace_point());
    EXPECT_NEAR(it->distance_from_trace_point(), 10., 0.5);
  }
}
