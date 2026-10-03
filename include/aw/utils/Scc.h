#ifndef AW_UTILS_SCC_H
#define AW_UTILS_SCC_H

#include <cstdint>
#include <span>

namespace aw {

// One Tarjan strongly-connected-component pass over a CSR digraph.
//
// `offsets` holds n + 1 entries and `targets` holds offsets[n] node ids, so the
// out-edges of node v are targets[offsets[v] .. offsets[v + 1]). Node ids run
// from 0 to n - 1 and `component` is indexed by node.
//
// Components are numbered in reverse topological order: an edge a -> b whose
// ends lie in different components has component[b] < component[a]. Within that
// constraint the ids follow the order in which Tarjan closes the components.
//
// Both overloads leave a node they do not reach untouched, so the caller can
// pre-fill `component` with a sentinel; they return the number of components.
//
// The traversal is a plain recursion and runs on the large stack (aw::ls::call),
// so a graph far deeper than the calling thread's stack is still safe.
uint32_t computeSccs(std::span<const uint32_t> offsets,
                     std::span<const uint32_t> targets,
                     std::span<int32_t> component) noexcept;

// The same pass over the given roots only, in order: a root already reached
// from an earlier one is skipped. Used when a caller has a node subset in hand
// instead of a separate membership mask.
uint32_t computeSccs(std::span<const uint32_t> offsets,
                     std::span<const uint32_t> targets,
                     std::span<const uint32_t> roots,
                     std::span<int32_t> component) noexcept;

}

#endif
