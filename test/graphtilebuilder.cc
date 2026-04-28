#include "mjolnir/graphtilebuilder.h"
#include "baldr/graphid.h"
#include "baldr/tilehierarchy.h"
#include "midgard/encoded.h"
#include "midgard/pointll.h"
#include "midgard/util.h"
#include "mjolnir/add_weather_profiles.h"

#include <boost/property_tree/ptree.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#if !defined(VALHALLA_SOURCE_DIR)
#define VALHALLA_SOURCE_DIR
#endif

using namespace std;
using namespace valhalla::baldr;
using namespace valhalla::midgard;
using namespace valhalla::mjolnir;

namespace {

constexpr uint32_t kWeatherProfileBuckets = GraphTile::kWeatherProfileBuckets;
constexpr float kBackendPrecipitationMax = 5.0f;
constexpr float kBackendWetRoadMax = 0.5f;

uint8_t backend_quantize_weather(float value, float max_value) {
  return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, max_value) / max_value * 255.0f));
}

class test_graph_tile_builder : public GraphTileBuilder {
public:
  using GraphTileBuilder::edge_offset_map_;
  using GraphTileBuilder::EdgeTuple;
  using GraphTileBuilder::EdgeTupleHasher;
  using GraphTileBuilder::GraphTileBuilder;
  using GraphTileBuilder::speed_profile_index_;
};

class test_predicted_speeds : public PredictedSpeeds {
public:
  using PredictedSpeeds::offset_;
  using PredictedSpeeds::profiles_;
};

class test_graph_tile : public GraphTile {
public:
  using GraphTile::GraphTile;
  using GraphTile::predictedspeeds_;

  test_graph_tile(const std::string& tile_dir, const GraphId& graphid)
      : GraphTile(tile_dir, graphid) {
  }

  // Get offset for an edge
  uint32_t get_speed_profile_offset(uint32_t edge_idx) const {
    const test_predicted_speeds& test_predictedspeeds_ =
        reinterpret_cast<const test_predicted_speeds&>(predictedspeeds_);

    return test_predictedspeeds_.offset_[edge_idx];
  }

  // Get the speed value from the tile's predicted speeds for edge idx and coefficient i
  float get_predicted_speed(uint32_t edge_idx, uint32_t seconds_of_week) const {
    return predictedspeeds_.speed(edge_idx, seconds_of_week);
  }

  float get_precipitation(uint32_t edge_idx, uint64_t epoch_seconds) const {
    return precipitation(edge_idx, epoch_seconds);
  }

  float get_wet_road(uint32_t edge_idx, uint64_t epoch_seconds) const {
    return wet_road(edge_idx, epoch_seconds);
  }
};

void add_weather_test_edges(test_graph_tile_builder& tilebuilder, uint32_t edge_count) {
  for (uint32_t i = 0; i < edge_count; ++i) {
    tilebuilder.directededges().emplace_back();
    bool added = false;
    tilebuilder.AddEdgeInfo(i, GraphId(0, 2, i), GraphId(0, 2, i + 1), 1000 + i, 100.0f, i, 60,
                            std::list<PointLL>{{0, static_cast<float>(i)},
                                               {1, static_cast<float>(i)}},
                            {"edge_" + std::to_string(i)}, {}, {}, 0, added);
  }
}

std::string encode_weather_profile(
    const std::array<uint8_t, GraphTile::kWeatherProfileBuckets>& profile) {
  return encode64(std::string(reinterpret_cast<const char*>(profile.data()), profile.size()));
}

void write_weather_csv(const std::string& weather_dir,
                       const GraphId& tile_id,
                       uint8_t precipitation,
                       uint8_t wet_road) {
  std::filesystem::path csv_path{weather_dir};
  csv_path.append(GraphTile::FileSuffix(tile_id, ".csv"));
  std::filesystem::create_directories(csv_path.parent_path());

  std::array<uint8_t, GraphTile::kWeatherProfileBuckets> precipitation_profile{};
  std::array<uint8_t, GraphTile::kWeatherProfileBuckets> wet_road_profile{};
  precipitation_profile[0] = precipitation;
  wet_road_profile[1] = wet_road;

  std::ofstream csv(csv_path);
  csv << std::to_string(tile_id.level()) << "/" << std::to_string(tile_id.tileid()) << "/0,"
      << encode_weather_profile(precipitation_profile) << ","
      << encode_weather_profile(wet_road_profile) << "\n";
}

void assert_tile_equalish(const GraphTile& a,
                          const GraphTile& b,
                          size_t difference,
                          const std::array<std::vector<GraphId>, kBinCount>& bins,
                          const std::string& /*msg*/) {
  // expected size
  ASSERT_EQ(a.header()->end_offset() + difference, b.header()->end_offset());

  // check the first chunk after the header
  ASSERT_EQ(memcmp(reinterpret_cast<const char*>(a.header()) + sizeof(GraphTileHeader),
                   reinterpret_cast<const char*>(b.header()) + sizeof(GraphTileHeader),
                   (reinterpret_cast<const char*>(b.GetBin(0, 0).data()) -
                    reinterpret_cast<const char*>(b.header())) -
                       sizeof(GraphTileHeader)),
            0);

  // check the stuff after the bins
  ASSERT_EQ(memcmp(reinterpret_cast<const char*>(a.header()) + a.header()->edgeinfo_offset(),
                   reinterpret_cast<const char*>(b.header()) + b.header()->edgeinfo_offset(),
                   b.header()->end_offset() - b.header()->edgeinfo_offset()),
            0);

  // if the header is as expected
  const auto *ah = a.header(), *bh = b.header();
  if (ah->access_restriction_count() == bh->access_restriction_count() &&
      ah->admincount() == bh->admincount() &&
      ah->complex_restriction_forward_offset() + difference ==
          bh->complex_restriction_forward_offset() &&
      ah->complex_restriction_reverse_offset() + difference ==
          bh->complex_restriction_reverse_offset() &&
      ah->date_created() == bh->date_created() && ah->density() == bh->density() &&
      ah->departurecount() == bh->departurecount() &&
      ah->directededgecount() == bh->directededgecount() &&
      ah->edgeinfo_offset() + difference == bh->edgeinfo_offset() &&
      ah->exit_quality() == bh->exit_quality() && ah->graphid() == bh->graphid() &&
      ah->name_quality() == bh->name_quality() && ah->nodecount() == bh->nodecount() &&
      ah->routecount() == bh->routecount() && ah->signcount() == bh->signcount() &&
      ah->speed_quality() == bh->speed_quality() && ah->stopcount() == bh->stopcount() &&
      ah->textlist_offset() + difference == bh->textlist_offset() &&
      ah->schedulecount() == bh->schedulecount() && ah->version() == bh->version()) {
    // make sure the edges' shape and names match
    for (size_t i = 0; i < ah->directededgecount(); ++i) {
      auto a_info = a.edgeinfo(a.directededge(i));
      auto b_info = b.edgeinfo(b.directededge(i));
      ASSERT_EQ(a_info.encoded_shape(), b_info.encoded_shape());
      ASSERT_EQ(a_info.GetNames().size(), b_info.GetNames().size());
      for (size_t j = 0; j < a_info.GetNames().size(); ++j)
        ASSERT_EQ(a_info.GetNames()[j], b_info.GetNames()[j]);
    }
    // check that the bins contain what was just added to them
    for (size_t i = 0; i < bins.size(); ++i) {
      auto bin = b.GetBin(i % kBinsDim, i / kBinsDim);
      auto offset = bin.size() - bins[i].size();
      for (size_t j = 0; j < bins[i].size(); ++j) {
        ASSERT_EQ(bin[j + offset], bins[i][j]);
      }
    }
  } else {
    FAIL() << "not equal";
  }
}

