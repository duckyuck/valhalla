// joyride_export_edges
//
// Walk every drivable edge in the base Valhalla tileset and emit one CSV
// line per edge:
//
//   edge_id,lon0,lat0,lon1,lat1,...
//
// where edge_id is Valhalla's GraphId serialized as "level/tile/index".
// Used by the Joyride weather rebake pipeline to associate each Valhalla
// edge with the weather grid cells it crosses.

#include "baldr/directededge.h"
#include "baldr/graphconstants.h"
#include "baldr/graphreader.h"
#include "baldr/graphtile.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <cstdint>
#include <cstdio>

using namespace valhalla::baldr;

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <valhalla.json>\n", argv[0]);
    return 1;
  }

  boost::property_tree::ptree cfg;
  boost::property_tree::read_json(argv[1], cfg);
  GraphReader reader(cfg.get_child("mjolnir"));

  for (auto tile_id : reader.GetTileSet()) {
    auto tile = reader.GetGraphTile(tile_id);
    if (!tile)
      continue;

    const uint32_t n = tile->header()->directededgecount();
    for (uint32_t i = 0; i < n; ++i) {
      GraphId eid = tile_id;
      eid.set_id(i);
      const DirectedEdge* e = tile->directededge(i);
      if (!(e->forwardaccess() & kAutoAccess))
        continue;
      if (e->is_shortcut())
        continue;

      auto shape = tile->edgeinfo(e).shape();
      if (shape.empty())
        continue;
      if (e->speed() < kMinSpeedKph)
        continue;

      const auto& first = shape.front();
      std::printf("%u/%u/%u,%.6f,%.6f", eid.level(), eid.tileid(), eid.id(), first.lng(),
                  first.lat());
      for (size_t j = 1; j < shape.size(); ++j) {
        const auto& p = shape[j];
        std::printf(",%.6f,%.6f", p.lng(), p.lat());
      }
      std::printf("\n");
    }
  }
  return 0;
}
