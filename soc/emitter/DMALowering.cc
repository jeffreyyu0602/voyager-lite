#include "soc/emitter/DMALowering.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <stdexcept>

#include "soc/firmware/common/voyager_address.h"

namespace {

// Records the moves of the executable spec. A read followed by a write of the
// same length is a byte move, a write of zeros with no read a fill. A 4-bit
// element moves as read source byte, read destination byte, write
// destination byte (MemoryInterface::write_value): the source reads back
// kSourceTag, the destination 0, and the written byte says which nibble went
// where.
class MoveRecorder : public MemoryInterface {
 public:
  std::vector<DMAMove> moves;

  void read_bytes_from_memory(const long long address, const int partition,
                              const int num_bytes, char* bytes) override {
    const uint64_t at = absolute(address, partition);
    if (pending_) {
      if (target_ || num_bytes != 1 || pending_->nibbles != 2) {
        throw std::runtime_error("two reads with no write between");
      }
      target_ = at;
      bytes[0] = 0;
      return;
    }
    pending_ = DMAMove{at, 0, 2 * static_cast<uint64_t>(num_bytes)};
    std::memset(bytes, kSourceTag, num_bytes);
  }

  void write_bytes_to_memory(const long long address, const int partition,
                             const int num_bytes, const char* bytes) override {
    const uint64_t dst = absolute(address, partition);
    if (target_) {
      if (num_bytes != 1 || dst != *target_) {
        throw std::runtime_error("an element move writes elsewhere");
      }
      // The source nibble came from kSourceTag's low (1) or high (2) half.
      const auto written = static_cast<uint8_t>(bytes[0]);
      if (written != 0x01 && written != 0x02 && written != 0x10 &&
          written != 0x20) {
        throw std::runtime_error("converts a type");
      }
      const int src_high = (written == 0x02 || written == 0x20) ? 1 : 0;
      const int dst_high = (written == 0x10 || written == 0x20) ? 1 : 0;
      moves.push_back({2 * pending_->src + src_high, 2 * dst + dst_high, 1});
      pending_.reset();
      target_.reset();
      return;
    }
    if (pending_) {
      if (pending_->nibbles != 2 * static_cast<uint64_t>(num_bytes)) {
        throw std::runtime_error("a move changes its byte count");
      }
      moves.push_back({2 * pending_->src, 2 * dst, pending_->nibbles});
      pending_.reset();
      return;
    }
    for (int i = 0; i < num_bytes; i++) {
      if (bytes[i] != 0) throw std::runtime_error("fills a nonzero value");
    }
    moves.push_back({kDMAFill, 2 * dst, 2 * static_cast<uint64_t>(num_bytes)});
  }

  void finish() const {
    if (pending_) throw std::runtime_error("reads with no write");
  }

 private:
  // A source byte's nibbles: 1 low, 2 high.
  static constexpr char kSourceTag = 0x21;
  // The pending move's source byte and its length in nibbles, and the
  // destination byte an element move read.
  std::optional<DMAMove> pending_;
  std::optional<uint64_t> target_;

  static uint64_t absolute(long long address, int partition) {
    if (partition == SRAM_PARTITION) return SRAM_BASE + address;
    if (partition == DRAM_PARTITION) return DRAM_BASE + address;
    throw std::runtime_error("touches partition " + std::to_string(partition));
  }
};

// Joins neighbouring moves that continue each other on both sides.
std::vector<DMAMove> coalesce(const std::vector<DMAMove>& moves) {
  std::vector<DMAMove> out;
  for (const auto& move : moves) {
    if (!out.empty()) {
      DMAMove& last = out.back();
      const bool src_follows = last.src == kDMAFill
                                   ? move.src == kDMAFill
                                   : move.src == last.src + last.nibbles;
      if (src_follows && move.dst == last.dst + last.nibbles) {
        last.nibbles += move.nibbles;
        continue;
      }
    }
    out.push_back(move);
  }
  return out;
}

// The moves as a set: sorted by destination, neighbours joined. Equal sets
// move the same bytes from the same places. Throws if two moves write one
// byte, which would make their order matter.
std::vector<DMAMove> canonical(std::vector<DMAMove> moves) {
  std::sort(moves.begin(), moves.end(),
            [](const DMAMove& a, const DMAMove& b) { return a.dst < b.dst; });
  for (size_t i = 1; i < moves.size(); i++) {
    if (moves[i].dst < moves[i - 1].dst + moves[i - 1].nibbles) {
      throw std::runtime_error("writes a byte twice");
    }
  }
  return coalesce(moves);
}

bool same_moves(const std::vector<DMAMove>& a, const std::vector<DMAMove>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    if (a[i].src != b[i].src || a[i].dst != b[i].dst ||
        a[i].nibbles != b[i].nibbles) {
      return false;
    }
  }
  return true;
}