TEST(GraphTileBuilder, TestDuplicateEdgeInfo) {
  edge_tuple a = test_graph_tile_builder::EdgeTuple(0, GraphId(0, 2, 0), GraphId(0, 2, 1));
  edge_tuple b = test_graph_tile_builder::EdgeTuple(0, GraphId(0, 2, 0), GraphId(0, 2, 1));
  EXPECT_EQ(a, b);
  EXPECT_TRUE(a == b) << "Edge tuples should be equivalent";

  std::unordered_map<edge_tuple, size_t, test_graph_tile_builder::EdgeTupleHasher> m;
  m.emplace(a, 0);
  EXPECT_EQ(m.size(), 1) << "Why isnt there an item in this map";
  ASSERT_NE(m.find(a), m.end()) << "We should have been able to find the edge tuple";

  const auto success = m.emplace(b, 1);
  EXPECT_FALSE(success.second) << "Why on earth would it be found but then insert just fine";

  // load a test builder
  std::string test_dir = "test/data/builder_tiles";
  test_graph_tile_builder test(test_dir, GraphId(0, 2, 0), false);
  test.directededges().emplace_back();
  // add edge info for node 0 to node 1
  bool added = false;
  test.AddEdgeInfo(0, GraphId(0, 2, 0), GraphId(0, 2, 1), 1234, 555, 0, 120,
                   std::list<PointLL>{{0, 0}, {1, 1}}, {"einzelweg"}, {"1xyz tunnel"}, {}, 0, added);
  EXPECT_EQ(test.edge_offset_map_.size(), 1) << "There should be exactly two of these in here";

  // add edge info for node 1 to node 0
  test.AddEdgeInfo(0, GraphId(0, 2, 1), GraphId(0, 2, 0), 1234, 555, 0, 120,
                   std::list<PointLL>{{1, 1}, {0, 0}}, {"einzelweg"}, {"1xyz tunnel"}, {}, 0, added);
  EXPECT_EQ(test.edge_offset_map_.size(), 1) << "There should still be exactly two of these in here";

  test.StoreTileData();
  test_graph_tile_builder test2(test_dir, GraphId(0, 2, 0), false);
  auto ei = test2.edgeinfo(&test2.directededge(0));
  EXPECT_NEAR(ei.mean_elevation(), 555.0f, kElevationBinSize);
  EXPECT_EQ(ei.speed_limit(), 120);

  auto n1 = ei.GetNames();
  EXPECT_EQ(n1.size(), 1);
  EXPECT_EQ(n1.at(0), "einzelweg");

  auto n2 = ei.GetNames(); // defaults to false
  EXPECT_EQ(n2.size(), 1);
  EXPECT_EQ(n2.at(0), "einzelweg");

  auto n3 = ei.GetTaggedValues();
  EXPECT_EQ(n3.size(), 1);
  EXPECT_EQ(n3.at(0), "1xyz tunnel"); // we always return the tag type in getnames

  auto names_and_types = ei.GetNamesAndTypes(false);
  EXPECT_EQ(names_and_types.size(), 1);

  auto n4 = names_and_types.at(0);
  EXPECT_EQ(std::get<0>(n4), "einzelweg");
  EXPECT_EQ(std::get<1>(n4), false);
  EXPECT_EQ(std::get<2>(n4), false);

  const auto& names_and_types_tagged = ei.GetTags();
  EXPECT_EQ(names_and_types_tagged.size(), 1);

  n4 = names_and_types.at(0);
  EXPECT_EQ(std::get<0>(n4), "einzelweg");
  EXPECT_EQ(std::get<1>(n4), false);

  names_and_types = ei.GetNamesAndTypes(); // defaults to false
  EXPECT_EQ(names_and_types.size(), 1);

  n4 = names_and_types.at(0);
  EXPECT_EQ(std::get<0>(n4), "einzelweg");
  EXPECT_EQ(std::get<1>(n4), false);

  const auto& tags = ei.GetTags();
  EXPECT_EQ(tags.size(), 1);
  EXPECT_EQ(tags.find(TaggedValue::kTunnel)->second, "xyz tunnel");

  /* Comparing similar results
   * GetNamesAndTypes -> (name, is_tagged, type)
   * GetNames(false) -> (name, is_tagged always false)
   * GetNames() -> (names) when is not tagged
   */
  names_and_types = ei.GetNamesAndTypes(false);
  auto names = ei.GetNames(false);
  auto only_names = ei.GetNames();
  /* sizes should be the same */
  EXPECT_EQ(names_and_types.size(), 1);
  EXPECT_EQ(names.size(), names_and_types.size());
  EXPECT_EQ(only_names.size(), names_and_types.size());

  for (size_t i = 0; i < names.size(); ++i) {
    /* contents (name) should be the same */
    EXPECT_EQ(std::get<0>(names_and_types[i]), names[i].first);
    EXPECT_EQ(only_names[i], names[i].first);
    /* contents (is_tagged) should be the same */
    EXPECT_EQ(std::get<1>(names_and_types[i]), false);
    EXPECT_EQ(std::get<1>(names_and_types[i]), names[i].second);
  }

  /* Comparing similar results
   * GetNamesAndTypes -> (name, is_tagged, type)
   * GetNames(false) -> (name, is_tagged)
   */
  names_and_types = ei.GetNamesAndTypes(true);
  names = ei.GetNames(true);
  EXPECT_EQ(names_and_types.size(), 2);
  EXPECT_EQ(names.size(), names_and_types.size());

  for (size_t i = 0; i < names.size(); ++i) {
    EXPECT_EQ(std::get<0>(names_and_types[i]), names[i].first);
    EXPECT_EQ(std::get<1>(names_and_types[i]), names[i].second);
  }
}

