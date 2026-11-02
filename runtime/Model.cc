#include "runtime/Model.h"

#include <google/protobuf/text_format.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>

#include "spdlog/spdlog.h"
#include "runtime/Utils.h"

using google::protobuf::TextFormat;

namespace {

std::string env(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr) {
    throw std::runtime_error(std::string("Environment variable not set: ") +
                             name);
  }
  return std::string(value);
}

using Visitor = std::function<void(const voyager::Operation&)>;

// Visits an Operation and everything nested inside it, descending into loop
// bodies, loop conditions and both arms of every conditional. Allocations are
// model-scoped but are *declared* wherever the alloc happens to sit, which is
// often several regions deep, so a flat scan of model.ops() sees almost none of
// them.
void walk_op(const voyager::Operation& op, const Visitor& visit);

void walk_all(const google::protobuf::RepeatedPtrField<voyager::Operation>& ops,
              const Visitor& visit) {
  for (const auto& op : ops) walk_op(op, visit);
}

void walk_op(const voyager::Operation& op, const Visitor& visit) {
  visit(op);
  switch (op.op_type_case()) {
    case voyager::Operation::kLoop:
      if (op.loop().has_for_loop()) {
        walk_all(op.loop().for_loop().body().ops(), visit);
      } else if (op.loop().has_while_loop()) {
        walk_all(op.loop().while_loop().condition().ops(), visit);
        walk_all(op.loop().while_loop().body().ops(), visit);
      }
      break;
    case voyager::Operation::kCond:
      walk_all(op.cond().true_region().ops(), visit);
      walk_all(op.cond().false_region().ops(), visit);
      break;
    case voyager::Operation::kAsync:
      walk_all(op.async().body().ops(), visit);
      break;
    default:
      break;
  }
}

// Steps of a tile loop per output tile. The loop delinearizes its step over
// the tile grid, and the dispatches of a split reduction sit in conditionals
// on its first or last round (`k == 0`, `k == K - 1`); K, that dimension's
// extent, is the rounds per tile, and the reduction must be the innermost
// dimension so a tile's rounds are consecutive steps. A loop with no such
// conditional runs one step per tile.
int64_t rounds_per_tile(const voyager::Operation& loop_op,
                        const std::string& layer) {
  const auto fail = [&](const std::string& why) {
    throw std::runtime_error("Layer " + layer + ", loop " + loop_op.name() +
                             ": cannot tell the steps per output tile (" + why +
                             ").");
  };
  if (!loop_op.loop().has_for_loop()) fail("not a counted loop");
  const voyager::ForLoop& loop = loop_op.loop().for_loop();

  // The step's grid index: each result of a delinearize of the induction
  // variable, as (basis, dimension).
  std::map<std::string, std::pair<std::vector<int64_t>, int>> grid_index;
  // Each scalar comparison: its operand and the literal compared against.
  std::map<std::string, std::pair<std::string, int64_t>> comparisons;
  for (const auto& op : loop.body().ops()) {
    if (!op.has_prim()) continue;
    const voyager::PrimOp& prim = op.prim();
    if (prim.target() == "voyager::delinearize_index") {
      const auto& linear = prim.kwargs().at("linear").scalar();
      if (!linear.has_node() || linear.node() != loop.iv()) continue;
      std::vector<int64_t> basis;
      for (const auto& value : prim.kwargs().at("basis").scalar_list().values()) {
        if (!value.has_int_value()) fail("a non-literal grid extent");
        basis.push_back(value.int_value());
      }
      for (int d = 0; d < op.outputs_size(); d++) {
        grid_index[op.outputs(d).name()] = {basis, d};
      }
    } else if (strip_namespace(prim.target()) == "eq" &&
               op.outputs_size() == 1 && has_arg(prim, "input") &&
               has_arg(prim, "other")) {
      const auto& input = arg(prim, "input").scalar();
      const auto& other = arg(prim, "other").scalar();
      if (input.has_node() && other.has_int_value()) {
        comparisons[op.outputs(0).name()] = {input.node(), other.int_value()};
      }
    }
  }

  const auto dispatches = [](const voyager::Region& region) {
    bool found = false;
    walk_all(region.ops(), [&](const voyager::Operation& op) {
      if (op.has_async()) found = true;
    });
    return found;
  };

  int reduction_dim = -1;
  std::vector<int64_t> basis;
  for (const auto& op : loop.body().ops()) {
    if (!op.has_cond() || !op.cond().predicate().has_node()) continue;
    const auto comparison = comparisons.find(op.cond().predicate().node());
    if (comparison == comparisons.end()) continue;
    const auto index = grid_index.find(comparison->second.first);
    if (index == grid_index.end()) continue;
    const auto& [index_basis, dim] = index->second;
    const int64_t extent = index_basis[dim];
    const int64_t literal = comparison->second.second;
    if (extent <= 1 || (literal != 0 && literal != extent - 1)) continue;
    if (!dispatches(op.cond().true_region()) &&
        !dispatches(op.cond().false_region())) {
      continue;
    }
    if (reduction_dim >= 0 && reduction_dim != dim) {
      fail("its round conditionals test two grid dimensions");
    }
    reduction_dim = dim;
    basis = index_basis;
  }
  if (reduction_dim < 0) return 1;
  for (size_t d = reduction_dim + 1; d < basis.size(); d++) {
    if (basis[d] != 1) fail("the reduction is not the innermost dimension");
  }
  return basis[reduction_dim];
}

