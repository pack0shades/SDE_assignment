// Shared definitions: record format, on-page node layout, tree metadata, I/O counters.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#ifndef BPT_PAGE_SIZE
#define BPT_PAGE_SIZE 4096
#endif

namespace bpt {

constexpr size_t kPageSize = BPT_PAGE_SIZE;
constexpr size_t kKeySize = 8;   // fixed-width alphanumeric key, e.g. "K026A13E"
constexpr size_t kValSize = 64;  // fixed-width payload

// The 8 key characters packed big-endian into an integer, so that integer order is
// exactly the lexicographic (memcmp) order of the original string.
using Key = uint64_t;
using PageId = uint32_t;
constexpr PageId kNoPage = 0;  // page 0 is the file's meta page, so 0 doubles as "null"

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "key packing assumes a little-endian host");

inline Key make_key(const char* s) {
  uint64_t v;
  memcpy(&v, s, kKeySize);
  return __builtin_bswap64(v);
}

inline void key_chars(Key k, char* out) {
  k = __builtin_bswap64(k);
  memcpy(out, &k, kKeySize);
}

// Every node is exactly one page:
//
//   leaf     | header(8) | keys[kLeafCap] (8 each) | payloads[kLeafCap] (64 each) |
//   internal | header(8) | keys[kIntCap]  (8 each) | children[kIntCap + 1] (4 each) |
//
// Keys are kept in their own contiguous array so a binary search stays within a few
// cache lines instead of striding over the payloads.
struct NodeHeader {
  uint16_t is_leaf;
  uint16_t count;  // number of keys in the node
  PageId next;     // leaf: right sibling (kNoPage at the end); internal: unused
};

constexpr size_t kLeafCap = (kPageSize - sizeof(NodeHeader)) / (kKeySize + kValSize);
constexpr size_t kIntCap =
    (kPageSize - sizeof(NodeHeader) - sizeof(PageId)) / (kKeySize + sizeof(PageId));
static_assert(kLeafCap >= 2 && kIntCap >= 3, "page size too small");

inline NodeHeader* hdr(uint8_t* p) { return reinterpret_cast<NodeHeader*>(p); }
inline Key* node_keys(uint8_t* p) { return reinterpret_cast<Key*>(p + sizeof(NodeHeader)); }
inline uint8_t* leaf_vals(uint8_t* p) { return p + sizeof(NodeHeader) + kLeafCap * kKeySize; }
inline PageId* int_children(uint8_t* p) {
  return reinterpret_cast<PageId*>(p + sizeof(NodeHeader) + kIntCap * kKeySize);
}

// Tree-level state; persisted in page 0 by the file-backed pagers.
struct Meta {
  PageId root = kNoPage;
  uint32_t height = 0;  // number of levels, leaves included
  uint64_t num_keys = 0;
  uint64_t leaf_pages = 0;
  uint64_t internal_pages = 0;
};

struct IoStats {
  uint64_t page_reads = 0;   // pages read from the file
  uint64_t page_writes = 0;  // pages written to the file
  uint64_t cache_hits = 0;   // buffer pool only
  uint64_t cache_misses = 0;
  uint64_t evictions = 0;
};

}  // namespace bpt