TEST(GraphTileBuilder, TestAddBins) {

  // if you update the tile format you must regenerate test tiles. after your tile format change,
  // run valhalla_build_tiles on a reasonable sized extract. when its done do the following:
  /*
    git rm -rf test/data/bin_tiles/no_bin
    for f in $(find /data/valhalla/2 -printf '%s %P\n'| sort -n | head -n 2 | awk '{print $2}'); do
      mkdir -p test/data/bin_tiles/no_bin/2/$(dirname ${f})
      cp -rp /data/valhalla/2/${f} test/data/bin_tiles/no_bin/2/${f}
    done
    git add test/data/bin_tiles/no_bin
    git status
   */
  // this will grab the 2 smallest tiles from you new tile set and make them the new test tiles
  // note the names of the new tiles and update the list with path and index in the list just below
  for (const auto& test_tile :
       std::list<std::pair<std::string, size_t>>{{"744/881.gph", 744881}, {"744/885.gph", 744885}}) {

    // load a tile
    GraphId id(test_tile.second, 2, 0);
    std::string no_bin_dir = VALHALLA_SOURCE_DIR "test/data/bin_tiles/no_bin";
    auto t = GraphTile::Create(no_bin_dir, id);
    ASSERT_TRUE(t && t->header()) << "Couldn't load test tile";

    // alter the config to point to another dir
    std::string bin_dir = "test/data/bin_tiles/bin";

    // send blank bins
    std::array<std::vector<GraphId>, kBinCount> bins;
    GraphTileBuilder::AddBins(bin_dir, t, bins);

    // check the new tile is the same as the old one
    {
      ifstream o;
      o.exceptions(std::ifstream::failbit | std::ifstream::badbit);
      o.open(VALHALLA_SOURCE_DIR "test/data/bin_tiles/no_bin/2/000/" + test_tile.first,
             std::ios::binary);
      std::string obytes((std::istreambuf_iterator<char>(o)), std::istreambuf_iterator<char>());
      ifstream n;
      n.exceptions(std::ifstream::failbit | std::ifstream::badbit);
      n.open("test/data/bin_tiles/bin/2/000/" + test_tile.first, std::ios::binary);
      std::string nbytes((std::istreambuf_iterator<char>(n)), std::istreambuf_iterator<char>());
      EXPECT_EQ(obytes, nbytes) << "Old tile and new tile should be the same if not adding any bins";
    }

    // send fake bins, we'll throw one in each bin
    for (auto& bin : bins)
      bin.emplace_back(test_tile.second, 2, 0);
    GraphTileBuilder::AddBins(bin_dir, t, bins);
    auto increase = bins.size() * sizeof(GraphId);

    // check the new tile isnt broken and is exactly the right size bigger
    assert_tile_equalish(*t, *GraphTile::Create(bin_dir, id), increase, bins,
                         "New tiles edgeinfo or names arent matching up: 1");

    // append some more
    for (auto& bin : bins)
      bin.emplace_back(test_tile.second, 2, 1);
    GraphTileBuilder::AddBins(bin_dir, t, bins);
    increase = bins.size() * sizeof(GraphId) * 2;

    // check the new tile isnt broken and is exactly the right size bigger
    assert_tile_equalish(*t, *GraphTile::Create(bin_dir, id), increase, bins,
                         "New tiles edgeinfo or names arent matching up: 2");

    // check that appending works
    t = GraphTile::Create(bin_dir, id);
    GraphTileBuilder::AddBins(bin_dir, t, bins);
    for (auto& bin : bins)
      bin.insert(bin.end(), bin.begin(), bin.end());

    // check the new tile isnt broken and is exactly the right size bigger
    assert_tile_equalish(*t, *GraphTile::Create(bin_dir, id), increase, bins,
                         "New tiles edgeinfo or names arent matching up: 3");
  }
}

TEST(GraphTileBuilder, TestDuplicatePredictedSpeeds) {

  // setup a tile with edges that have two edges with the same predicted speeds
  std::string test_dir = "test/data/builder_predicted_speeds";
  test_graph_tile_builder base_tilebuilder(test_dir, GraphId(0, 2, 0), false);

  // Add three edges with different edge info
  base_tilebuilder.directededges().emplace_back();
  base_tilebuilder.directededges().emplace_back();
  base_tilebuilder.directededges().emplace_back();

  bool added = false;
  base_tilebuilder.AddEdgeInfo(0, GraphId(0, 2, 0), GraphId(0, 2, 1), 1111, 100.5f, 1, 60,
                               std::list<PointLL>{{0, 0}, {1, 1}}, {"edge_one"}, {}, {}, 0, added);
  base_tilebuilder.AddEdgeInfo(1, GraphId(0, 2, 1), GraphId(0, 2, 2), 2222, 200.5f, 2, 70,
                               std::list<PointLL>{{1, 1}, {2, 2}}, {"edge_two"}, {}, {}, 0, added);
  base_tilebuilder.AddEdgeInfo(2, GraphId(0, 2, 2), GraphId(0, 2, 3), 3333, 300.5f, 3, 80,
                               std::list<PointLL>{{2, 2}, {3, 3}}, {"edge_three"}, {}, {}, 0, added);

  base_tilebuilder.StoreTileData();

  test_graph_tile_builder speeds_tilebuilder(test_dir, GraphId(0, 2, 0), true);

  // Create a constant speed array
  std::array<float, kBucketsPerWeek> speeds;
  speeds.fill(20.0f); // 20 kph for the entire week
  auto test_predicted_speed_coefficients_1 = compress_speed_buckets(speeds.data());

  speeds.fill(30.0f);
  auto test_predicted_speed_coefficients_2 = compress_speed_buckets(speeds.data());

  speeds_tilebuilder.AddPredictedSpeed(0, test_predicted_speed_coefficients_1);
  speeds_tilebuilder.AddPredictedSpeed(1, test_predicted_speed_coefficients_2);
  speeds_tilebuilder.AddPredictedSpeed(2, test_predicted_speed_coefficients_2);

  EXPECT_EQ(speeds_tilebuilder.speed_profile_index_.size(), 2);

  speeds_tilebuilder.UpdatePredictedSpeeds(speeds_tilebuilder.directededges());

  // load the tile and assert the predicted speeds and offsets are correct
  test_graph_tile test_tile(test_dir, GraphId(0, 2, 0));

  EXPECT_NEAR(test_tile.get_predicted_speed(0, 0), 20.0f, 0.1);
  EXPECT_NEAR(test_tile.get_predicted_speed(1, 0), 30.0f, 0.1);
  EXPECT_NEAR(test_tile.get_predicted_speed(2, 0), 30.0f, 0.1);

  EXPECT_NE(test_tile.get_speed_profile_offset(0), test_tile.get_speed_profile_offset(1));
  EXPECT_EQ(test_tile.get_speed_profile_offset(1), test_tile.get_speed_profile_offset(2));
}