// The epilogue ops a cut that ends mid-tile must not run: each store of the
// last output tile, which no pass finalized, and the wait before it on the
// counter of that tile's last writer (a counter the loop's dispatches post).
// The stores are the epilogue's copies from the scratchpad to DRAM.
std::set<const voyager::Operation*> unfinished_tile_stores(
    const google::protobuf::RepeatedPtrField<voyager::Operation>& ops,
    int loop_index, int end_index, const std::string& layer) {
  std::set<std::string> unit_counters;
  walk_op(ops.Get(loop_index), [&](const voyager::Operation& op) {
    if (op.has_async() && op.async().has_post()) {
      unit_counters.insert(op.async().post().box().node());
    }
  });

  std::set<const voyager::Operation*> dropped;
  for (int i = loop_index + 1; i <= end_index; i++) {
    const voyager::Operation& op = ops.Get(i);
    if (!op.has_prim() || op.prim().target() != "voyager::async_copy") {
      continue;
    }
    const voyager::PrimOp& copy = op.prim();
    const auto& src = arg(copy, "src").tensor_box().box();
    const auto& dst = arg(copy, "dst").tensor_box().box();
    if (src.memory().level() != voyager::MEMORY_LEVEL_SCRATCHPAD ||
        !is_dram(dst)) {
      continue;
    }
    // The wait guarding the store sits before it, after any address
    // arithmetic and views.
    int wait_index = i - 1;
    while (wait_index > loop_index && ops.Get(wait_index).has_prim() &&
           ops.Get(wait_index).prim().target() != "voyager::async_wait" &&
           ops.Get(wait_index).prim().target() != "voyager::async_copy") {
      wait_index--;
    }
    const voyager::Operation& wait = ops.Get(wait_index);
    const bool guards = wait.has_prim() &&
                        wait.prim().target() == "voyager::async_wait" &&
                        unit_counters.count(arg(wait.prim(), "counter")
                                                .tensor_box()
                                                .box()
                                                .node()) != 0;
    if (!guards) {
      throw std::runtime_error(
          "Layer " + layer + ": the epilogue store " + op.name() +
          " has no wait on a counter the loop's dispatches post before it.");
    }
    dropped.insert(&op);
    dropped.insert(&wait);
  }
  return dropped;
}

}  // namespace