std::vector<DMAMove> expand(const DMADescriptor& copy) {
  std::vector<DMAMove> moves;
  for_each_dma_move(copy, [&](uint64_t src, uint64_t dst, uint32_t nibbles) {
    moves.push_back({src, dst, nibbles});
  });
  return moves;
}

// Outer levels of equal-length moves, level 0 fastest: each level repeats the
// one below it at a fixed source and destination stride. Up to `max_levels`.
struct Nest {
  uint64_t nibbles = 0;
  std::vector<dma_level_t> levels;
};

std::optional<Nest> fit_nest(const std::vector<DMAMove>& moves,
                             size_t max_levels) {
  if (moves.empty()) return std::nullopt;
  Nest nest;
  nest.nibbles = moves[0].nibbles;
  for (const auto& move : moves) {
    if (move.nibbles != nest.nibbles) return std::nullopt;
  }
  const bool fill = moves[0].src == kDMAFill;
  size_t group = 1;
  while (group < moves.size()) {
    if (nest.levels.size() == max_levels) return std::nullopt;
    const int64_t src_stride =
        fill ? 0 : static_cast<int64_t>(moves[group].src - moves[0].src);
    const int64_t dst_stride =
        static_cast<int64_t>(moves[group].dst - moves[0].dst);
    size_t count = 1;
    while ((count + 1) * group <= moves.size()) {
      const DMAMove& next = moves[count * group];
      if (!fill && next.src != moves[0].src + count * src_stride) break;
      if (next.dst != moves[0].dst + count * dst_stride) break;
      count++;
    }
    if (count == 1 || count > UINT32_MAX) return std::nullopt;
    nest.levels.push_back(
        {static_cast<uint32_t>(count), src_stride, dst_stride});
    group *= count;
  }
  if (group != moves.size()) return std::nullopt;
  return nest;
}

