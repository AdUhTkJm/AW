#include "SearchIndex.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "Pinyin.h"

namespace aw::search::detail {

namespace {

// Match quality, best first. The query path always matches every query token;
// the kind only describes how tightly the tokens sit inside a field.
enum MatchKind : uint8_t {
  KIND_EXACT = 0,           // the query is the whole field
  KIND_PREFIX = 1,          // the match starts at the field's first token
  KIND_CONTAINS = 2,        // contiguous, but starts later in the field
  KIND_SUBSEQUENCE = 3,     // matched tokens have gaps between them
  KIND_INITIALS = 4,        // pinyin initials substring
  KIND_WORD_SUBSTRING = 5,  // a query token occurs inside an entry token
};

// Field priority used only to break ties of equal match quality.
enum FieldRank : uint8_t {
  RANK_EN = 0,
  RANK_ZH = 1,
  RANK_ID = 2,
};

// The variants of one handle are capped: a name with many polyphonic characters
// would otherwise explode combinatorially. Weight-ordered generation keeps the
// readings a human is most likely to have meant inside the cap.
constexpr uint32_t MAX_VARIANTS = 8;
constexpr uint32_t MAX_READINGS = 8;

struct ReadingList {
  std::string_view items[MAX_READINGS];
  uint8_t count = 0;
};

struct FieldMatch {
  uint8_t kind = 0;
  uint32_t position = 0;
  uint32_t span = 0;
};

struct Score {
  uint8_t kind;
  uint8_t variantWeight;  // non-primary readings the matched pinyin variant needed
  uint8_t field;
  uint32_t position;
  uint32_t span;
  uint32_t handle;

