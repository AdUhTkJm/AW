// Multi-item "wasteful pack" certificates.
//
// See docs/algorithm.typ. For a recipe R we look for a non-negative integer pack
// z with z_R = 1 such that every feasible plan that executes R at least once also
// executes the pack (z <= x), while the pack is a net loss (A z <= 0).
//
// Subtracting the pack keeps the plan feasible and strictly cheaper, so no optimal
// plan executes R.
//
// The bound z <= x is derived with three rules, applied to items assumed to
// have no stock (b = 0):
//
//   R1  If i has a single producer p of `a` units, then x_p >= ceil(need_i/a).
//   R2' If i has producers P, then for every input j of P,
//       consumed_j(x) >= min_p (j per unit of i of p) * consumed_i(x).
//   R3  If i has several producers, every feasible x has some producer p
//       supplying at least need_i/|P| of the demand. Branch on p with
//       x_p >= ceil(need_i / (|P| a_p)); the pointwise minimum over all leaves
//       is a valid lower bound.
//
// The doc's R3 ("each producer supplies all of the demand, accept when every
// branch closes, take the min") is unsound when production can split: a demand
// of 3 with two producers that each make 2 per run is met by one run of each,
// and that plan satisfies neither full-production branch. Dividing the demand
// by the branch count is what makes the disjunction complete.

#include <chrono>

#include "aw/plan/CraftingGraph.h"
#include "aw/plan/Options.h"
#include "aw/utils/Int128.h"

namespace aw::detail {
namespace {

using uint = uint32_t;

// GCC and Clang have the overflow builtins and define __has_builtin; MSVC has
// neither.
#if defined(__has_builtin)
#  if __has_builtin(__builtin_add_overflow) && __has_builtin(__builtin_mul_overflow)
#    define AW_HAVE_OVERFLOW_BUILTINS 1
#  endif
#endif

bool addOverflow(int64_t a, int64_t b, int64_t &out) noexcept {
#ifdef AW_HAVE_OVERFLOW_BUILTINS
  return __builtin_add_overflow(a, b, &out);
#else
  if (b > 0 ? a > INT64_MAX - b : a < INT64_MIN - b)
    return true;
  out = a + b;
  return false;
#endif
}

bool mulOverflow(int64_t a, int64_t b, int64_t &out) noexcept {
#ifdef AW_HAVE_OVERFLOW_BUILTINS
  return __builtin_mul_overflow(a, b, &out);
#else
  if (a == 0 || b == 0) {
    out = 0;
    return false;
  }
  const bool over = a > 0 ? (b > 0 ? a > INT64_MAX / b : b < INT64_MIN / a)
                          : (b > 0 ? a < INT64_MIN / b : a < INT64_MAX / b);
  if (over)
    return true;
  out = a * b;
  return false;
#endif
}

int64_t ceilDiv(int64_t n, int64_t d) noexcept {
  return n / d + (n % d != 0 ? 1 : 0);
}

// Compares n1 / d1 with n2 / d2 for non-negative numerators and positive
// denominators. Exact for every int64_t input: the cross products are formed in
// 128 bits, where |a * b| <= 2^126 can never overflow. Comparing the ratios in
// floating point instead would be wrong on MSVC, whose long double has the same
// 53-bit mantissa as double and could not separate two neighbouring fractions.
int compareRatio(int64_t n1, int64_t d1, int64_t n2, int64_t d2) noexcept {
  const aw::int128 left = (aw::int128) n1 * d2;
  const aw::int128 right = (aw::int128) n2 * d1;
  return left < right ? -1 : (left > right ? 1 : 0);
}

// num / den is the minimum over every producer of `item` of the amount of
// `item`'s input consumed per unit of `item` produced. Only inputs consumed by
// *all* producers appear, because an input one producer skips has rate 0.
struct Rate {
  NodeId input;
  int64_t num;
  int64_t den;
};

struct PackSearch {
  const CraftingGraph &graph;
  const PackPruneOptions &opt;

  // The pack z and the derived consumption bounds.
  aw::vector<int64_t> z;     // nRecipe
  aw::vector<int64_t> cons;  // nItem: consumption implied by z
  aw::vector<int64_t> need;  // nItem: max(cons, propagated lower bound)