TEST(GraphTileBuilder, TestDuplicatePredictedSpeedSmallHint) {
  constexpr uint32_t edge_count = 100;
  constexpr uint32_t unique_speeds = 50;

  std::string test_dir = "test/data/builder_predicted_speeds_rehash";
  test_graph_tile_builder base_tilebuilder(test_dir, GraphId(0, 2, 0), false);

  for (uint32_t i = 0; i < edge_count; ++i) {
    base_tilebuilder.directededges().emplace_back();
    bool added = false;
    base_tilebuilder.AddEdgeInfo(i, GraphId(0, 2, i), GraphId(0, 2, i + 1), 1000 + i, 100.0f, i, 60,
                                 std::list<PointLL>{{0, static_cast<float>(i)},
                                                    {1, static_cast<float>(i)}},
                                 {"edge_" + std::to_string(i)}, {}, {}, 0, added);
  }
  base_tilebuilder.StoreTileData();

  test_graph_tile_builder speeds_tilebuilder(test_dir, GraphId(0, 2, 0), true);

  // Generate unique_speeds distinct profiles, cycling them across all edges
  std::vector<std::array<int16_t, kCoefficientCount>> profiles(unique_speeds);
  std::array<float, kBucketsPerWeek> speeds;
  for (uint32_t i = 0; i < unique_speeds; ++i) {
    speeds.fill(10.0f + static_cast<float>(i));
    profiles[i] = compress_speed_buckets(speeds.data());
  }

  for (uint32_t i = 0; i < edge_count; ++i) {
    speeds_tilebuilder.AddPredictedSpeed(i, profiles[i % unique_speeds], 1);
  }

  EXPECT_EQ(speeds_tilebuilder.speed_profile_index_.size(), unique_speeds);

  speeds_tilebuilder.UpdatePredictedSpeeds(speeds_tilebuilder.directededges());

  test_graph_tile test_tile(test_dir, GraphId(0, 2, 0));
  for (uint32_t i = 0; i < edge_count; ++i) {
    float expected = 10.0f + static_cast<float>(i % unique_speeds);
    EXPECT_NEAR(test_tile.get_predicted_speed(i, 0), expected, 0.5)
        << "Edge " << i << " has wrong predicted speed";
  }

  // Edges sharing the same profile must share the same offset
  for (uint32_t i = unique_speeds; i < edge_count; ++i) {
    EXPECT_EQ(test_tile.get_speed_profile_offset(i),
              test_tile.get_speed_profile_offset(i % unique_speeds))
        << "Edge " << i << " should share offset with edge " << (i % unique_speeds);
  }
}

TEST(GraphTileBuilder, TestWeatherProfilesCoexistWithPredictedSpeeds) {
  std::string test_dir = "test/data/builder_weather_profiles_with_predicted_speeds";
  std::filesystem::remove_all(test_dir);
  test_graph_tile_builder base_tilebuilder(test_dir, GraphId(0, 2, 0), false);

  base_tilebuilder.directededges().emplace_back();
  base_tilebuilder.directededges().emplace_back();

  bool added = false;
  base_tilebuilder.AddEdgeInfo(0, GraphId(0, 2, 0), GraphId(0, 2, 1), 1111, 100.5f, 1, 60,
                               std::list<PointLL>{{0, 0}, {1, 1}}, {"edge_one"}, {}, {}, 0, added);
  base_tilebuilder.AddEdgeInfo(1, GraphId(0, 2, 1), GraphId(0, 2, 2), 2222, 200.5f, 2, 70,
                               std::list<PointLL>{{1, 1}, {2, 2}}, {"edge_two"}, {}, {}, 0, added);
  base_tilebuilder.StoreTileData();

  test_graph_tile_builder tilebuilder(test_dir, GraphId(0, 2, 0), true);

  std::array<float, kBucketsPerWeek> predicted_speeds;
  predicted_speeds.fill(25.0f);
  tilebuilder.AddPredictedSpeed(0, compress_speed_buckets(predicted_speeds.data()));
  tilebuilder.AddPredictedSpeed(1, compress_speed_buckets(predicted_speeds.data()));
  tilebuilder.directededges()[0].set_has_predicted_speed(true);
  tilebuilder.directededges()[1].set_has_predicted_speed(true);
  tilebuilder.UpdatePredictedSpeeds(tilebuilder.directededges());

  std::array<float, kBucketsPerWeek> precipitation_a;
  std::array<float, kBucketsPerWeek> wet_road_a;
  precipitation_a.fill(0.f);
  wet_road_a.fill(0.f);
  precipitation_a[96] = 1.5f;
  precipitation_a[108] = 3.25f;
  wet_road_a[96] = 0.25f;
  wet_road_a[108] = 0.375f;

  std::array<float, kBucketsPerWeek> precipitation_b;
  std::array<float, kBucketsPerWeek> wet_road_b;
  precipitation_b.fill(0.f);
  wet_road_b.fill(0.f);
  precipitation_b[96] = 2.0f;
  precipitation_b[108] = 4.0f;
  wet_road_b[96] = 0.5f;
  wet_road_b[108] = 0.5f;

  test_graph_tile_builder weather_tilebuilder(test_dir, GraphId(0, 2, 0), true);
  weather_tilebuilder.SetWeatherProfileMetadata(0, GraphTile::kWeatherProfileBuckets);
  weather_tilebuilder.AddWeatherProfile(0, precipitation_a, wet_road_a);
  weather_tilebuilder.AddWeatherProfile(1, precipitation_b, wet_road_b);
  weather_tilebuilder.UpdateWeatherProfiles();

  test_graph_tile test_tile(test_dir, GraphId(0, 2, 0));

  EXPECT_NEAR(test_tile.get_predicted_speed(0, 8 * 3600), 25.0f, 0.5f);
  EXPECT_NEAR(test_tile.get_predicted_speed(1, 9 * 3600), 25.0f, 0.5f);

  EXPECT_NEAR(test_tile.get_precipitation(0, 8 * 3600), 1.5f, 0.01f);
  EXPECT_NEAR(test_tile.get_precipitation(0, 9 * 3600), 3.25f, 0.01f);
  EXPECT_NEAR(test_tile.get_wet_road(0, 8 * 3600), 0.25f, 0.01f);
  EXPECT_NEAR(test_tile.get_wet_road(0, 9 * 3600), 0.375f, 0.01f);

  EXPECT_NEAR(test_tile.get_precipitation(1, 8 * 3600), 2.0f, 0.01f);
  EXPECT_NEAR(test_tile.get_precipitation(1, 9 * 3600), 4.0f, 0.01f);
  EXPECT_NEAR(test_tile.get_wet_road(1, 8 * 3600), 0.5f, 0.01f);
  EXPECT_NEAR(test_tile.get_wet_road(1, 9 * 3600), 0.5f, 0.01f);

  // Unpopulated buckets must read back as zero — guards against a bug where
  // the hourly-bucket index is miscomputed or the profile table bleeds across
  // buckets.
  EXPECT_NEAR(test_tile.get_precipitation(0, 0), 0.0f, 0.001f);
  EXPECT_NEAR(test_tile.get_precipitation(0, 7 * 3600), 0.0f, 0.001f);
  EXPECT_NEAR(test_tile.get_precipitation(0, 10 * 3600), 0.0f, 0.001f);
  EXPECT_NEAR(test_tile.get_wet_road(0, 7 * 3600), 0.0f, 0.001f);
  EXPECT_NEAR(test_tile.get_wet_road(1, 167 * 3600), 0.0f, 0.001f);
}