  bool operator<(const Score &other) const noexcept {
    if (kind != other.kind)
      return kind < other.kind;
    if (variantWeight != other.variantWeight)
      return variantWeight < other.variantWeight;
    if (field != other.field)
      return field < other.field;
    if (position != other.position)
      return position < other.position;
    if (span != other.span)
      return span < other.span;
    return handle < other.handle;
  }
};

SearchIndex gIndex;

// ASCII only. The Chinese bytes are copied untouched, which is what the arena
// fold already guarantees for the corpus.
void foldAscii(std::string &text) noexcept {
  for (char &c : text) {
    if (c >= 'A' && c <= 'Z')
      c = (char) (c - 'A' + 'a');
  }
}

// A byte belongs to a token unless it is an ASCII separator. Bytes >= 0x80 are
// kept, so a run of UTF-8 Chinese stays one token instead of a byte per token.
bool isTokenByte(unsigned char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80;
}

void tokenize(std::string_view text, std::vector<std::string_view> &out) {
  out.clear();
  size_t i = 0;
  while (i < text.size()) {
    if (!isTokenByte((unsigned char) text[i])) {
      i++;
      continue;
    }
    const size_t start = i;
    while (i < text.size() && isTokenByte((unsigned char) text[i]))
      i++;
    out.push_back(text.substr(start, i - start));
  }
}

uint32_t decodedCodepoint(std::string_view text, size_t &i) noexcept {
  const unsigned char lead = (unsigned char) text[i];
  if (lead < 0x80) {
    i += 1;
    return lead;
  }
  if ((lead & 0xE0) == 0xC0 && i + 1 < text.size()) {
    const uint32_t cp = ((lead & 0x1Fu) << 6) | ((unsigned char) text[i + 1] & 0x3Fu);
    i += 2;
    return cp;
  }
  if ((lead & 0xF0) == 0xE0 && i + 2 < text.size()) {
    const uint32_t cp = ((lead & 0x0Fu) << 12) | (((unsigned char) text[i + 1] & 0x3Fu) << 6) |
                        ((unsigned char) text[i + 2] & 0x3Fu);
    i += 3;
    return cp;
  }
  if ((lead & 0xF8) == 0xF0 && i + 3 < text.size()) {
    const uint32_t cp = ((lead & 0x07u) << 18) | (((unsigned char) text[i + 1] & 0x3Fu) << 12) |
                        (((unsigned char) text[i + 2] & 0x3Fu) << 6) |
                        ((unsigned char) text[i + 3] & 0x3Fu);
    i += 4;
    return cp;
  }
  // Not a valid lead byte: keep the byte so the scan makes progress.
  i += 1;
  return lead;
}

// The readings of one character. Only CJK characters reach the pinyin string:
// Latin inside an untranslated Chinese name is already covered by the English
// field and by the raw Chinese substring, and spelling it out here would turn
// every letter into an initial (so "td" would match "cut deepslate").
void collectReadings(uint32_t codepoint, ReadingList &out) noexcept {
  out.count = 0;
  if (const pinyin::PinyinEntry *entry = pinyin::find(codepoint)) {
    pinyin::forEachReading(*entry, [&out](std::string_view reading) {
      if (out.count < MAX_READINGS)
        out.items[out.count++] = reading;
    });
  }
}

void appendVariant(const std::vector<ReadingList> &chars, const std::vector<uint8_t> &combo,
                   SearchIndex &index) {
  PinyinVariant variant{};
  variant.textBegin = (uint32_t) index.pinyin.size();
  variant.boundaryBegin = (uint32_t) index.boundaries.size();
  variant.initialsBegin = (uint32_t) index.initials.size();
  uint8_t weight = 0;
  for (size_t i = 0; i < chars.size(); i++) {
    if (combo[i] != 0)
      weight++;
    const std::string_view reading = chars[i].items[combo[i]];
    index.boundaries.push_back((uint32_t) index.pinyin.size());
    index.pinyin.append(reading.data(), reading.size());
    index.initials.push_back(reading[0]);
  }
  variant.weight = weight;
  variant.textEnd = (uint32_t) index.pinyin.size();
  variant.boundaryEnd = (uint32_t) index.boundaries.size();
  variant.initialsEnd = (uint32_t) index.initials.size();
  index.variants.push_back(variant);
}

// Enumerates reading combinations with exactly `remaining` non-primary choices.
// The caller sweeps `remaining` upwards, so one changed character is tried
// before two, which is the more likely intent when the cap is hit.
void emitCombos(size_t position, uint32_t remaining, const std::vector<ReadingList> &chars,
                std::vector<uint8_t> &combo, SearchIndex &index, uint32_t &emitted) {
  if (emitted >= MAX_VARIANTS)
    return;
  if (position == chars.size()) {
    if (remaining == 0) {
      appendVariant(chars, combo, index);
      emitted++;
    }
    return;
  }
  combo[position] = 0;
  emitCombos(position + 1, remaining, chars, combo, index, emitted);
  if (remaining == 0)
    return;
  const uint8_t count = chars[position].count;
  for (uint8_t reading = 1; reading < count; reading++) {
    combo[position] = reading;
    emitCombos(position + 1, remaining - 1, chars, combo, index, emitted);
    if (emitted >= MAX_VARIANTS)
      return;
  }
  combo[position] = 0;
}

void buildPinyin(std::string_view chinese, SearchIndex &index, std::vector<ReadingList> &chars,
                 std::vector<uint8_t> &combo) {
  chars.clear();
  size_t i = 0;
  while (i < chinese.size()) {
    const uint32_t codepoint = decodedCodepoint(chinese, i);
    ReadingList readings;
    collectReadings(codepoint, readings);
    if (readings.count > 0)
      chars.push_back(readings);
  }
  combo.assign(chars.size(), 0);
  uint32_t emitted = 0;
  for (uint32_t weight = 0; (size_t) weight <= chars.size() && emitted < MAX_VARIANTS; weight++)
    emitCombos(0, weight, chars, combo, index, emitted);
}

void addTokens(std::string_view text, uint32_t base, std::vector<TokenRef> &out) {
  size_t i = 0;
  while (i < text.size()) {
    if (!isTokenByte((unsigned char) text[i])) {
      i++;
      continue;
    }
    const size_t start = i;
    while (i < text.size() && isTokenByte((unsigned char) text[i]))
      i++;
    out.push_back(TokenRef{base + (uint32_t) start, (uint32_t) (i - start)});
  }
}

uint32_t arenaOffset(const TextIndex &text, std::string_view view) noexcept {
  if (view.empty())
    return 0;
  return (uint32_t) (view.data() - text.arena.data());
}

bool startsWith(std::string_view text, std::string_view prefix) noexcept {
  return text.size() >= prefix.size() &&
         std::memcmp(text.data(), prefix.data(), prefix.size()) == 0;
}

// Rule 1 for a sparse field: every query token is a prefix of some entry token,
// and the entry tokens are used in order.
bool matchSparsePrefix(const TextIndex &text, const std::vector<TokenRef> &tokens, uint32_t begin,
                       uint32_t end, const std::vector<std::string_view> &query,
                       FieldMatch &out) noexcept {
  uint32_t index = begin;
  uint32_t previous = 0;
  uint32_t first = 0;
  uint32_t gaps = 0;
  bool allFull = true;
  for (size_t q = 0; q < query.size(); q++) {
    bool found = false;
    while (index < end) {
      const TokenRef &token = tokens[index];
      if (startsWith(std::string_view(text.arena.data() + token.offset, token.length), query[q])) {
        found = true;
        break;
      }
      index++;
    }
    if (!found)
      return false;
    if (q == 0)
      first = index;
    else if (index != previous + 1)
      gaps++;
    if (tokens[index].length != query[q].size())
      allFull = false;
    previous = index;
    index++;
  }
  out.position = first - begin;
  out.span = previous - first + 1;
  if (first == begin && gaps == 0 && allFull && previous + 1 == end)
    out.kind = KIND_EXACT;
  else if (first == begin && gaps == 0)
    out.kind = KIND_PREFIX;
  else if (gaps == 0)
    out.kind = KIND_CONTAINS;
  else
    out.kind = KIND_SUBSEQUENCE;
  return true;
}

// The weaker fallback: a query token only has to occur inside an entry token,
// which is what lets "stone" find "redstone" without letting a token span two
// entry tokens.
bool matchSparseSubstring(const TextIndex &text, const std::vector<TokenRef> &tokens,
                          uint32_t begin, uint32_t end,
                          const std::vector<std::string_view> &query, FieldMatch &out) noexcept {
  uint32_t index = begin;
  uint32_t previous = 0;
  uint32_t first = 0;
  for (size_t q = 0; q < query.size(); q++) {
    bool found = false;
    while (index < end) {
      const TokenRef &token = tokens[index];
      const std::string_view view(text.arena.data() + token.offset, token.length);
      if (view.find(query[q]) != std::string_view::npos) {
        found = true;
        break;
      }
      index++;
    }
    if (!found)
      return false;
    if (q == 0)
      first = index;
    previous = index;
    index++;
  }
  out.kind = KIND_WORD_SUBSTRING;
  out.position = first - begin;
  out.span = previous - first + 1;
  return true;
}

// Direct Chinese search: contiguous bytes, so 暗铁 does not match 暗影铁锭.
bool matchChineseRaw(const TextIndex &text, uint32_t offset,
                     const std::vector<std::string_view> &query, FieldMatch &out) noexcept {
  if (offset == CHINESE_ABSENT)
    return false;
  const std::string_view name(text.arena.data() + offset);
  size_t cursor = 0;
  size_t previousEnd = 0;
  uint32_t first = 0;
  bool adjacent = true;
  for (size_t q = 0; q < query.size(); q++) {
    const size_t at = name.find(query[q], cursor);
    if (at == std::string_view::npos)
      return false;
    if (q == 0)
      first = (uint32_t) at;
    else if (at != previousEnd)
      adjacent = false;
    previousEnd = at + query[q].size();
    cursor = previousEnd;
  }
  out.position = first;
  out.span = (uint32_t) (previousEnd - first);
  if (query.size() == 1 && first == 0 && out.span == name.size())
    out.kind = KIND_EXACT;
  else if (first == 0)
    out.kind = KIND_PREFIX;
  else if (query.size() == 1 || adjacent)
    out.kind = KIND_CONTAINS;
  else
    out.kind = KIND_SUBSEQUENCE;
  return true;
}

// The heart of rule 1 for pinyin: a query token may only start where a
// character starts, but it may end in the middle of the next syllable.
bool matchPinyin(const SearchIndex &index, const PinyinVariant &variant,
                 const std::vector<std::string_view> &query, FieldMatch &out) noexcept {
  const char *base = index.pinyin.data();
  uint32_t boundary = variant.boundaryBegin;
  uint32_t first = 0;
  uint32_t previousEnd = 0;
  bool adjacent = true;
  for (size_t q = 0; q < query.size(); q++) {
    bool found = false;
    while (boundary < variant.boundaryEnd) {
      const uint32_t offset = index.boundaries[boundary];
      if (query[q].size() <= variant.textEnd - offset &&
          std::memcmp(base + offset, query[q].data(), query[q].size()) == 0) {
        found = true;
        break;
      }
      boundary++;
    }
    if (!found)
      return false;
    if (q == 0)
      first = boundary;
    else if (index.boundaries[boundary] != previousEnd)
      adjacent = false;
    previousEnd = index.boundaries[boundary] + (uint32_t) query[q].size();
    boundary++;
  }
  out.position = first - variant.boundaryBegin;
  out.span = previousEnd - index.boundaries[first];
  if (first == variant.boundaryBegin && query.size() == 1 &&
      out.span == variant.textEnd - variant.textBegin)
    out.kind = KIND_EXACT;
  else if (first == variant.boundaryBegin)
    out.kind = KIND_PREFIX;
  else if (query.size() == 1 || adjacent)
    out.kind = KIND_CONTAINS;
  else
    out.kind = KIND_SUBSEQUENCE;
  return true;
}

// Rule 2: the query is a substring of the pinyin initials.
bool matchInitials(const SearchIndex &index, const PinyinVariant &variant, std::string_view query,
                   FieldMatch &out) noexcept {
  const std::string_view initials(index.initials.data() + variant.initialsBegin,
                                  variant.initialsEnd - variant.initialsBegin);
  const size_t at = initials.find(query);
  if (at == std::string_view::npos)
    return false;
  out.kind = KIND_INITIALS;
  out.position = (uint32_t) at;
  out.span = (uint32_t) query.size();
  return true;
}

void consider(Score &best, bool &any, uint8_t kind, uint8_t field, uint32_t position,
              uint32_t span, uint32_t handle, uint8_t variantWeight = 0) noexcept {
  const Score candidate{kind, variantWeight, field, position, span, handle};
  if (!any || candidate < best) {
    best = candidate;
    any = true;
  }
}

bool matchHandle(const TextIndex &text, uint32_t handle,
                 const std::vector<std::string_view> &query, bool queryHasChinese,
                 Score &out) {
  const SearchIndex &index = gIndex;
  const HandleSpans &spans = index.handles[handle];
  Score best{};
  bool any = false;
  FieldMatch match;

  if (matchSparsePrefix(text, index.enTokens, spans.enBegin, spans.enEnd, query, match))
    consider(best, any, match.kind, RANK_EN, match.position, match.span, handle);
  else if (query.size() == 1 &&
           matchSparseSubstring(text, index.enTokens, spans.enBegin, spans.enEnd, query, match))
    consider(best, any, match.kind, RANK_EN, match.position, match.span, handle);

  // A raw Chinese match is only meaningful for a query that actually contains
  // Chinese. An ASCII query against an untranslated name would otherwise match
  // the Latin text twice, once weakly through English and once strongly here.
  if (queryHasChinese && matchChineseRaw(text, spans.chineseOffset, query, match))
    consider(best, any, match.kind, RANK_ZH, match.position, match.span, handle);
  for (uint32_t v = spans.variantBegin; v < spans.variantEnd; v++) {
    if (matchPinyin(index, index.variants[v], query, match))
      consider(best, any, match.kind, RANK_ZH, match.position, match.span, handle,
               index.variants[v].weight);
  }
  if (query.size() == 1) {
    for (uint32_t v = spans.variantBegin; v < spans.variantEnd; v++) {
      if (matchInitials(index, index.variants[v], query[0], match))
        consider(best, any, match.kind, RANK_ZH, match.position, match.span, handle,
                 index.variants[v].weight);
    }
  }

  if (matchSparsePrefix(text, index.idTokens, spans.idBegin, spans.idEnd, query, match))
    consider(best, any, match.kind, RANK_ID, match.position, match.span, handle);
  else if (query.size() == 1 &&
           matchSparseSubstring(text, index.idTokens, spans.idBegin, spans.idEnd, query, match))
    consider(best, any, match.kind, RANK_ID, match.position, match.span, handle);

  if (!any)
    return false;
  out = best;
  return true;
}

}  // namespace

void buildSearchIndex(const TextIndex &text) {
  SearchIndex index;
  if (!text.empty())
    return;
  
  index.empty = false;
  index.handles.resize(text.handleCount + 1);
  std::vector<ReadingList> chars;
  std::vector<uint8_t> combo;
  for (uint32_t handle = 1; handle <= text.handleCount; handle++) {
    HandleSpans &spans = index.handles[handle];

    const std::string_view id = text.text(handle, RESOURCE_ID);
    spans.idBegin = (uint32_t) index.idTokens.size();
    addTokens(id, arenaOffset(text, id), index.idTokens);
    spans.idEnd = (uint32_t) index.idTokens.size();

    const std::string_view english = text.text(handle, ENGLISH_NAME);
    spans.enBegin = (uint32_t) index.enTokens.size();
    addTokens(english, arenaOffset(text, english), index.enTokens);
    spans.enEnd = (uint32_t) index.enTokens.size();

    const std::string_view chinese = text.text(handle, CHINESE_NAME);
    spans.chineseOffset = chinese.empty() ? CHINESE_ABSENT : arenaOffset(text, chinese);

    spans.variantBegin = (uint32_t) index.variants.size();
    buildPinyin(chinese, index, chars, combo);
    spans.variantEnd = (uint32_t) index.variants.size();
  }
  gIndex = std::move(index);
}

const SearchIndex &getSearchIndex() noexcept {
  return gIndex;
}

std::shared_mutex &indexMutex() noexcept {
  static std::shared_mutex mutex;
  return mutex;
}

std::vector<uint32_t> runSearch(std::string_view query, uint32_t limit) {
  std::vector<uint32_t> result;
  const std::shared_lock<std::shared_mutex> lock(indexMutex());
  if (limit == 0 || gIndex.empty)
    return result;

  std::string folded(query);
  foldAscii(folded);
  std::vector<std::string_view> tokens;
  tokenize(folded, tokens);
  if (tokens.empty())
    return result;

  bool queryHasChinese = false;
  for (size_t i = 0; i < tokens.size() && !queryHasChinese; i++)
    for (char c : tokens[i])
      if ((unsigned char) c >= 0x80) {
        queryHasChinese = true;
        break;
      }

  const TextIndex &text = getTextIndex();
  std::vector<Score> matches;
  for (uint32_t handle = 1; handle <= text.handleCount; handle++) {
    Score score;
    if (matchHandle(text, handle, tokens, queryHasChinese, score))
      matches.push_back(score);
  }
  if (matches.empty())
    return result;

  const size_t count = std::min<size_t>(matches.size(), limit);
  std::nth_element(matches.begin(), matches.begin() + (std::ptrdiff_t) count, matches.end());
  matches.resize(count);
  std::sort(matches.begin(), matches.end());
  result.reserve(count);
  for (const Score &score : matches)
    result.push_back(score.handle);
  return result;
}

}  // namespace aw::search::detail

namespace aw::search {

std::vector<uint32_t> search(std::string_view query, uint32_t limit) {
  return detail::runSearch(query, limit);
}

}  // namespace aw::search