Model::Model(const std::string& model_name) {
  const std::string dir = env("PROJECT_ROOT") + "/" + env("CODEGEN_DIR") +
                          "/networks/" + model_name + "/" + env("DATATYPE");
  data_dir = dir + "/tensor_files";

  const std::string filename = dir + "/model.txt";
  if (!std::filesystem::exists(filename)) {
    throw std::runtime_error("Model does not exist: " + filename);
  }

  std::ifstream input(filename);
  std::stringstream buffer;
  buffer << input.rdbuf();
  if (!TextFormat::ParseFromString(buffer.str(), &model_)) {
    throw std::runtime_error("Failed to parse " + filename);
  }

  collect_boxes();
  load_layers(dir);
}

void Model::load_layers(const std::string& dir) {
  // Absent for whole-graph flows that never select by name; required to
  // resolve TESTS.
  const std::string filename = dir + "/layers.txt";
  if (!std::filesystem::exists(filename)) return;

  std::ifstream input(filename);
  std::string line;
  while (std::getline(input, line)) {
    std::istringstream fields(line);
    std::string name, start, end, group, tiles;
    if (!(fields >> name)) continue;
    if (!(fields >> start >> end >> group >> tiles)) {
      throw std::runtime_error(
          filename +
          " is not the five-column layer table; regenerate the network with "
          "the current compiler.");
    }
    layers_[name] = {start, end};
  }
}

void Model::collect_boxes() {
  const auto declare = [&](const voyager::TensorBox& box) {
    const auto existing = boxes_.find(box.node());
    if (existing != boxes_.end()) {
      // slice_tensor is declared twice on purpose: once as a model output and
      // once as the box the slice op owns. Anything else is a real conflict.
      if (existing->second->SerializeAsString() != box.SerializeAsString()) {
        throw std::runtime_error("Conflicting declarations of TensorBox " +
                                 box.node());
      }
      return;
    }
    // A box with no memory names a value an earlier PrimOp of a fusion
    // produced: it never leaves the datapath and owns no storage.
    if (!box.has_memory()) return;
    boxes_[box.node()] = &box;
  };

  for (const auto& box : model_.inputs()) declare(box);
  for (const auto& box : model_.parameters()) declare(box);
  for (const auto& box : model_.outputs()) declare(box);
  walk_all(model_.ops(), [&](const voyager::Operation& op) {
    for (const auto& output : op.outputs()) {
      if (output.has_tensor_box()) declare(output.tensor_box());
    }
  });
}

const voyager::TensorBox& Model::box(const std::string& node) const {
  const auto found = boxes_.find(node);
  if (found == boxes_.end()) {
    throw std::runtime_error("No TensorBox named " + node);
  }
  return *found->second;
}