// A descriptor that moves exactly `moves`; `transposed` prims try the
// transpose buffer first. Empty with the reason in *why otherwise.
std::optional<DMADescriptor> fit(const std::vector<DMAMove>& moves,
                                 bool transposed, std::string* why) {
  if (moves.empty()) {
    // Moves nothing: one level of count 0, and the counter still advances.
    DMADescriptor empty;
    empty.inner = 1;
    empty.levels[0] = {0, 0, 0};
    return empty;
  }
  const bool fill = moves[0].src == kDMAFill;
  for (const auto& move : moves) {
    if ((move.src == kDMAFill) != fill) {
      *why = "mixes moved and filled bytes (a padded copy)";
      return std::nullopt;
    }
  }

  DMADescriptor copy;
  if (fill) copy.ctrl |= DMA_FILL;
  // The descriptor counts bytes: each nibble address and stride it takes must
  // be even.
  auto whole = [](int64_t nibbles) { return nibbles % 2 == 0; };
  const uint64_t elem = moves[0].nibbles;
  if (transposed && (elem == 1 || elem == 2 || elem == 4)) {
    // Element moves: level 0 walks a source row (source +elem), level 1 the
    // rows (destination +elem); the rest repeat the transpose.
    const auto nest = fit_nest(moves, 2 + (DMA_LEVELS - 1));
    bool fits = nest && nest->levels.size() >= 2 &&
                nest->levels[0].src_stride == static_cast<int64_t>(elem) &&
                nest->levels[1].dst_stride == static_cast<int64_t>(elem) &&
                whole(moves[0].src) && whole(moves[0].dst) &&
                whole(nest->levels[0].count * elem) &&
                whole(nest->levels[1].src_stride) &&
                whole(nest->levels[0].dst_stride);
    // 4-bit columns are whole bytes only for an even row count.
    if (fits && elem == 1) fits = nest->levels[1].count % 2 == 0;
    for (size_t k = 2; fits && k < nest->levels.size(); k++) {
      fits = whole(nest->levels[k].src_stride) &&
             whole(nest->levels[k].dst_stride);
    }
    if (fits) {
      copy.src = moves[0].src / 2;
      copy.dst = moves[0].dst / 2;
      copy.inner = static_cast<uint32_t>(nest->levels[0].count * elem / 2);
      copy.ctrl |= DMA_TRANSPOSED |
                   (elem == 1 ? DMA_NIBBLES : DMA_ELEM_LG2(elem == 4 ? 1 : 0));
      copy.levels[0] = {nest->levels[1].count, nest->levels[1].src_stride / 2,
                        nest->levels[0].dst_stride / 2};
      for (size_t k = 2; k < nest->levels.size(); k++) {
        copy.levels[k - 1] = {nest->levels[k].count,
                              nest->levels[k].src_stride / 2,
                              nest->levels[k].dst_stride / 2};
      }
      return copy;
    }
  }

  // Runs: fills merge whatever is contiguous; copies only a level whose
  // strides equal the run, so the run length stays uniform.
  const std::vector<DMAMove> runs = fill ? coalesce(moves) : moves;
  auto nest = fit_nest(runs, 6);
  if (!nest) {
    *why = "is not a nest of equal runs at fixed strides";
    return std::nullopt;
  }
  while (!nest->levels.empty()) {
    const dma_level_t& level = nest->levels.front();
    const auto run = static_cast<int64_t>(nest->nibbles);
    if (!((fill || level.src_stride == run) && level.dst_stride == run)) break;
    nest->nibbles *= level.count;
    nest->levels.erase(nest->levels.begin());
  }
  if (nest->levels.size() > DMA_LEVELS) {
    *why = "needs " + std::to_string(nest->levels.size()) + " outer levels";
    return std::nullopt;
  }
  if (nest->nibbles / 2 > UINT32_MAX) {
    *why = "has a run longer than 4 GB";
    return std::nullopt;
  }
  bool aligned = whole(runs[0].dst) && whole(nest->nibbles) &&
                 (fill || whole(runs[0].src));
  for (const auto& level : nest->levels) {
    aligned = aligned && whole(level.src_stride) && whole(level.dst_stride);
  }
  if (!aligned) {
    *why = "moves half bytes";
    return std::nullopt;
  }
  copy.src = fill ? 0 : runs[0].src / 2;
  copy.dst = runs[0].dst / 2;
  copy.inner = static_cast<uint32_t>(nest->nibbles / 2);
  for (size_t k = 0; k < nest->levels.size(); k++) {
    copy.levels[k] = {nest->levels[k].count, nest->levels[k].src_stride / 2,
                      nest->levels[k].dst_stride / 2};
  }
  return copy;
}

bool same_shape(const DMADescriptor& a, const DMADescriptor& b) {
  if (a.inner != b.inner || a.ctrl != b.ctrl) return false;
  for (int k = 0; k < DMA_LEVELS; k++) {
    if (a.levels[k].count != b.levels[k].count ||
        a.levels[k].src_stride != b.levels[k].src_stride ||
        a.levels[k].dst_stride != b.levels[k].dst_stride) {
      return false;
    }
  }
  return true;
}