  // Superset of recipes ever raised (never reverted; z decides at a leaf).
  aw::vector<uint32_t> support;
  aw::vector<uint8_t> inSupport;

  // Superset of items ever given a positive `need`.
  aw::vector<uint32_t> needList;
  aw::vector<uint8_t> inNeed;

  // Union of items whose balance was used as b = 0.
  aw::vector<uint8_t> inG;
  aw::vector<NodeId> zeroStock;

  // Per-item minimum input rates (CSR).
  aw::vector<uint32_t> rateOffsets;  // nItem + 1
  aw::vector<NodeId> rateInput;
  aw::vector<int64_t> rateNum;
  aw::vector<int64_t> rateDen;

  // True when no recipe consumes the item. A pack that produces such an item
  // can never have A z <= 0, so the search stops as soon as one is produced.
  aw::vector<uint8_t> unconsumed;  // nItem

  // Undo log, so branching can backtrack without copying the state.
  struct Undo {
    uint8_t kind;
    uint32_t index;
    int64_t old;
  };
  enum : uint8_t { kUndoZ, kUndoCons, kUndoNeed, kUndoNet };
  aw::vector<Undo> undo;

  int64_t totalZ = 0;
  uint64_t nodes = 0;
  bool aborted = false;     // discard the whole certificate
  bool budgetStop = false;  // stop refining, keep the current pack
  bool closed = false;      // A z <= 0 already
  bool deadlineHit = false;

  // Incremental A z. `positiveRows` counts rows of the current pack with a
  // positive net, so "closed" is an O(1) test; `deadRows` counts positive rows
  // nothing can ever consume, which prunes a branch that cannot close.
  int64_t positiveRows = 0;
  int64_t deadRows = 0;

  // Leaf accounting for the pointwise minimum.
  int64_t seenLeaves = 0;
  aw::vector<uint32_t> positiveCount;  // nRecipe
  aw::vector<int64_t> minPositive;     // nRecipe

  // Scratch.
  aw::vector<std::pair<NodeId, int64_t>> pending;
  aw::vector<int64_t> net;  // nItem
  aw::vector<uint32_t> netTouched;

  std::chrono::steady_clock::time_point passStart;

  PackSearch(const CraftingGraph &graph, const PackPruneOptions &opt) noexcept
      : graph(graph), opt(opt) {
    const uint nItem = graph.nItem;
    const uint nRecipe = graph.nRecipe;
    z.assign(nRecipe, 0);
    cons.assign(nItem, 0);
    need.assign(nItem, 0);
    inSupport.assign(nRecipe, 0);
    inNeed.assign(nItem, 0);
    inG.assign(nItem, 0);
    positiveCount.assign(nRecipe, 0);
    minPositive.assign(nRecipe, 0);
    net.assign(nItem, 0);
    unconsumed.assign(nItem, 1);
    // All four grow monotonically and are capped by the search budgets; rates
    // are bounded by the total number of input slots.
    support.reserve(opt.maxPackRecipes);
    needList.reserve(nItem);
    zeroStock.reserve(opt.maxZeroStockItems);
    rateInput.reserve(graph.r2i.targets.size());
    rateNum.reserve(graph.r2i.targets.size());
    rateDen.reserve(graph.r2i.targets.size());
  }

  bool timedOut() const noexcept {
    if (opt.maxSeconds <= 0.0)
      return false;
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - passStart).count();
    return seconds >= opt.maxSeconds;
  }

  // ---- Undo-managed writes ------------------------------------------------

  void writeZ(uint32_t r, int64_t value) noexcept {
    undo.push_back({kUndoZ, r, z[r]});
    z[r] = value;
  }

  void writeCons(NodeId i, int64_t value) noexcept {
    undo.push_back({kUndoCons, i, cons[i]});
    cons[i] = value;
  }

  void writeNeed(NodeId i, int64_t value) noexcept {
    if (value > options.maxNeed)
      value = options.maxNeed;
    if (value <= need[i])
      return;
    undo.push_back({kUndoNeed, i, need[i]});
    need[i] = value;
    if (!inNeed[i]) {
      inNeed[i] = 1;
      needList.push_back(i);
    }
  }