TEST(GraphTileBuilder, TestWeatherProfilesUseCompactDeduplicatedHourlyPayload) {
  std::string test_dir = "test/data/builder_weather_profiles_compact";
  std::filesystem::remove_all(test_dir);
  test_graph_tile_builder base_tilebuilder(test_dir, GraphId(0, 2, 0), false);
  add_weather_test_edges(base_tilebuilder, 3);
  base_tilebuilder.StoreTileData();

  test_graph_tile base_tile(test_dir, GraphId(0, 2, 0));
  const auto base_size = base_tile.header()->end_offset();

  std::array<float, kBucketsPerWeek> precipitation_a;
  std::array<float, kBucketsPerWeek> wet_road_a;
  std::array<float, kBucketsPerWeek> precipitation_b;
  std::array<float, kBucketsPerWeek> wet_road_b;
  precipitation_a.fill(0.f);
  wet_road_a.fill(0.f);
  precipitation_b.fill(0.f);
  wet_road_b.fill(0.f);

  precipitation_a[8 * 12] = 1.5f;
  precipitation_a[9 * 12] = 3.25f;
  wet_road_a[8 * 12] = 0.25f;
  wet_road_a[9 * 12] = 0.375f;
  precipitation_b[8 * 12] = 2.0f;
  precipitation_b[9 * 12] = 4.0f;
  wet_road_b[8 * 12] = 0.5f;
  wet_road_b[9 * 12] = 0.5f;

  test_graph_tile_builder weather_tilebuilder(test_dir, GraphId(0, 2, 0), true);
  weather_tilebuilder.SetWeatherProfileMetadata(0, GraphTile::kWeatherProfileBuckets);
  weather_tilebuilder.AddWeatherProfile(0, precipitation_a, wet_road_a);
  weather_tilebuilder.AddWeatherProfile(1, precipitation_b, wet_road_b);
  weather_tilebuilder.AddWeatherProfile(2, precipitation_a, wet_road_a);
  weather_tilebuilder.UpdateWeatherProfiles();

  test_graph_tile test_tile(test_dir, GraphId(0, 2, 0));

  EXPECT_NEAR(test_tile.get_precipitation(0, 8 * 3600), 1.5f, 0.01f);
  EXPECT_NEAR(test_tile.get_precipitation(0, 9 * 3600), 3.25f, 0.01f);
  EXPECT_NEAR(test_tile.get_wet_road(0, 8 * 3600), 0.25f, 0.01f);
  EXPECT_NEAR(test_tile.get_wet_road(0, 9 * 3600), 0.375f, 0.01f);
  EXPECT_NEAR(test_tile.get_precipitation(1, 8 * 3600), 2.0f, 0.01f);
  EXPECT_NEAR(test_tile.get_precipitation(1, 9 * 3600), 4.0f, 0.01f);
  EXPECT_NEAR(test_tile.get_precipitation(2, 8 * 3600), 1.5f, 0.01f);
  EXPECT_NEAR(test_tile.get_wet_road(2, 9 * 3600), 0.375f, 0.01f);

  const auto compact_growth = test_tile.header()->end_offset() - base_size;
  const auto old_raw_growth = 3 * kBucketsPerWeek * 2 * sizeof(uint16_t);
  const auto compact_upper_bound =
      (3 * 2 * sizeof(uint32_t)) + (3 * 2 * kWeatherProfileBuckets * sizeof(uint8_t));
  EXPECT_LE(compact_growth, compact_upper_bound);
  EXPECT_LT(compact_growth, old_raw_growth / 10);
}

