#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "runtime/GraphUtils.h"
#include "build/codegen/proto/voyager_ir.pb.h"

// The loaded graph: model.txt parsed, its TensorBoxes indexed, and the
// whole-graph questions the testbench asks of it. Reading any individual
// operation, argument or operand is GraphUtils.h.

class Model {
 public:
  // Reads $PROJECT_ROOT/$CODEGEN_DIR/networks/<model>/<DATATYPE>/model.txt
  explicit Model(const std::string& model_name);

  const voyager::Model& proto() const { return model_; }

  bool has_box(const std::string& node) const { return boxes_.count(node) > 0; }
  // The declaration of `node`. References carry their own box; this is the
  // canonical one, for the whole-model questions below.
  const voyager::TensorBox& box(const std::string& node) const;

  // What a TESTS selection runs: the layer extent named by TESTS -- every
  // top-level op the layer's bufferized nest emitted, its epilogue included
  // -- or every op when unset, plus the ops MAX_TILES bounds (every selected
  // op; the bound acts only on an outermost loop). A cut that ends a tile
  // loop mid-tile drops the epilogue's stores of that unfinished tile; every
  // later wait on a dropped store's counter, until that counter's next
  // reset, is lowered by the stores dropped from it: `lowered` maps the
  // wait's threshold (an async_wait's or a commit wait's) to that number.
  struct Selection {
    std::vector<const voyager::Operation*> ops;
    std::set<const voyager::Operation*> bounded;
    std::map<const voyager::ScalarValue*, int64_t> lowered;
  };

  Selection selected_ops(const std::vector<std::string>& names) const;

  // {DRAM, SRAM, reference} sizes for the ArrayMemory.
  std::vector<uint64_t> memory_sizes() const;

  // Inputs + parameters the graph actually reads out of DRAM, i.e. everything
  // that must be preloaded from tensor_files before a walk.
  std::vector<const voyager::TensorBox*> resident_inputs() const;

  // DRAM buffers `ops` reads before writing (live_in, to preload) and the
  // gradable buffers it writes (live_out): DRAM boxes plus, when
  // include_scratchpad is set, scratchpad results.
  void live_sets(const std::vector<const voyager::Operation*>& ops,
                 std::vector<const voyager::TensorBox*>* live_in,
                 std::vector<const voyager::TensorBox*>* live_out,
                 bool include_scratchpad = false) const;

  void print_summary() const;

  std::string data_dir;

 private:
  void collect_boxes();
  void load_layers(const std::string& dir);

  // A layer's extent: the contiguous top-level ops its bufferized nest
  // emitted (output allocs, tile loop, pipeline drain), from layers.txt.
  struct LayerExtent {
    std::string start;
    std::string end;
  };

  voyager::Model model_;
  std::map<std::string, const voyager::TensorBox*> boxes_;
  std::map<std::string, LayerExtent> layers_;
};