  void revert(size_t mark) noexcept {
    while (undo.size() > mark) {
      const Undo u = undo.back();
      undo.pop_back();
      if (u.kind == kUndoZ) {
        totalZ -= z[u.index] - u.old;
        z[u.index] = u.old;
      } else if (u.kind == kUndoCons) {
        cons[u.index] = u.old;
      } else if (u.kind == kUndoNeed) {
        need[u.index] = u.old;
      } else {
        const int64_t current = net[u.index];
        const bool was = current > 0, now = u.old > 0;
        if (was != now) {
          positiveRows += now ? 1 : -1;
          if (unconsumed[u.index])
            deadRows += now ? 1 : -1;
        }
        net[u.index] = u.old;
      }
    }
  }

  bool addG(NodeId i) noexcept {
    if (i >= graph.nReal || inG[i])
      return true;
    if ((uint32_t) zeroStock.size() >= opt.maxZeroStockItems) {
      aborted = true;
      return false;
    }
    inG[i] = 1;
    zeroStock.push_back(i);
    return true;
  }

  // Raises z_p to t, updating implied consumption and need. False means the
  // budget stopped the raise (budgetStop) or the search must be abandoned.
  bool raiseZ(uint32_t p, int64_t t) noexcept {
    const int64_t delta = t - z[p];
    if (delta <= 0)
      return true;
    if (!inSupport[p] && (uint32_t) support.size() >= opt.maxPackRecipes) {
      budgetStop = true;
      return false;
    }
    if (totalZ > opt.maxPackValue || delta > opt.maxPackValue - totalZ) {
      budgetStop = true;
      return false;
    }

    writeZ(p, t);
    totalZ += delta;
    if (!inSupport[p]) {
      inSupport[p] = 1;
      support.push_back(p);
    }

    // Update the pack's net production incrementally.
    int64_t produced = 0;
    int64_t next = 0;
    if (mulOverflow(graph.outputAmt[p], delta, produced) ||
        addOverflow(net[graph.output[p]], produced, next)) {
      aborted = true;
      return false;
    }
    netWrite(graph.output[p], next);

    const auto inputs = graph.r2i.targetsOf(p);
    const auto weights = graph.r2i.weightsOf(p);
    for (size_t k = 0; k < inputs.size(); k++) {
      int64_t scaled = 0;
      int64_t consumed = 0;
      if (mulOverflow(weights[k], delta, scaled) ||
          addOverflow(cons[inputs[k]], scaled, consumed)) {
        aborted = true;
        return false;
      }
      writeCons(inputs[k], consumed);
      writeNeed(inputs[k], consumed);

      int64_t reduced = 0;
      if (addOverflow(net[inputs[k]], -scaled, reduced)) {
        aborted = true;
        return false;
      }
      netWrite(inputs[k], reduced);
    }
    return true;
  }

  // ---- Incremental net production -----------------------------------------

  void netWrite(NodeId item, int64_t value) noexcept {
    if (value == net[item])
      return;
    undo.push_back({kUndoNet, item, net[item]});
    const bool was = net[item] > 0, now = value > 0;
    if (was != now) {
      positiveRows += now ? 1 : -1;
      if (unconsumed[item])
        deadRows += now ? 1 : -1;
    }
    net[item] = value;
  }

  void netAdd(NodeId item, int64_t delta) noexcept {
    if (delta == 0)
      return;
    int64_t value = 0;
    if (addOverflow(net[item], delta, value)) {
      aborted = true;
      return;
    }
    if (net[item] == 0)
      netTouched.push_back(item);
    net[item] = value;
  }

  void netClear() noexcept {
    for (NodeId item : netTouched)
      net[item] = 0;
    netTouched.clear();
  }

  void recordLeaf() noexcept {
    seenLeaves++;
    for (uint32_t r : support) {
      if (z[r] <= 0)
        continue;
      if (positiveCount[r] == 0 || z[r] < minPositive[r])
        minPositive[r] = z[r];
      positiveCount[r]++;
    }
  }

  // ---- Precomputed input rates --------------------------------------------

