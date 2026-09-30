// Unit tests for the native search corpus loader. A tiny assertion helper keeps
// the WSL build dependency-free, matching the rest of the tree.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <sstream> // IWYU pragma: keep
#include <fstream> // IWYU pragma: keep

#include "aw/search/TextIndex.h"

namespace {

int failures = 0;

void expect(bool condition, const char *what) {
  if (!condition) {
    std::cout << "  [FAIL] " << what << '\n';
    ++failures;
  }
}

void expectEq(std::string_view actual, std::string_view expected, const char *what) {
  if (actual != expected) {
    std::cout << "  [FAIL] " << what << ": expected '" << std::string(expected)
              << "', got '" << std::string(actual) << "'\n";
    ++failures;
  }
}

// Appends a string with its terminating NUL and records its offset, the way the
// mod's StringPool does.
void appendString(std::vector<std::byte> &arena, std::vector<uint32_t> &offsets,
                  std::string_view text) {
  offsets.push_back((uint32_t) arena.size());
  arena.insert(arena.end(), (const std::byte *) text.data(),
               (const std::byte *) text.data() + text.size());
  arena.push_back((std::byte) 0);
}

// Registers `arena` split into chunks of at most `chunkBytes`. Chunk boundaries
// may fall inside a string here; the native side only sees offsets into the
// concatenation, so this is a stronger test than a real StringPool produces.
bool registerSplit(const std::vector<std::byte> &arena,
                   const std::vector<uint32_t> &offsets, uint32_t fields,
                   size_t chunkBytes, std::string &error) {
  std::vector<const std::byte *> chunks;
  std::vector<std::size_t> sizes;
  for (size_t at = 0; at < arena.size(); at += chunkBytes) {
    const size_t size = std::min(chunkBytes, arena.size() - at);
    chunks.push_back(arena.data() + at);
    sizes.push_back(size);
  }
  return aw::search::registerTextIndex(chunks, sizes, offsets, fields, error);
}

// Two handles of three fields each, including a multi-byte Chinese name.
void makeCorpus(std::vector<std::byte> &arena, std::vector<uint32_t> &offsets) {
  appendString(arena, offsets, "minecraft:stone");
  appendString(arena, offsets, "Stone");
  appendString(arena, offsets, "\xE7\x9F\xB3\xE5\xA4\xB4");  // 石头
  appendString(arena, offsets, "minecraft:dirt");
  appendString(arena, offsets, "Dirt");
  appendString(arena, offsets, "\xE6\xB3\xA5\xE5\x9C\x9F");  // 泥土
}

void checkCorpus() {
  const auto &index = aw::search::getTextIndex();
  expect(index.handleCount == 2, "two handles");
  expect(index.fields == 3, "three fields");
  expectEq(index.text(1, aw::search::RESOURCE_ID), "minecraft:stone", "handle 1 id");
  expectEq(index.text(1, aw::search::ENGLISH_NAME), "stone", "handle 1 English folded");
  expectEq(index.text(1, aw::search::CHINESE_NAME),
           "\xE7\x9F\xB3\xE5\xA4\xB4", "handle 1 Chinese preserved");
  expectEq(index.text(2, aw::search::RESOURCE_ID), "minecraft:dirt", "handle 2 id");
  expectEq(index.text(2, aw::search::ENGLISH_NAME), "dirt", "handle 2 English folded");
  expectEq(index.text(2, aw::search::CHINESE_NAME),
           "\xE6\xB3\xA5\xE5\x9C\x9F", "handle 2 Chinese preserved");
  expect(index.text(3, aw::search::RESOURCE_ID).empty(), "out-of-range handle is empty");
  expect(index.text(1, 9).empty(), "out-of-range field is empty");
  expect(index.text(0, 0).empty(), "handle 0 is empty");
}

void testSingleChunk() {
  std::vector<std::byte> arena;
  std::vector<uint32_t> offsets;
  makeCorpus(arena, offsets);

  std::string error;
  expect(registerSplit(arena, offsets, 3, arena.size(), error), "single-chunk registration");
  checkCorpus();
}

// The same corpus cut at awkward places, including inside a multi-byte
// character, must produce identical strings.
void testChunked() {
  std::vector<std::byte> arena;
  std::vector<uint32_t> offsets;
  makeCorpus(arena, offsets);

  std::string error;
  expect(registerSplit(arena, offsets, 3, 7, error), "chunked registration");
  checkCorpus();
}

void testRejectsMalformed() {
  std::vector<std::byte> arena;
  std::vector<uint32_t> offsets;
  appendString(arena, offsets, "ab");
  appendString(arena, offsets, "cd");
  appendString(arena, offsets, "ef");

  std::string error;
  expect(!registerSplit(arena, offsets, 2, arena.size(), error),
         "offset count must be a multiple of the field count");
  expect(!error.empty(), "a rejection sets an error message");
  expect(!registerSplit(arena, offsets, 0, arena.size(), error),
         "field count must be positive");

  std::vector<uint32_t> badStart = offsets;
  badStart[0] = 1;
  expect(!registerSplit(arena, badStart, 3, arena.size(), error),
         "offsets must start at zero");

  std::vector<uint32_t> notAscending = offsets;
  notAscending[1] = notAscending[0];
  expect(!registerSplit(arena, notAscending, 3, arena.size(), error),
         "offsets must ascend strictly");

  std::vector<uint32_t> outOfBounds = offsets;
  outOfBounds[1] = (uint32_t) arena.size() + 10;
  expect(!registerSplit(arena, outOfBounds, 3, arena.size(), error),
         "offsets must stay inside the arena");

  std::vector<std::byte> unterminated = arena;
  unterminated.pop_back();
  expect(!registerSplit(unterminated, offsets, 3, unterminated.size(), error),
         "every string must be terminated");
}

void testFailureKeepsPrevious() {
  std::vector<std::byte> arena;
  std::vector<uint32_t> offsets;
  appendString(arena, offsets, "keep");

  std::string error;
  expect(registerSplit(arena, offsets, 1, arena.size(), error), "a valid index registers");
  expectEq(aw::search::getTextIndex().text(1, 0), "keep", "index before the failure");

  const std::vector<uint32_t> bad = {1};
  std::vector<const std::byte *> chunks = {arena.data()};
  std::vector<std::size_t> sizes = {arena.size()};
  expect(!aw::search::registerTextIndex(chunks, sizes, bad, 1, error),
         "a malformed index is rejected");
  expectEq(aw::search::getTextIndex().text(1, 0), "keep", "the index survives a failure");
}

void testClear() {
  std::vector<std::byte> arena;
  std::vector<uint32_t> offsets;
  appendString(arena, offsets, "x");

  std::string error;
  expect(registerSplit(arena, offsets, 1, arena.size(), error), "a non-empty index registers");

  std::vector<const std::byte *> noChunks;
  std::vector<std::size_t> noSizes;
  std::vector<uint32_t> noOffsets;
  expect(aw::search::registerTextIndex(noChunks, noSizes, noOffsets, 1, error),
         "an empty corpus clears the index");
  expect(aw::search::getTextIndex().empty(), "the index is empty after clearing");
}

#ifdef AW_SEARCH_TEST_DATA

std::string slurp(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// Splits a line on tabs. Returns false when it does not have `count` fields.
bool splitFields(const std::string &line, std::size_t count, std::vector<std::string> &out) {
  out.clear();
  std::size_t start = 0;
  while (true) {
    const std::size_t tab = line.find('\t', start);
    if (tab == std::string::npos) {
      out.push_back(line.substr(start));
      break;
    }
    out.push_back(line.substr(start, tab - start));
    start = tab + 1;
  }
  return out.size() == count;
}

uint32_t parseU32(const std::string &text) {
  uint32_t value = 0;
  for (char c : text)
    if (c >= '0' && c <= '9')
      value = value * 10 + (uint32_t) (c - '0');
  return value;
}

std::string join(const std::vector<uint32_t> &handles) {
  std::string out;
  for (std::size_t i = 0; i < handles.size(); i++) {
    if (i != 0)
      out += ',';
    out += std::to_string(handles[i]);
  }
  return out;
}

// Loads the committed corpus in the mod's name-table format, registers it
// through the same chunked path the JNI bridge uses, and checks every golden
// query in order. This is the one place the matching and ranking rules are
// pinned down, so the fixture stays small enough to read.
void testCorpusSearch() {
  const std::string base(AW_SEARCH_TEST_DATA);
  const std::string corpus = slurp(base + "/search-corpus.tsv");
  const std::string queries = slurp(base + "/search-queries.tsv");
  if (corpus.empty() || queries.empty()) {
    std::cout << "  [FAIL] missing search fixture under " << base << '\n';
    ++failures;
    return;
  }

  std::vector<std::byte> arena;
  std::vector<uint32_t> offsets;
  std::vector<std::string> fields;
  uint32_t expectedHandle = 1;
  std::istringstream corpusStream(corpus);
  std::string line;
  while (std::getline(corpusStream, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    if (!splitFields(line, 4, fields)) {
      expect(false, "a corpus line must have four fields");
      continue;
    }
    expect(parseU32(fields[0]) == expectedHandle, "corpus handles must be consecutive from 1");
    appendString(arena, offsets, fields[1]);
    appendString(arena, offsets, fields[2]);
    appendString(arena, offsets, fields[3]);
    expectedHandle++;
  }

  std::string error;
  expect(registerSplit(arena, offsets, 3, 7, error), "the golden corpus registers (chunked)");

  std::istringstream queryStream(queries);
  while (std::getline(queryStream, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    if (!splitFields(line, 2, fields)) {
      expect(false, "a query line must have two fields");
      continue;
    }
    const std::vector<uint32_t> hits = aw::search::search(fields[0], 50);
    expectEq(join(hits), fields[1], fields[0].c_str());
  }
}

#endif  // AW_SEARCH_TEST_DATA

}  // namespace

int main() {
  std::cout << "search index\n";
  testSingleChunk();
  testChunked();
  testRejectsMalformed();
  testFailureKeepsPrevious();
  testClear();
#ifdef AW_SEARCH_TEST_DATA
  std::cout << "search queries\n";
  testCorpusSearch();
#endif

  if (failures == 0) {
    std::cout << "all tests passed\n";
    return 0;
  }
  std::cout << failures << " check(s) failed\n";
  return 1;
}