bool is_transposed(const voyager::PrimOp& prim, const ScalarEnv& env) {
  return prim.kwargs().count("transposed") > 0 &&
         to_bool(eval(prim.kwargs().at("transposed").scalar(), env));
}

}  // namespace

std::vector<DMAMove> record_dma_moves(const HostOp& host,
                                      const ScalarEnv& env) {
  MoveRecorder recorder;
  if (host.kind == HostOp::kCopy) {
    run_async_copy(*host.prim, env, &recorder);
  } else if (host.kind == HostOp::kZero) {
    // As the testbench fills it (SoCSimulation::execute).
    const auto& box = host.op->outputs(0).tensor_box();
    if (host.prim->target() == "voyager::alloc") {
      for (uint32_t bank = 0; bank < banks_of(box); bank++) {
        zero_buffer(to_tensor(box, bank), 1, 0, &recorder);
      }
    } else {
      zero_buffer(to_tensor(box), banks_of(box), bank_stride_of(box),
                  &recorder);
    }
  } else {
    throw std::runtime_error("is not a copy or a zero fill");
  }
  recorder.finish();
  return recorder.moves;
}

DMALowering lower_to_dma(
    const HostOp& host, const ScalarEnv& env,
    const std::function<int64_t(const std::string&)>& step_of) {
  DMALowering lowering;
  const bool transposed =
      host.kind == HostOp::kCopy && is_transposed(*host.prim, env);

  // The descriptor and the spec's moves under one environment; nullopt when
  // the spec refuses that environment (a probe out of range).
  struct Sample {
    DMADescriptor copy;
    std::vector<DMAMove> moves;
  };
  auto sample = [&](const ScalarEnv& at) -> std::optional<Sample> {
    std::vector<DMAMove> moves;
    try {
      moves = record_dma_moves(host, at);
    } catch (const std::exception&) {
      return std::nullopt;
    }
    std::string why;
    const auto copy = fit(moves, transposed, &why);
    if (!copy) throw std::runtime_error(why);
    return Sample{*copy, canonical(moves)};
  };

  try {
    // The reference environment: the emission one, or, where the spec
    // refuses it (an epilogue's store sees the loop's initial values, not its
    // final ones), the nearest shift of its scalars the spec accepts. The
    // emitted addresses only extrapolate from it; the probes check them.
    ScalarEnv origin = env;
    std::vector<DMAMove> base_moves;
    try {
      base_moves = record_dma_moves(host, env);
    } catch (const std::exception& refusal) {
      bool found = false;
      for (const auto& name : request_scalar_names(*host.op)) {
        const int64_t value = to_int(env.lookup(name));
        const int64_t step = step_of(name);
        for (const int64_t delta :
             {step, -step, int64_t{1}, int64_t{-1}, int64_t{2}, int64_t{-2}}) {
          if (delta == 0) continue;
          ScalarEnv at = env;
          at.define(name, value + delta);
          try {
            base_moves = record_dma_moves(host, at);
          } catch (const std::exception&) {
            continue;
          }
          origin = at;
          found = true;
          break;
        }
        if (found) break;
      }
      if (!found) {
        // Every scalar a step forward at once.
        ScalarEnv at = env;
        for (const auto& name : request_scalar_names(*host.op)) {
          const int64_t step = step_of(name);
          at.define(name, to_int(env.lookup(name)) + (step != 0 ? step : 1));
        }
        try {
          base_moves = record_dma_moves(host, at);
        } catch (const std::exception&) {
          throw refusal;
        }
        origin = at;
      }
    }
    std::string why;
    const auto base_copy = fit(base_moves, transposed, &why);
    if (!base_copy) throw std::runtime_error(why);
    const Sample base{*base_copy, canonical(base_moves)};
    if (!same_moves(canonical(expand(base.copy)), base.moves)) {
      throw std::runtime_error("its descriptor does not reproduce the spec");
    }
    lowering.descriptor = base.copy;
    // Copies into the scratchpad are loads, the rest stores.
    lowering.queue = base.copy.dst < DRAM_BASE ? DMA_LOAD : DMA_STORE;

    // A descriptor whose addresses moved by `delta` units of `name`; it must
    // reproduce the spec's moves there.
    auto predict = [&](const std::map<std::string, int64_t>& deltas) {
      DMADescriptor copy = base.copy;
      for (const auto& [name, delta] : deltas) {
        copy.src += lowering.src_coefficients[name] * delta;
        copy.dst += lowering.dst_coefficients[name] * delta;
      }
      return copy;
    };

    std::map<std::string, int64_t> combined;
    for (const auto& name : request_scalar_names(*host.op)) {
      const int64_t value = to_int(origin.lookup(name));
      lowering.base_values[name] = value;
      lowering.src_coefficients[name] = 0;
      lowering.dst_coefficients[name] = 0;

      // Accepted deltas: the scalar's own step both ways, else a search; then
      // the farthest accepted in each direction, where a tile's shape would
      // change at the edge of its range.
      std::vector<std::pair<int64_t, Sample>> probes;
      auto probe = [&](int64_t delta) {
        if (delta == 0) return false;
        for (const auto& [seen, _] : probes) {
          if (seen == delta) return true;
        }
        ScalarEnv at = origin;
        at.define(name, value + delta);
        const auto got = sample(at);
        if (!got) return false;
        probes.emplace_back(delta, *got);
        return true;
      };
      const int64_t step = step_of(name);
      int64_t unit = step != 0 ? std::abs(step) : 0;
      if (step != 0) {
        probe(step);
        probe(-step);
      }
      if (probes.empty()) {
        for (int64_t delta : {int64_t{1}, int64_t{-1}, int64_t{2}}) {
          if (probe(delta) && unit == 0) unit = std::abs(delta);
        }
      }
      if (probes.empty()) {
        throw std::runtime_error("no accepted value of " + name +
                                 " but the emission one");
      }
      if (unit == 0) unit = std::abs(probes[0].first);
      for (const int64_t direction : {int64_t{1}, int64_t{-1}}) {
        int64_t good = 0;
        for (int64_t delta = direction * unit;
             std::abs(delta) <= (int64_t{1} << 22); delta *= 2) {
          ScalarEnv at = origin;
          at.define(name, value + delta);
          if (!sample(at)) break;
          good = delta;
        }
        probe(good);
      }

      const auto& [delta0, first_probe] = probes[0];
      const int64_t src_moved =
          static_cast<int64_t>(first_probe.copy.src - base.copy.src);
      const int64_t dst_moved =
          static_cast<int64_t>(first_probe.copy.dst - base.copy.dst);
      if (src_moved % delta0 != 0 || dst_moved % delta0 != 0) {
        throw std::runtime_error("moves by a fraction of a byte per unit of " +
                                 name);
      }
      lowering.src_coefficients[name] = src_moved / delta0;
      lowering.dst_coefficients[name] = dst_moved / delta0;
      for (const auto& [delta, got] : probes) {
        if (!same_shape(got.copy, base.copy)) {
          throw std::runtime_error("changes shape with " + name);
        }
        const auto predicted = predict({{name, delta}});
        if (!same_moves(canonical(expand(predicted)), got.moves)) {
          throw std::runtime_error("is not affine in " + name);
        }
      }
      combined[name] = probes[0].first;
    }

    if (combined.size() > 1) {
      ScalarEnv at = origin;
      for (const auto& [name, delta] : combined) {
        at.define(name, lowering.base_values[name] + delta);
      }
      const auto got = sample(at);
      if (got &&
          !same_moves(canonical(expand(predict(combined))), got->moves)) {
        throw std::runtime_error("is not affine in its scalars together");
      }
    }
    lowering.lowered = true;
  } catch (const std::exception& error) {
    lowering.lowered = false;
    lowering.reason = error.what();
  }
  return lowering;
}