  void buildRates() noexcept {
    const uint nItem = graph.nItem;
    rateOffsets.assign(nItem + 1, 0);
    std::fill(unconsumed.begin(), unconsumed.end(), 1);
    for (uint r = 0; r < graph.nRecipe; r++)
      for (NodeId input : graph.r2i.targetsOf(r))
        unconsumed[input] = 0;

    struct Raw {
      NodeId input;
      uint32_t producer;
      int64_t amount;
    };
    aw::vector<Raw> raw;
    aw::vector<Rate> rates;

    for (NodeId i = 0; i < nItem; i++) {
      const auto producers = graph.i2r.targetsOf(i);
      rates.clear();

      if (!producers.empty()) {
        raw.clear();
        uint32_t usable = 0;
        for (NodeId producerNode : producers) {
          const uint32_t p = producerNode - graph.nItem;
          const int64_t a = graph.outputAmt[p];
          if (a <= 0)
            continue;
          usable++;
          const auto inputs = graph.r2i.targetsOf(p);
          const auto weights = graph.r2i.weightsOf(p);
          for (size_t k = 0; k < inputs.size(); k++)
            raw.push_back({inputs[k], p, weights[k]});
        }

        std::sort(raw.begin(), raw.end(), [](const Raw &a, const Raw &b) noexcept {
          if (a.input != b.input)
            return a.input < b.input;
          return a.producer < b.producer;
        });

        size_t k = 0;
        while (k < raw.size()) {
          const NodeId input = raw[k].input;
          size_t group = k;
          uint32_t consumers = 0;
          int64_t bestNum = 0, bestDen = 0;
          bool have = false;
          while (group < raw.size() && raw[group].input == input) {
            const uint32_t p = raw[group].producer;
            int64_t sum = 0;
            size_t e = group;
            while (e < raw.size() && raw[e].input == input && raw[e].producer == p) {
              int64_t next = 0;
              if (addOverflow(sum, raw[e].amount, next)) {
                aborted = true;
                return;
              }
              sum = next;
              e++;
            }
            const int64_t a = graph.outputAmt[p];
            if (!have || compareRatio(sum, a, bestNum, bestDen) < 0) {
              bestNum = sum;
              bestDen = a;
              have = true;
            }
            consumers++;
            group = e;
          }
          // A producer that skips this input makes the minimum rate 0.
          if (have && consumers == usable)
            rates.push_back({input, bestNum, bestDen});
          k = group;
        }
      }

      for (const Rate &r : rates) {
        rateInput.push_back(r.input);
        rateNum.push_back(r.num);
        rateDen.push_back(r.den);
      }
      rateOffsets[i + 1] = (uint32_t) rateInput.size();
    }
  }

  // ---- Rules --------------------------------------------------------------

  bool checkClosed() noexcept {
    if (positiveRows == 0) {
      closed = true;
      return true;
    }
    return false;
  }

  bool propagate() noexcept {
    if (aborted)
      return false;
    if (checkClosed())
      return true;

    int iterations = 0;
    bool changed = true;
    while (changed && deadRows == 0) {
      if (aborted)
        return false;
      if (budgetStop || deadlineHit)
        return true;
      if (++iterations > options.maxPropIterations)
        return true;
      changed = false;

      // R1: a unique producer must cover the demand.
      for (size_t k = 0; k < needList.size(); k++) {
        const NodeId i = needList[k];
        if (need[i] <= 0)
          continue;
        const auto producers = graph.i2r.targetsOf(i);
        if (producers.size() != 1)
          continue;
        const uint32_t p = producers[0] - graph.nItem;
        const int64_t a = graph.outputAmt[p];
        if (a <= 0)
          continue;
        const int64_t t = ceilDiv(need[i], a);
        if (t <= z[p])
          continue;
        if (!addG(i))
          return false;
        if (!raiseZ(p, t))
          return !aborted;
        changed = true;
        if (checkClosed())
          return true;
      }
      if (aborted)
        return false;

      // R2': propagate consumption through the producers' inputs.
      for (size_t k = 0; k < needList.size(); k++) {
        const NodeId i = needList[k];
        if (need[i] <= 0)
          continue;
        if (rateOffsets[i] == rateOffsets[i + 1])
          continue;

        pending.clear();
        for (uint32_t e = rateOffsets[i]; e < rateOffsets[i + 1]; e++) {
          int64_t product = 0;
          if (mulOverflow(rateNum[e], need[i], product)) {
            aborted = true;
            return false;
          }
          const int64_t t = product / rateDen[e];
          if (t > need[rateInput[e]])
            pending.push_back({rateInput[e], t});
        }
        if (pending.empty())
          continue;
        if (!addG(i))
          return false;
        for (const auto &update : pending)
          writeNeed(update.first, update.second);
        changed = true;
      }
      if (aborted)
        return false;
      if (checkClosed())
        return true;
    }
    return !aborted;
  }