Model::Selection Model::selected_ops(
    const std::vector<std::string>& names) const {
  Selection selection;
  if (names.empty()) {
    // The whole graph: every outer tile loop is the run's own work, so the
    // bound applies to all of it.
    for (const auto& op : model_.ops()) {
      selection.ops.push_back(&op);
      selection.bounded.insert(&op);
    }
    return selection;
  }

  std::map<std::string, int> top_index;
  for (int i = 0; i < model_.ops_size(); i++) {
    top_index[model_.ops(i).name()] = i;
  }

  const auto extent_range = [&](const LayerExtent& extent) {
    const auto start = top_index.find(extent.start);
    const auto end = top_index.find(extent.end);
    if (start == top_index.end() || end == top_index.end()) {
      throw std::runtime_error("layers.txt extent [" + extent.start + ", " +
                               extent.end + "] names ops the model lacks.");
    }
    return std::pair<int, int>(start->second, end->second);
  };

  const int64_t max_tiles = getenv_int("MAX_TILES", 0);

  for (const auto& name : names) {
    int start_index = -1;
    int end_index = -1;

    const auto layer = layers_.find(name);
    if (layer != layers_.end()) {
      const std::pair<int, int> range = extent_range(layer->second);
      start_index = range.first;
      end_index = range.second;
    } else {
      int found_index = -1;
      for (int i = 0; i < model_.ops_size() && found_index < 0; i++) {
        walk_op(model_.ops(i), [&](const voyager::Operation& op) {
          if (op.name() == name) found_index = i;
        });
      }
      if (found_index < 0) {
        throw std::runtime_error("No operation named " + name);
      }
      if (layers_.empty()) {
        throw std::runtime_error(
            "Selecting TESTS=" + name +
            " needs the layer extents in layers.txt; regenerate the network "
            "with the current compiler.");
      }
      // An extent that covers the op selects its whole layer; a host-side op
      // between two layers belongs to neither and is selected alone.
      start_index = end_index = found_index;
      for (const auto& [display, extent] : layers_) {
        const std::pair<int, int> range = extent_range(extent);
        if (range.first <= found_index && found_index <= range.second) {
          start_index = range.first;
          end_index = range.second;
          break;
        }
      }
    }

    // A MAX_TILES cut ends a kernel's tile loop early and the kernel's
    // epilogue, which follows the loop, still runs: it reads the counts the
    // loop reached, so the whole extent is selected and its loops bounded. A
    // cut that ends mid-tile leaves the last tile unfinished, so the epilogue
    // runs without that tile's stores.
    std::set<const voyager::Operation*> dropped;
    for (int i = start_index; i <= end_index && max_tiles > 0; i++) {
      const voyager::Operation& op = model_.ops(i);
      if (!op.has_loop() || !op.loop().has_for_loop()) continue;
      const voyager::ForLoop& loop = op.loop().for_loop();
      if (!loop.start().has_int_value() || !loop.end().has_int_value() ||
          !loop.step().has_int_value() || loop.step().int_value() <= 0) {
        continue;
      }
      const int64_t step = loop.step().int_value();
      const int64_t trips =
          (loop.end().int_value() - loop.start().int_value() + step - 1) /
          step;
      if (max_tiles >= trips) continue;
      if (max_tiles % rounds_per_tile(op, name) == 0) continue;
      const auto stores =
          unfinished_tile_stores(model_.ops(), i, end_index, name);
      dropped.insert(stores.begin(), stores.end());
    }

    for (int i = start_index; i <= end_index; i++) {
      const voyager::Operation& op = model_.ops(i);
      if (dropped.count(&op) != 0) continue;
      if (!contains(selection.ops, &op)) selection.ops.push_back(&op);
      selection.bounded.insert(&op);
    }
  }
  return selection;
}

std::vector<uint64_t> Model::memory_sizes() const {
  uint64_t dram = 0;
  uint64_t sram = 0;
  uint64_t reference = 0;

  for (const auto& [node, box] : boxes_) {
    if (is_counter(*box) || is_constant(*box)) continue;

    const Tensor tensor = to_tensor(*box);
    const uint64_t end =
        tensor.address +
        banks_of(*box) *
            std::max<uint64_t>(bank_stride_of(*box), get_num_bytes(tensor));
    if (is_dram(*box)) {
      dram = std::max(dram, end);
      reference = std::max(reference, get_num_bytes(tensor));
    } else {
      sram = std::max(sram, end);
    }
  }

  // A little headroom: sub-byte writes round up to a whole byte at the tail.
  return {dram + 1024, sram + 1024, reference + 1024};
}

std::vector<const voyager::TensorBox*> Model::resident_inputs() const {
  std::vector<const voyager::TensorBox*> resident;
  for (const auto& box : model_.inputs()) resident.push_back(&box);
  for (const auto& box : model_.parameters()) {
    if (!is_constant(box)) resident.push_back(&box);
  }
  return resident;
}