TEST(GraphTileBuilder, TestCompactWeatherPayloadStaysWithinSizeBudget) {
  // Regression guard for the catastrophic uint16[2016] payload size. On a
  // fixture where every edge carries a DIFFERENT profile (dedup cannot help),
  // the compact representation must still stay below one tenth of the raw
  // legacy approach and within the theoretical upper bound.
  constexpr uint32_t kEdgeCount = 50;
  const std::string test_dir = "test/data/builder_weather_size_budget";
  std::filesystem::remove_all(test_dir);
  test_graph_tile_builder base_tilebuilder(test_dir, GraphId(0, 2, 0), false);
  add_weather_test_edges(base_tilebuilder, kEdgeCount);
  base_tilebuilder.StoreTileData();
  const uint32_t base_size = test_graph_tile(test_dir, GraphId(0, 2, 0)).header()->end_offset();

  test_graph_tile_builder weather_tilebuilder(test_dir, GraphId(0, 2, 0), true);
  weather_tilebuilder.SetWeatherProfileMetadata(0, GraphTile::kWeatherProfileBuckets);
  for (uint32_t i = 0; i < kEdgeCount; ++i) {
    std::array<uint8_t, GraphTile::kWeatherProfileBuckets> precipitation{};
    std::array<uint8_t, GraphTile::kWeatherProfileBuckets> wet_road{};
    precipitation[i % GraphTile::kWeatherProfileBuckets] = static_cast<uint8_t>(50 + i);
    wet_road[(i + 7) % GraphTile::kWeatherProfileBuckets] = static_cast<uint8_t>(100 + i);
    weather_tilebuilder.AddWeatherProfile(i, precipitation, wet_road);
  }
  weather_tilebuilder.UpdateWeatherProfiles();

  test_graph_tile sized_tile(test_dir, GraphId(0, 2, 0));
  const uint32_t compact_growth = sized_tile.header()->end_offset() - base_size;

  const size_t old_raw_growth = kEdgeCount * kBucketsPerWeek * 2 * sizeof(uint16_t);
  // Upper bound accounts for the dry-profile sentinel (always present at
  // offset 0) plus one distinct profile per edge in both signals.
  const size_t compact_upper_bound =
      (kEdgeCount * 2 * sizeof(uint32_t)) +
      ((kEdgeCount + 1) * 2 * GraphTile::kWeatherProfileBuckets * sizeof(uint8_t));

  EXPECT_LE(compact_growth, compact_upper_bound)
      << "compact weather payload exceeded the theoretical upper bound";
  EXPECT_LT(compact_growth, old_raw_growth / 10)
      << "compact weather payload regressed toward raw uint16[2016] size";
}

TEST(GraphTileBuilder, TestWeatherProfilesDecodeBackendQuantizedBytes) {
  std::string test_dir = "test/data/builder_weather_profiles_backend_quantized";
  std::filesystem::remove_all(test_dir);
  test_graph_tile_builder base_tilebuilder(test_dir, GraphId(0, 2, 0), false);
  add_weather_test_edges(base_tilebuilder, 1);
  base_tilebuilder.StoreTileData();

  constexpr float precipitation_hour_8 = 1.0f;
  constexpr float precipitation_hour_9 = 4.0f;
  constexpr float wet_road_hour_8 = 0.125f;
  constexpr float wet_road_hour_9 = 0.375f;
  const auto precipitation_q8 =
      backend_quantize_weather(precipitation_hour_8, kBackendPrecipitationMax);
  const auto precipitation_q9 =
      backend_quantize_weather(precipitation_hour_9, kBackendPrecipitationMax);
  const auto wet_road_q8 = backend_quantize_weather(wet_road_hour_8, kBackendWetRoadMax);
  const auto wet_road_q9 = backend_quantize_weather(wet_road_hour_9, kBackendWetRoadMax);

  std::array<uint16_t, kBucketsPerWeek> precipitation;
  std::array<uint16_t, kBucketsPerWeek> wet_road;
  precipitation.fill(0);
  wet_road.fill(0);
  precipitation[8 * 12] = precipitation_q8;
  precipitation[9 * 12] = precipitation_q9;
  wet_road[8 * 12] = wet_road_q8;
  wet_road[9 * 12] = wet_road_q9;

  test_graph_tile_builder weather_tilebuilder(test_dir, GraphId(0, 2, 0), true);
  weather_tilebuilder.SetWeatherProfileMetadata(0, GraphTile::kWeatherProfileBuckets);
  weather_tilebuilder.AddWeatherProfile(0, precipitation, wet_road);
  weather_tilebuilder.UpdateWeatherProfiles();

  test_graph_tile test_tile(test_dir, GraphId(0, 2, 0));

  EXPECT_NEAR(test_tile.get_precipitation(0, 8 * 3600),
              precipitation_q8 / 255.0f * kBackendPrecipitationMax, 0.001f);
  EXPECT_NEAR(test_tile.get_precipitation(0, 9 * 3600),
              precipitation_q9 / 255.0f * kBackendPrecipitationMax, 0.001f);
  EXPECT_NEAR(test_tile.get_wet_road(0, 8 * 3600), wet_road_q8 / 255.0f * kBackendWetRoadMax,
              0.0001f);
  EXPECT_NEAR(test_tile.get_wet_road(0, 9 * 3600), wet_road_q9 / 255.0f * kBackendWetRoadMax,
              0.0001f);
}

TEST(GraphTileBuilder, TestCompactAddWeatherProfileOverload) {
  std::string test_dir = "test/data/builder_weather_profiles_compact_overload";
  std::filesystem::remove_all(test_dir);
  test_graph_tile_builder base_tilebuilder(test_dir, GraphId(0, 2, 0), false);
  add_weather_test_edges(base_tilebuilder, 1);
  base_tilebuilder.StoreTileData();

  // Hand-build the same hourly profile shape the backend ships: one byte per
  // hour, already quantized against the backend's caps (5.0 mm/h, 0.5 mm).
  std::array<uint8_t, GraphTile::kWeatherProfileBuckets> precipitation{};
  std::array<uint8_t, GraphTile::kWeatherProfileBuckets> wet_road{};
  precipitation[8] = 128;
  wet_road[9] = 191;

  test_graph_tile_builder weather_tilebuilder(test_dir, GraphId(0, 2, 0), true);
  weather_tilebuilder.SetWeatherProfileMetadata(0, GraphTile::kWeatherProfileBuckets);
  weather_tilebuilder.AddWeatherProfile(0, precipitation, wet_road);
  weather_tilebuilder.UpdateWeatherProfiles();

  test_graph_tile test_tile(test_dir, GraphId(0, 2, 0));

  EXPECT_NEAR(test_tile.get_precipitation(0, 8 * 3600),
              128 / 255.0f * kBackendPrecipitationMax, 0.01f);
  EXPECT_NEAR(test_tile.get_wet_road(0, 9 * 3600),
              191 / 255.0f * kBackendWetRoadMax, 0.01f);

  // Unpopulated buckets must stay zero, including the last hour of the week.
  EXPECT_NEAR(test_tile.get_precipitation(0, 7 * 3600), 0.0f, 0.001f);
  EXPECT_NEAR(test_tile.get_precipitation(0, 10 * 3600), 0.0f, 0.001f);
  EXPECT_NEAR(test_tile.get_wet_road(0, 167 * 3600), 0.0f, 0.001f);
}

