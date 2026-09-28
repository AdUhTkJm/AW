#ifndef AW_SEARCH_TEXT_INDEX_H
#define AW_SEARCH_TEXT_INDEX_H

// The native side of the item search index.
//
// The mod owns two languages at most (en_US and zh_CN; other locales are not
// supported), so the corpus is a fixed list of strings per resource handle:
//
//   field 0: the resource location, e.g. "minecraft:stone"
//   field 1: the en_US display name
//   field 2: the zh_CN display name
//
// A future pinyin field is appended to the end of that list; nothing about the
// transfer format depends on the count, which is why `registerTextIndex` takes
// it as a parameter. Only the field order is a contract between the two sides.
//
// Handles are the planner's 1-based resource handles, contiguous from 1 to
// `handleCount`. That is the same numbering the recipe blob and the plan
// protocol use, so a search result can be fed straight to a plan request.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "aw/utils/PodVector.h"

namespace aw::search {

// Field order inside one handle's block. `FIELD_COUNT` is the count the mod
// currently sends; the native side accepts any positive count.
enum Field : uint32_t {
  RESOURCE_ID = 0,
  ENGLISH_NAME = 1,
  CHINESE_NAME = 2,
  FIELD_COUNT = 3,
};

// The search corpus, owned by the native side.
//
// `arena` holds every string back to back, each one '\0'-terminated, with runs
// of ASCII upper case folded to lower case. Folding is byte-length-preserving,
// so the offsets stay valid, and multi-byte UTF-8 never contains an ASCII
// upper-case byte, so the Chinese names are copied untouched.
//
// `offsets[i]` is the start of the i-th string, indexed by
// `(handle - 1) * fields + field`. Handle 0 is intentionally unused.
struct TextIndex {
  aw::vector<char> arena;
  aw::vector<uint32_t> offsets;
  uint32_t fields = 0;
  uint32_t handleCount = 0;

  [[nodiscard]] bool empty() const noexcept {
    return handleCount == 0;
  }

  // The string at a raw offset-array index, or an empty view when out of range.
  [[nodiscard]] std::string_view at(uint32_t index) const noexcept {
    if (index >= offsets.size())
      return {};
    return std::string_view(arena.data() + offsets[index]);
  }

  // The string for a resource handle and a field. An unknown handle or field
  // yields an empty view rather than reading past the corpus.
  [[nodiscard]] std::string_view text(uint32_t handle, uint32_t field) const noexcept {
    if (handle == 0 || handle > handleCount || field >= fields)
      return {};
    return at((handle - 1) * fields + field);
  }
};

// Replaces the global index with the corpus described by `chunks`.
//
// `chunks[i]` is a direct buffer of `chunkSizes[i]` bytes; the buffers are
// concatenated in order and form one logical byte stream that `offsets` indexes
// into. A string never straddles a chunk, so no offset ever needs remapping.
// The bytes are copied, so the caller may release the buffers as soon as the
// call returns.
//
// Returns false and fills `error` when the description is malformed: an empty
// corpus, offsets that are not strictly ascending from zero, an offset outside
// the arena, or a string that is not terminated inside it. On failure the
// previous index is left untouched. An empty `offsets` (and no chunks) is a
// valid way to clear the index.
bool registerTextIndex(std::span<const std::byte *> chunks,
                       std::span<const std::size_t> chunkSizes,
                       std::span<const uint32_t> offsets,
                       uint32_t fieldsPerHandle, std::string &error) noexcept;

// The process-wide index. Empty until the first successful registration.
const TextIndex &getTextIndex() noexcept;

}  // namespace aw::search

#endif
