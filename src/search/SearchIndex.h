#ifndef AW_SEARCH_SEARCH_INDEX_H
#define AW_SEARCH_SEARCH_INDEX_H

// Private, derived form of the search corpus.
//
// TextIndex owns the raw strings the mod sends; SearchIndex owns everything the
// query path needs that is expensive to compute per keystroke: the tokens of
// the resource location and the English name, and for the Chinese name the
// pinyin variants (one per legal reading combination) together with the
// syllable boundaries and initials they imply.
//
// `buildSearchIndex` derives the whole thing from a loaded TextIndex and
// replaces the process-wide instance. It is called once per registration, so it
// may allocate freely; the query path may not.

#include <cstdint>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "aw/search/TextIndex.h"

namespace aw::search::detail {

// A token of a sparse field (resource location or English name): a view into
// the TextIndex arena. Tokens are never copied.
struct TokenRef {
  uint32_t offset;
  uint32_t length;
};

// One pinyin spelling of a Chinese name. Reading combinations are materialised
// as separate variants rather than matched by an NFA.
//
// `text` is [textBegin, textEnd) of SearchIndex::pinyin. `boundaries` holds one
// byte offset per character into that same blob, so a query token may only start
// at a character boundary; that is what stops "ied" from matching "tieding".
// `initials` is [initialsBegin, initialsEnd) of SearchIndex::initials. `weight`
// is how many characters use a non-primary reading, so a primary match can
// outrank one that needed an alternate.
struct PinyinVariant {
  uint32_t textBegin;
  uint32_t textEnd;
  uint32_t boundaryBegin;  // index into SearchIndex::boundaries
  uint32_t boundaryEnd;
  uint32_t initialsBegin;
  uint32_t initialsEnd;
  uint8_t weight;
};

// Per-handle ranges and offsets. Index 0 is unused: handles are 1-based.
struct HandleSpans {
  uint32_t idBegin;
  uint32_t idEnd;
  uint32_t enBegin;
  uint32_t enEnd;
  uint32_t chineseOffset;  // CHINESE_NAME_ABSENT when the name is empty
  uint32_t variantBegin;
  uint32_t variantEnd;
};

inline constexpr uint32_t kChineseAbsent = 0xFFFFFFFFu;

struct SearchIndex {
  std::vector<TokenRef> idTokens;
  std::vector<TokenRef> enTokens;
  std::vector<HandleSpans> handles;
  std::vector<PinyinVariant> variants;
  std::vector<uint32_t> boundaries;
  std::string pinyin;
  std::string initials;
  bool empty = true;
};

// Derives the index from `text` and installs it. An empty `text` clears it.
void buildSearchIndex(const TextIndex &text);

// The installed index, read by the query path.
const SearchIndex &getSearchIndex() noexcept;

// Serialises registration (exclusive) against queries (shared). Registration
// runs on the planner worker thread; search runs on the render thread.
std::shared_mutex &indexMutex() noexcept;

}  // namespace aw::search::detail

#endif  // AW_SEARCH_SEARCH_INDEX_H