  // ceil(need / (producers * output)).
  static int64_t share(int64_t demand, uint32_t producers, int64_t output) noexcept {
    if (demand <= 0 || producers == 0 || output <= 0)
      return 0;
    if ((uint64_t) producers > (uint64_t) INT64_MAX / (uint64_t) output)
      return 1;
    return ceilDiv(demand, (int64_t) producers * output);
  }

  bool branchable(NodeId i) const noexcept {
    const auto producers = graph.i2r.targetsOf(i);
    const uint32_t m = (uint32_t) producers.size();
    for (NodeId producerNode : producers) {
      const uint32_t p = producerNode - graph.nItem;
      const int64_t a = graph.outputAmt[p];
      if (a <= 0)
        continue;
      if (share(need[i], m, a) > z[p])
        return true;
    }
    return false;
  }

  void search(uint32_t depth) noexcept {
    if (aborted)
      return;
    if (deadlineHit) {
      recordLeaf();
      return;
    }
    if (++nodes > opt.maxBranchNodes) {
      recordLeaf();
      return;
    }
    if ((nodes & 0x3F) == 0 && timedOut()) {
      deadlineHit = true;
      recordLeaf();
      return;
    }

    if (!propagate())
      return;
    if (aborted)
      return;
    if (closed || budgetStop || deadRows > 0) {
      // `closed` is a certificate; the other two prune a branch that cannot
      // close. Recording the current pack keeps the leaf disjunction complete.
      recordLeaf();
      return;
    }
    if (depth >= opt.maxBranchDepth) {
      recordLeaf();
      return;
    }

    // Branch on the item with the fewest producers that can still be pushed.
    if (!std::getenv("AW_R3"))
      return;

    NodeId branchItem = UINT32_MAX;
    uint32_t branchProducers = UINT32_MAX;
    for (NodeId i : needList) {
      if (need[i] <= 0)
        continue;
      const uint32_t m = (uint32_t) graph.i2r.targetsOf(i).size();
      if (m < 2 || m >= branchProducers)
        continue;
      if (!branchable(i))
        continue;
      branchItem = i;
      branchProducers = m;
    }
    if (branchItem == UINT32_MAX) {
      recordLeaf();
      return;
    }

    if (!addG(branchItem))
      return;
    const int64_t demand = need[branchItem];
    const auto producers = graph.i2r.targetsOf(branchItem);
    const uint32_t m = (uint32_t) producers.size();

    bool recorded = false;
    for (NodeId producerNode : producers) {
      const uint32_t p = producerNode - graph.nItem;
      const int64_t a = graph.outputAmt[p];
      if (a <= 0)
        continue;
      const int64_t target = share(demand, m, a);
      if (target <= z[p]) {
        // This branch is already implied by the current lower bound.
        if (!recorded) {
          recordLeaf();
          recorded = true;
        }
        continue;
      }

      const size_t mark = undo.size();
      if (!raiseZ(p, target)) {
        if (aborted)
          return;
        // Budget: the current pack is still a valid bound for this branch.
        if (!recorded) {
          recordLeaf();
          recorded = true;
        }
        break;
      }
      search(depth + 1);
      revert(mark);
      if (aborted)
        return;
      if (budgetStop || deadlineHit) {
        // The remaining branches were not explored. The current pack is still
        // a valid lower bound for all of them, so recording it as a leaf keeps
        // the leaf disjunction complete.
        if (!recorded) {
          recordLeaf();
          recorded = true;
        }
        return;
      }
    }
  }

  // ---- Certificate assembly ----------------------------------------------

