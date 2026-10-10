// Correctness test: every storage engine is driven through the same randomised workload
// and compared against std::map, then structurally validated. The disk-backed engines
// are additionally closed and reopened to check that the tree persists.
//
// Built at two page sizes (see Makefile): 4096, and 256 where nodes hold only a few
// entries so splits at every level and deep trees are exercised constantly.
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "bptree.h"
#include "pager.h"

using namespace bpt;

using Payload = std::array<uint8_t, kValSize>;
using Model = std::map<Key, Payload>;

static int failures = 0;

#define CHECK(cond, ...)              \
  do {                                \
    if (!(cond)) {                    \
      ++failures;                     \
      printf("    FAIL: " __VA_ARGS__); \
      printf("\n");                   \
      return;                         \
    }                                 \
  } while (0)

static Payload random_payload(std::mt19937_64& rng) {
  Payload v;
  for (size_t i = 0; i < kValSize; i += 8) {
    uint64_t r = rng();
    memcpy(&v[i], &r, 8);
  }
  return v;
}

template <class Pager>
static void verify(BPlusTree<Pager>& tree, const Model& model, std::mt19937_64& rng) {
  std::string err;
  CHECK(tree.validate(err), "validate: %s", err.c_str());
  CHECK(tree.size() == model.size(), "size %llu != %zu", (unsigned long long)tree.size(), model.size());

  // Every key is found with the right payload; neighbours that were never inserted are not.
  Payload got;
  for (const auto& [k, v] : model) {
    CHECK(tree.search(k, got.data()), "key %llx missing", (unsigned long long)k);
    CHECK(got == v, "wrong payload for key %llx", (unsigned long long)k);
    if (!model.count(k + 1)) CHECK(!tree.search(k + 1, got.data()), "phantom key");
  }

  // A full scan returns exactly the model, in order.
  auto it = model.begin();
  bool same = true;
  uint64_t n = tree.scan(0, UINT64_MAX, [&](Key k, const uint8_t* v) {
    if (it == model.end() || it->first != k || memcmp(it->second.data(), v, kValSize) != 0) same = false;
    if (it != model.end()) ++it;
  });
  CHECK(same && n == model.size(), "full scan differs from the model");

  // Bounded scans from arbitrary start keys (present or not).
  for (int i = 0; i < 200; ++i) {
    Key start = rng();
    if (i % 2 && !model.empty()) start = std::next(model.begin(), rng() % model.size())->first;
    uint64_t limit = rng() % 300;
    auto mit = model.lower_bound(start);
    bool ok = true;
    uint64_t seen = tree.scan(start, limit, [&](Key k, const uint8_t*) {
      if (mit == model.end() || mit->first != k) ok = false;
      if (mit != model.end()) ++mit;
    });
    uint64_t expect = std::min<uint64_t>(limit, std::distance(model.lower_bound(start), model.end()));
    CHECK(ok && seen == expect, "range scan differs from the model");
  }
}

enum class Order { Random, Ascending, Descending };

// `make(create)` builds a fresh pager; `persistent` engines are reopened and re-verified.
template <class Pager>
static void run(const char* name, Order order, size_t n, bool persistent,
                const std::function<std::unique_ptr<Pager>(bool)>& make) {
  static const char* order_name[] = {"random", "ascending", "descending"};
  printf("  %-8s %-10s n=%zu\n", name, order_name[int(order)], n);
  std::mt19937_64 rng(42 + int(order));
  Model model;
  uint32_t height = 0;
  {
    auto pager = make(true);
    BPlusTree<Pager> tree(*pager);
    std::string err;
    CHECK(tree.validate(err) && tree.size() == 0, "empty tree invalid");

    for (size_t i = 0; i < n; ++i) {
      Key k = order == Order::Random ? rng() >> 20
              : order == Order::Ascending ? Key(i) * 3 : Key(n - i) * 3;
      Payload v = random_payload(rng);
      bool fresh = !model.count(k);
      CHECK(tree.insert(k, v.data()) == fresh, "insert returned the wrong 'new key' flag");
      model[k] = v;
      if (i % 7 == 0) {  // overwrite an existing key
        auto it = std::next(model.begin(), rng() % std::min<size_t>(model.size(), 50));
        it->second = random_payload(rng);
        CHECK(!tree.insert(it->first, it->second.data()), "overwrite reported as new");
      }
    }
    verify(tree, model, rng);
    if (failures) return;
    height = tree.height();
  }
  if (!persistent) return;

  auto pager = make(false);
  BPlusTree<Pager> tree(*pager);
  CHECK(tree.height() == height, "height changed across reopen");
  verify(tree, model, rng);
  if (failures) return;
  // Keep writing to the reopened tree.
  for (size_t i = 0; i < n / 4; ++i) {
    Key k = rng() >> 20;
    Payload v = random_payload(rng);
    tree.insert(k, v.data());
    model[k] = v;
  }
  verify(tree, model, rng);
}

int main() {
  printf("page size %zu: leaf holds %zu records, internal node has up to %zu children\n", kPageSize,
         kLeafCap, kIntCap + 1);
  const std::string path = "bptree_test_" + std::to_string(getpid()) + ".idx";
  // O_DIRECT needs I/O sized in multiples of the device block size; only use it at 4K.
  const bool direct = kPageSize % 4096 == 0;
  const size_t n = 60000;

  for (Order order : {Order::Random, Order::Ascending, Order::Descending}) {
    run<MemPager>("memory", order, n, false, [](bool) { return std::make_unique<MemPager>(); });
    run<DiskPager>("disk", order, n, true,
                   [&](bool create) { return std::make_unique<DiskPager>(path, create, direct); });
    // A deliberately tiny pool (16 frames) so nearly every access evicts something.
    run<BufferPoolPager>("hybrid", order, n, true, [&](bool create) {
      return std::make_unique<BufferPoolPager>(path, create, 16 * kPageSize, direct);
    });
    run<BufferPoolPager>("hybrid+", order, n, true, [&](bool create) {
      return std::make_unique<BufferPoolPager>(path, create, 4096 * kPageSize, direct);
    });
  }
  unlink(path.c_str());

  printf(failures ? "FAILED (%d)\n" : "all tests passed\n", failures);
  return failures != 0;
}