TEST(GraphTileBuilder, TestWeatherProfilesUseUtcSourceAxisMetadata) {
  std::string test_dir = "test/data/builder_weather_profiles_utc_source_axis";
  std::filesystem::remove_all(test_dir);
  test_graph_tile_builder base_tilebuilder(test_dir, GraphId(0, 2, 0), false);
  add_weather_test_edges(base_tilebuilder, 1);
  base_tilebuilder.StoreTileData();

  constexpr uint32_t forecast_start_epoch = 1750579200; // 2025-06-22T08:00:00Z

  std::array<uint8_t, GraphTile::kWeatherProfileBuckets> precipitation{};
  std::array<uint8_t, GraphTile::kWeatherProfileBuckets> wet_road{};
  precipitation[0] = backend_quantize_weather(1.5f, kBackendPrecipitationMax);
  precipitation[1] = backend_quantize_weather(3.25f, kBackendPrecipitationMax);
  wet_road[0] = backend_quantize_weather(0.25f, kBackendWetRoadMax);
  wet_road[1] = backend_quantize_weather(0.4f, kBackendWetRoadMax);

  test_graph_tile_builder weather_tilebuilder(test_dir, GraphId(0, 2, 0), true);
  weather_tilebuilder.SetWeatherProfileMetadata(forecast_start_epoch, 2);
  weather_tilebuilder.AddWeatherProfile(0, precipitation, wet_road);
  weather_tilebuilder.UpdateWeatherProfiles();

  test_graph_tile test_tile(test_dir, GraphId(0, 2, 0));

  EXPECT_EQ(test_tile.header()->weather_profile_start_epoch(), forecast_start_epoch);
  EXPECT_EQ(test_tile.header()->weather_profile_valid_count(), 2);
  EXPECT_NEAR(test_tile.get_precipitation(0, forecast_start_epoch), 1.5f, 0.02f);
  EXPECT_NEAR(test_tile.get_wet_road(0, forecast_start_epoch), 0.25f, 0.01f);
  EXPECT_NEAR(test_tile.get_precipitation(0, forecast_start_epoch + 3600), 3.25f, 0.02f);
  EXPECT_NEAR(test_tile.get_wet_road(0, forecast_start_epoch + 3600), 0.4f, 0.01f);
  EXPECT_NEAR(test_tile.get_precipitation(0, forecast_start_epoch - 3600), 0.0f, 0.001f);
  EXPECT_NEAR(test_tile.get_wet_road(0, forecast_start_epoch + 7200), 0.0f, 0.001f);
}

TEST(GraphTileBuilder, TestWeatherProfilesRemainBeforePredictedSpeedTail) {
  std::string test_dir = "test/data/builder_weather_profiles_before_predicted_tail";
  std::filesystem::remove_all(test_dir);
  test_graph_tile_builder base_tilebuilder(test_dir, GraphId(0, 2, 0), false);
  add_weather_test_edges(base_tilebuilder, 2);
  base_tilebuilder.StoreTileData();

  test_graph_tile_builder speeds_tilebuilder(test_dir, GraphId(0, 2, 0), true);
  std::array<float, kBucketsPerWeek> speeds;
  speeds.fill(25.0f);
  speeds_tilebuilder.AddPredictedSpeed(0, compress_speed_buckets(speeds.data()));
  speeds.fill(35.0f);
  speeds_tilebuilder.AddPredictedSpeed(1, compress_speed_buckets(speeds.data()));
  speeds_tilebuilder.directededges()[0].set_has_predicted_speed(true);
  speeds_tilebuilder.directededges()[1].set_has_predicted_speed(true);
  speeds_tilebuilder.UpdatePredictedSpeeds(speeds_tilebuilder.directededges());

  test_graph_tile predicted_tile(test_dir, GraphId(0, 2, 0));
  const auto predicted_offset = predicted_tile.header()->predictedspeeds_offset();

  std::array<float, kBucketsPerWeek> precipitation;
  std::array<float, kBucketsPerWeek> wet_road;
  precipitation.fill(0.f);
  wet_road.fill(0.f);
  precipitation[8 * 12] = 1.5f;
  wet_road[9 * 12] = 0.375f;

  test_graph_tile_builder weather_tilebuilder(test_dir, GraphId(0, 2, 0), true);
  weather_tilebuilder.SetWeatherProfileMetadata(0, GraphTile::kWeatherProfileBuckets);
  weather_tilebuilder.AddWeatherProfile(0, precipitation, wet_road);
  weather_tilebuilder.AddWeatherProfile(1, precipitation, wet_road);
  weather_tilebuilder.UpdateWeatherProfiles();

  test_graph_tile test_tile(test_dir, GraphId(0, 2, 0));

  EXPECT_GT(test_tile.header()->predictedspeeds_offset(), predicted_offset);
  EXPECT_NEAR(test_tile.get_predicted_speed(0, 8 * 3600), 25.0f, 0.5f);
  EXPECT_NEAR(test_tile.get_predicted_speed(1, 9 * 3600), 35.0f, 0.5f);
  EXPECT_NEAR(test_tile.get_precipitation(0, 8 * 3600), 1.5f, 0.01f);
  EXPECT_NEAR(test_tile.get_wet_road(1, 9 * 3600), 0.375f, 0.01f);
}