void Model::live_sets(const std::vector<const voyager::Operation*>& ops,
                      std::vector<const voyager::TensorBox*>* live_in,
                      std::vector<const voyager::TensorBox*>* live_out,
                      bool include_scratchpad) const {
  std::set<std::string> read;
  std::set<std::string> written;
  // A scratchpad buffer a DMA writes into is a staged input or weight tile,
  // not a compute result, so it is never gradable.
  std::set<std::string> copy_dests;
  // A CSR's values and column indices are allocated for the worst-case
  // nonzero count, and only the entries the row pointers span mean anything.
  // Past them the buffer holds whatever the last tile that reached that far
  // left, which the two simulators need not agree on and no consumer reads --
  // an SpMM is bounded by the indptr. The row pointers themselves are defined
  // end to end, so they stay gradable.
  std::set<std::string> csr_payloads;

  const auto visit = [&](const voyager::Operation& op) {
    for (const auto* prim : get_prim_ops(op)) {
      const bool is_copy = prim->target() == "voyager::async_copy";
      const bool is_zeros = prim->target() == "voyager::zeros";
      for (const auto& [key, arg] : prim->kwargs()) {
        if (!arg.has_tensor_box()) continue;
        const voyager::TensorBox& operand = arg.tensor_box().box();
        const std::string& node = operand.node();
        if (!has_box(node)) continue;  // a fusion intermediate
        if (is_counter(operand)) continue;
        if (is_copy && key == "dst") {
          copy_dests.insert(node);
          written.insert(node);
        } else if (is_zeros) {
          written.insert(node);
        } else if (!written.count(node)) {
          read.insert(node);
        }
      }
    }
    for (const auto& output : op.outputs()) {
      if (output.has_destination()) {
        written.insert(output.destination().box().node());
      } else if (output.has_tensor_box()) {
        written.insert(output.tensor_box().node());
      }
    }

    // set_quantize_mx_params reads the same three in this order: values,
    // column indices, row pointers.
    for (const auto* prim : get_prim_ops(op)) {
      if (strip_namespace(prim->target()) != "quantize_mx_outlier") continue;
      for (int i = 0; i < 2 && i < op.outputs_size(); i++) {
        const auto& output = op.outputs(i);
        if (output.has_destination()) {
          csr_payloads.insert(output.destination().box().node());
        } else if (output.has_tensor_box()) {
          csr_payloads.insert(output.tensor_box().node());
        }
      }
    }
  };

  for (const auto* op : ops) walk_op(*op, visit);

  if (live_in != nullptr) {
    for (const auto& node : read) {
      const voyager::TensorBox& b = box(node);
      if (is_dram(b) && !is_constant(b)) live_in->push_back(&b);
    }
  }
  if (live_out != nullptr) {
    for (const auto& node : written) {
      const voyager::TensorBox& b = box(node);
      // zeros declares the counter it resets as an output; never grade those.
      if (is_counter(b)) continue;

      if (csr_payloads.count(node) != 0) continue;

      if (is_dram(b)) {
        live_out->push_back(&b);
      } else if (include_scratchpad && partition_of(b) == SRAM_PARTITION &&
                 copy_dests.count(node) == 0) {
        live_out->push_back(&b);
      }
    }
  }
}

void Model::print_summary() const {
  int dram = 0, sram = 0, counters = 0, constants = 0, banked = 0;
  for (const auto& [node, box] : boxes_) {
    if (is_counter(*box))
      counters++;
    else if (partition_of(*box) == SRAM_PARTITION)
      sram++;
    else
      dram++;
    if (is_constant(*box)) constants++;
    if (banks_of(*box) > 1) banked++;
  }

  const auto sizes = memory_sizes();
  spdlog::info("Model: {} top-level ops, {} TensorBoxes\n", model_.ops_size(),
               boxes_.size());
  spdlog::info("> DRAM boxes: {} ({} file-backed constants)\n", dram,
               constants);
  spdlog::info("> Scratchpad boxes: {} ({} banked)\n", sram, banked);
  spdlog::info("> Counters: {}\n", counters);
  spdlog::info("> DRAM: {} B, SRAM: {} B\n", sizes[0], sizes[1]);
}
