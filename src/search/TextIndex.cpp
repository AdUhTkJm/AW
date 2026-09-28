#include "aw/search/TextIndex.h"

#include <cstring>

namespace aw::search {

namespace {

TextIndex gTextIndex;

// ASCII-only lower casing; see the header for why this is length preserving.
void foldAscii(char *text, size_t size) noexcept {
  for (size_t i = 0; i < size; i++) {
    if (text[i] >= 'A' && text[i] <= 'Z')
      text[i] = (char) (text[i] - 'A' + 'a');
  }
}

}  // namespace

bool registerTextIndex(std::span<const std::byte *> chunks,
                       std::span<const std::size_t> chunkSizes,
                       std::span<const uint32_t> offsets,
                       uint32_t fieldsPerHandle, std::string &error) noexcept {
  error.clear();

  if (chunks.size() != chunkSizes.size()) {
    error = "the chunk and chunk-size counts disagree";
    return false;
  }
  if (fieldsPerHandle == 0) {
    error = "the field count must be positive";
    return false;
  }
  if (offsets.size() % fieldsPerHandle != 0) {
    error = "the offset count is not a multiple of the field count";
    return false;
  }

  // An empty corpus clears the index. This is the only case with no offsets.
  if (offsets.empty()) {
    gTextIndex = TextIndex{};
    return true;
  }

  // Offsets are 32-bit, so the arena has to fit in 32 bits as well. A corpus
  // this large is a Java-side bug, not a realistic pack.
  std::size_t total = 0;
  for (std::size_t size : chunkSizes) {
    if (size > (std::size_t) UINT32_MAX || total > (std::size_t) UINT32_MAX - size) {
      error = "the search corpus is larger than a 32-bit offset can address";
      return false;
    }
    total += size;
  }
  if (total == 0) {
    error = "an empty corpus cannot carry offsets";
    return false;
  }

  TextIndex index;
  index.fields = fieldsPerHandle;
  index.handleCount = (uint32_t) (offsets.size() / fieldsPerHandle);
  index.arena.resize((uint32_t) total);
  index.offsets = aw::vector<uint32_t>(offsets.begin(), offsets.end());

  // The chunks are one logical stream. Concatenating them recreates exactly the
  // byte positions the offsets were computed against.
  std::size_t written = 0;
  for (std::size_t i = 0; i < chunks.size(); i++) {
    if (chunks[i] == nullptr) {
      error = "a chunk has no direct buffer address";
      return false;
    }
    std::memcpy(index.arena.data() + written, chunks[i], chunkSizes[i]);
    written += chunkSizes[i];
  }
  foldAscii(index.arena.data(), index.arena.size());

  // Ascending from zero, inside the arena, and terminated. The last check is
  // what makes the string_view returned by `at` safe.
  const char *base = index.arena.data();
  uint32_t previous = 0;
  for (size_t i = 0; i < index.offsets.size(); i++) {
    const uint32_t offset = index.offsets[i];
    if (i == 0 ? offset != 0 : offset <= previous) {
      error = "string offsets must be strictly ascending and start at zero";
      return false;
    }
    if (offset >= total) {
      error = "a string offset is outside the corpus";
      return false;
    }
    if (std::memchr(base + offset, '\0', total - offset) == nullptr) {
      error = "a string is not '\\0'-terminated inside the corpus";
      return false;
    }
    previous = offset;
  }

  gTextIndex = index;
  return true;
}

const TextIndex &getTextIndex() noexcept {
  return gTextIndex;
}

}  // namespace aw::search