  bool buildCertificate(PackCertificate &out) noexcept {
    if (aborted || seenLeaves == 0)
      return false;

    aw::vector<std::pair<uint32_t, int64_t>> positive;
    for (uint32_t r : support)
      if (positiveCount[r] == seenLeaves && minPositive[r] > 0)
        positive.push_back({r, minPositive[r]});
    if (positive.empty())
      return false;
    std::sort(positive.begin(), positive.end());

    // The pointwise minimum over leaves need not inherit A z <= 0, so verify.
    for (const auto &entry : positive) {
      const uint32_t r = entry.first;
      const int64_t count = entry.second;
      int64_t produced = 0;
      if (mulOverflow(graph.outputAmt[r], count, produced)) {
        netClear();
        return false;
      }
      netAdd(graph.output[r], produced);
      const auto inputs = graph.r2i.targetsOf(r);
      const auto weights = graph.r2i.weightsOf(r);
      for (size_t k = 0; k < inputs.size(); k++) {
        int64_t consumed = 0;
        if (mulOverflow(weights[k], count, consumed)) {
          netClear();
          return false;
        }
        netAdd(inputs[k], -consumed);
      }
    }
    bool ok = !aborted;
    for (NodeId item : netTouched)
      if (net[item] > 0)
        ok = false;
    netClear();
    if (!ok)
      return false;

    out.support.clear();
    out.count.clear();
    out.support.reserve(positive.size());
    out.count.reserve(positive.size());
    for (const auto &entry : positive) {
      out.support.push_back_unchecked(entry.first);
      out.count.push_back_unchecked(entry.second);
    }
    out.zeroStock = zeroStock;
    std::sort(out.zeroStock.begin(), out.zeroStock.end());
    out.zeroStock.erase(std::unique(out.zeroStock.begin(), out.zeroStock.end()),
                        out.zeroStock.end());
    return true;
  }

  void cleanup() noexcept {
    for (uint32_t r : support) {
      positiveCount[r] = 0;
      minPositive[r] = 0;
      inSupport[r] = 0;
    }
    support.clear();
    for (NodeId i : needList)
      inNeed[i] = 0;
    needList.clear();
    for (NodeId i : zeroStock)
      inG[i] = 0;
    zeroStock.clear();
    netClear();
    undo.clear();
    totalZ = 0;
    positiveRows = 0;
    deadRows = 0;
    aborted = false;
    budgetStop = false;
    closed = false;
    nodes = 0;
    seenLeaves = 0;
  }

  bool run(uint32_t root) noexcept {
    cleanup();
    deadlineHit = false;

    if (!raiseZ(root, 1)) {
      revert(0);
      cleanup();
      return false;
    }
    search(0);
    // Reset z and the incremental net before verifying the certificate, which
    // accumulates A z from scratch into the same buffer.
    revert(0);
    const bool ok = buildCertificate(certScratch);
    cleanup();
    return ok;
  }

  PackCertificate certScratch;
};

}  // namespace

void computePackPruning(CraftingGraph &graph) noexcept {
  graph.packDominated.assign(graph.nRecipe, 0);
  graph.packCertificates.assign(graph.nRecipe, PackCertificate{});

  if (!options.pack.enabled || graph.nRecipe == 0)
    return;

  PackSearch search(graph, options.pack);
  search.buildRates();
  search.passStart = std::chrono::steady_clock::now();

  for (uint32_t r = 0; r < graph.nRecipe; r++) {
    // A recipe the other passes already drop needs no certificate.
    if (graph.tagEdgeDominated.size() == graph.nRecipe && graph.tagEdgeDominated[r])
      continue;
    if (graph.recipeDominated.size() == graph.nRecipe && graph.recipeDominated[r])
      continue;
    if (graph.recipeDirectDominated.size() == graph.nRecipe &&
        graph.recipeDirectDominated[r])
      continue;
    // A recipe whose output nothing consumes can never close A z <= 0.
    if (graph.output[r] < graph.nItem && search.unconsumed[graph.output[r]])
      continue;
    if (options.pack.maxSeconds > 0.0 && search.timedOut())
      break;

    if (search.run(r)) {
      graph.packDominated[r] = 1;
      graph.packCertificates[r] = search.certScratch;
    }
  }
}

}  // namespace aw