TEST(GraphTileBuilder, TestProcessWeatherTilesKeepsWorkerPromisesAlive) {
  std::random_device random;
  const auto scratch_dir = std::filesystem::temp_directory_path() /
                           ("builder_weather_process_threaded_" +
                            std::to_string(std::chrono::steady_clock::now()
                                               .time_since_epoch()
                                               .count()) +
                            "_" + std::to_string(random()));
  const std::string test_dir = (scratch_dir / "tiles").string();
  const std::string weather_dir = (scratch_dir / "weather").string();
  std::filesystem::remove_all(scratch_dir);
  std::filesystem::remove_all(test_dir);
  std::filesystem::remove_all(weather_dir);

  constexpr uint32_t tile_count = 16;
  for (uint32_t i = 0; i < tile_count; ++i) {
    const GraphId tile_id(i, 2, 0);
    test_graph_tile_builder base_tilebuilder(test_dir, tile_id, false);
    add_weather_test_edges(base_tilebuilder, 1);
    base_tilebuilder.StoreTileData();
    write_weather_csv(weather_dir, tile_id, static_cast<uint8_t>(i + 1),
                      static_cast<uint8_t>(i + 2));
  }

  boost::property_tree::ptree config;
  config.put("mjolnir.concurrency", 4);
  config.put("mjolnir.weather_profile.start_epoch", 0);
  config.put("mjolnir.weather_profile.valid_count", GraphTile::kWeatherProfileBuckets);
  config.put("mjolnir.weather_profile.capacity", GraphTile::kWeatherProfileBuckets);

  EXPECT_NO_THROW(ProcessWeatherTiles(test_dir, weather_dir, config));

  for (uint32_t i = 0; i < tile_count; ++i) {
    const GraphId tile_id(i, 2, 0);
    test_graph_tile test_tile(test_dir, tile_id);
    EXPECT_NEAR(test_tile.get_precipitation(0, 0),
                (static_cast<float>(i + 1) / 255.0f) * kBackendPrecipitationMax, 0.001f);
    EXPECT_NEAR(test_tile.get_wet_road(0, GraphTile::kWeatherBucketSizeSeconds),
                (static_cast<float>(i + 2) / 255.0f) * kBackendWetRoadMax, 0.001f);
  }
}

struct fake_tile : public GraphTile {
public:
  fake_tile(const std::string& plyenc_shape) {
    auto s = valhalla::midgard::decode<std::vector<PointLL>>(plyenc_shape);
    auto e = valhalla::midgard::encode7(s);
    auto l = TileHierarchy::levels().back().level;
    auto tiles = TileHierarchy::levels().back().tiles;
    auto id = GraphId(tiles.TileId(s.front()), l, 0);
    auto o_id = GraphId(tiles.TileId(s.front()), l, tiles.TileId(s.back()));
    o_id.set_id(o_id == id);
    header_ = new GraphTileHeader();
    header_->set_graphid(id);
    header_->set_directededgecount(1 + (id.tileid() == o_id.tileid()) * 1);

    auto ei_size = sizeof(EdgeInfo::EdgeInfoInner) + e.size();
    edgeinfo_ = new char[ei_size];
    EdgeInfo::EdgeInfoInner pi{0, 0, 0, 0, 0, 0, static_cast<uint32_t>(e.size()), 0, 0, 0, 0};
    std::memcpy(static_cast<void*>(edgeinfo_), static_cast<void*>(&pi),
                sizeof(EdgeInfo::EdgeInfoInner));
    textlist_ = edgeinfo_;
    textlist_size_ = 0;
    std::memcpy(static_cast<void*>(edgeinfo_ + sizeof(EdgeInfo::EdgeInfoInner)),
                static_cast<void*>(&e[0]), e.size());

    directededges_ = new DirectedEdge[2];
    std::memset(static_cast<void*>(&directededges_[0]), 0,
                sizeof(DirectedEdge) * header_->directededgecount());
    directededges_[0].set_forward(true);
  }
  ~fake_tile() {
    delete header_;
    delete[] edgeinfo_;
    delete[] directededges_;
  }
};

TEST(GraphTileBuilder, TestBinEdges) {
  std::string encoded_shape5 =
      "gsoyLcpczmFgJOsMzAwGtDmDtEmApG|@tE|EdF~PjKlRjLbKhLrJnTdD`\\oEz`@wAlJKjVnHfMpRbQdQbRvTtNrM~"
      "ShNdZ|HjLfCbPfIbGdNxBjOyBjPOnJm@rDvD~BbFxFzA|IjAdEdFy@tOqBbPv@`HfHj`@"
      "pGxMtNbFlUjBtNvMhLbQfOxLhNzVTrFkGhMsQ|J_N{AqKkBqRxCoZpGu^jLqMz@sQwCmPmJwLgNcHePwIqG{"
      "KOoLvCsLbGeUpGm`@l@}_@jBeZmA}[aQs_@gd@gOyVosAqf@gYkLub@m@{LgCkFz@sBvDKrEjApH~Er[hOha@hIfc@i@"
      "pHaIrPyMng@mF~^kA~\\h@zVtCnHrFzAlUtE~@hBv@rFqBb\\?xVdEjV}@hM?tOvClJmAdEwCvD_b@|SuKbGiGpH_"
      "JtOsOvXyBvMbBtOlAtO}EdF}GjA_QhBiKjBsHxLoGtYkGdd@yJbf@fAxWvEtO]fCcFl@{ZlJcKnI{@lJtDpH`I|J~"
      "GbFtG|@hCz@MtEoCxB_QpGmKdE_KtPyAdO~BvNUdEyBxB_HjB{NxBwOxAuHrFwF~IXjLbDjKbKbFnIzArBvDwAdE{"
      "GtEyE|IElJ~B`H_AvCyDjBgH?mRgDy`@]{OtDaKxLiCvNiFjLgMxL_XdPgr@re@yi@vb@mWrQuFjKqHnSuH|"
      "JiGlJqAfDbAtE~A~GgBdFyKlJy[xWqMvMqTlKoPfCeKiBkHeF}E{KoD{JaLsGwSeEg~BqR";
  auto decoded_shape = valhalla::midgard::decode<std::vector<PointLL>>(encoded_shape5);
  auto encoded_shape7 = valhalla::midgard::encode7(decoded_shape);
  graph_tile_ptr fake{new fake_tile(encoded_shape5)};
  auto info = fake->edgeinfo(fake->directededge(0));
  EXPECT_EQ(info.encoded_shape(), encoded_shape7);
  GraphTileBuilder::tweeners_t tweeners;
  auto bins = GraphTileBuilder::BinEdges(fake, tweeners);
  EXPECT_EQ(tweeners.size(), 1) << "This edge leaves a tile for 1 other tile and comes back.";
}

} // namespace

int main(int argc, char* argv[]) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
