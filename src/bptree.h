// B+ tree over fixed-size pages, parameterised on the storage engine (see pager.h).
//
// All records live in the leaves, which are chained left-to-right for range scans.
// Internal nodes hold separator keys only: child i covers keys < keys[i], child i+1
// covers keys >= keys[i]. Supports insert (upsert), point lookup and ordered scans.
#pragma once

#include <algorithm>
#include <string>

#include "common.h"

namespace bpt {

template <class Pager>
class BPlusTree {
 public:
  // Opens the tree stored in `pager`, or creates an empty one.
  explicit BPlusTree(Pager& pager) : pager_(pager) {
    if (!pager_.load_meta(meta_)) {
      Ref root(pager_);
      hdr(root.p)->is_leaf = 1;
      meta_.root = root.pid;
      meta_.height = 1;
      meta_.leaf_pages = 1;
      dirty_ = true;
    }
  }
  BPlusTree(const BPlusTree&) = delete;
  ~BPlusTree() {
    try {
      flush();
    } catch (...) {
    }
  }

  // Inserts the record, or overwrites the payload if the key already exists.
  // Returns true if the key was new.
  bool insert(Key k, const uint8_t* val) {
    Split sp;
    bool added = false;
    insert_rec(meta_.root, meta_.height, k, val, sp, added);
    if (sp.happened) {  // the root split: grow the tree by one level
      Ref root(pager_);
      hdr(root.p)->count = 1;
      node_keys(root.p)[0] = sp.key;
      int_children(root.p)[0] = meta_.root;
      int_children(root.p)[1] = sp.right;
      meta_.root = root.pid;
      ++meta_.height;
      ++meta_.internal_pages;
    }
    meta_.num_keys += added;
    dirty_ = true;
    return added;
  }

  // Copies the payload into `out` (kValSize bytes) and returns true if the key exists.
  bool search(Key k, uint8_t* out) {
    Ref leaf(pager_, find_leaf(k));
    Key* keys = node_keys(leaf.p);
    size_t cnt = hdr(leaf.p)->count;
    size_t pos = std::lower_bound(keys, keys + cnt, k) - keys;
    if (pos == cnt || keys[pos] != k) return false;
    memcpy(out, leaf_vals(leaf.p) + pos * kValSize, kValSize);
    return true;
  }

  // Calls fn(key, payload) for up to `limit` records with key >= start, in key order.
  // Returns the number of records visited.
  template <class Fn>
  uint64_t scan(Key start, uint64_t limit, Fn&& fn) {
    PageId pid = find_leaf(start);
    uint64_t n = 0;
    bool first = true;
    while (pid != kNoPage && n < limit) {
      Ref leaf(pager_, pid);
      Key* keys = node_keys(leaf.p);
      uint8_t* vals = leaf_vals(leaf.p);
      size_t cnt = hdr(leaf.p)->count;
      size_t i = first ? std::lower_bound(keys, keys + cnt, start) - keys : 0;
      for (; i < cnt && n < limit; ++i, ++n) fn(keys[i], vals + i * kValSize);
      pid = hdr(leaf.p)->next;
      first = false;
    }
    return n;
  }

  // Persists the metadata and everything the pager is still holding in memory.
  void flush() {
    if (!dirty_) return;
    pager_.store_meta(meta_);
    pager_.flush();
    dirty_ = false;
  }

  uint64_t size() const { return meta_.num_keys; }
  uint32_t height() const { return meta_.height; }
  const Meta& meta() const { return meta_; }

  // Walks the whole tree checking ordering, separator bounds, node occupancy, uniform
  // leaf depth and the leaf chain. Returns false and fills `err` on the first violation.
  bool validate(std::string& err) {
    Counts c;
    if (!check(meta_.root, meta_.height, false, 0, false, 0, c, err)) return false;
    if (c.keys != meta_.num_keys || c.leaves != meta_.leaf_pages || c.internals != meta_.internal_pages) {
      err = "metadata counters do not match the tree";
      return false;
    }
    uint64_t n = 0;
    Key prev = 0;
    bool ordered = true;
    scan(0, UINT64_MAX, [&](Key k, const uint8_t*) {
      if (n++ > 0 && k <= prev) ordered = false;
      prev = k;
    });
    if (!ordered || n != meta_.num_keys) {
      err = "leaf chain is out of order or incomplete";
      return false;
    }
    return true;
  }

 private:
  // Pins a page for the lifetime of the object.
  struct Ref {
    Pager& pager;
    uint8_t* p;
    PageId pid;
    bool dirty;

    Ref(Pager& pg, PageId id) : pager(pg), p(pg.pin(id)), pid(id), dirty(false) {}
    explicit Ref(Pager& pg) : pager(pg), p(nullptr), pid(pg.alloc(&p)), dirty(true) {}  // new zeroed page
    Ref(const Ref&) = delete;
    ~Ref() { pager.unpin(pid, p, dirty); }
  };

  struct Split {
    bool happened = false;
    Key key = 0;             // separator to add to the parent
    PageId right = kNoPage;  // new right sibling
  };

  struct Counts {
    uint64_t keys = 0, leaves = 0, internals = 0;
  };

  static size_t child_index(uint8_t* p, Key k) {
    Key* keys = node_keys(p);
    return std::upper_bound(keys, keys + hdr(p)->count, k) - keys;
  }

  PageId find_leaf(Key k) {
    PageId pid = meta_.root;
    for (uint32_t level = meta_.height; level > 1; --level) {
      Ref n(pager_, pid);
      pid = int_children(n.p)[child_index(n.p, k)];
    }
    return pid;
  }

  static void leaf_put(uint8_t* p, size_t pos, Key k, const uint8_t* val) {
    Key* keys = node_keys(p);
    uint8_t* vals = leaf_vals(p);
    size_t cnt = hdr(p)->count;
    memmove(keys + pos + 1, keys + pos, (cnt - pos) * kKeySize);
    memmove(vals + (pos + 1) * kValSize, vals + pos * kValSize, (cnt - pos) * kValSize);
    keys[pos] = k;
    memcpy(vals + pos * kValSize, val, kValSize);
    hdr(p)->count = uint16_t(cnt + 1);
  }

  void insert_leaf(Ref& n, Key k, const uint8_t* val, Split& sp, bool& added) {
    NodeHeader* h = hdr(n.p);
    Key* keys = node_keys(n.p);
    uint8_t* vals = leaf_vals(n.p);
    size_t cnt = h->count;
    size_t pos = std::lower_bound(keys, keys + cnt, k) - keys;
    n.dirty = true;
    if (pos < cnt && keys[pos] == k) {
      memcpy(vals + pos * kValSize, val, kValSize);
      return;
    }
    added = true;
    if (cnt < kLeafCap) {
      leaf_put(n.p, pos, k, val);
      return;
    }

    // Full: move the upper half to a new right sibling, then insert into whichever
    // side the key belongs to.
    Ref r(pager_);
    ++meta_.leaf_pages;
    size_t half = cnt / 2, moved = cnt - half;
    memcpy(node_keys(r.p), keys + half, moved * kKeySize);
    memcpy(leaf_vals(r.p), vals + half * kValSize, moved * kValSize);
    hdr(r.p)->is_leaf = 1;
    hdr(r.p)->count = uint16_t(moved);
    hdr(r.p)->next = h->next;
    h->count = uint16_t(half);
    h->next = r.pid;
    if (pos <= half)
      leaf_put(n.p, pos, k, val);
    else
      leaf_put(r.p, pos - half, k, val);
    sp = {true, node_keys(r.p)[0], r.pid};
  }

  void insert_rec(PageId pid, uint32_t level, Key k, const uint8_t* val, Split& sp, bool& added) {
    Ref n(pager_, pid);
    if (level == 1) {
      insert_leaf(n, k, val, sp, added);
      return;
    }

    NodeHeader* h = hdr(n.p);
    Key* keys = node_keys(n.p);
    PageId* ch = int_children(n.p);
    size_t cnt = h->count;
    size_t idx = child_index(n.p, k);
    Split cs;
    insert_rec(ch[idx], level - 1, k, val, cs, added);
    if (!cs.happened) return;

    // The child split: add (cs.key, cs.right) just after child idx.
    n.dirty = true;
    if (cnt < kIntCap) {
      memmove(keys + idx + 1, keys + idx, (cnt - idx) * kKeySize);
      memmove(ch + idx + 2, ch + idx + 1, (cnt - idx) * sizeof(PageId));
      keys[idx] = cs.key;
      ch[idx + 1] = cs.right;
      h->count = uint16_t(cnt + 1);
      return;
    }

    // Full: lay out the overflowing node in scratch arrays, push the middle key up
    // and split the rest between this node and a new right sibling.
    Key tk[kIntCap + 1];
    PageId tc[kIntCap + 2];
    memcpy(tk, keys, idx * kKeySize);
    tk[idx] = cs.key;
    memcpy(tk + idx + 1, keys + idx, (cnt - idx) * kKeySize);
    memcpy(tc, ch, (idx + 1) * sizeof(PageId));
    tc[idx + 1] = cs.right;
    memcpy(tc + idx + 2, ch + idx + 1, (cnt - idx) * sizeof(PageId));

    size_t total = cnt + 1, mid = total / 2, rcnt = total - mid - 1;
    Ref r(pager_);
    ++meta_.internal_pages;
    memcpy(keys, tk, mid * kKeySize);
    memcpy(ch, tc, (mid + 1) * sizeof(PageId));
    h->count = uint16_t(mid);
    memcpy(node_keys(r.p), tk + mid + 1, rcnt * kKeySize);
    memcpy(int_children(r.p), tc + mid + 1, (rcnt + 1) * sizeof(PageId));
    hdr(r.p)->count = uint16_t(rcnt);
    sp = {true, tk[mid], r.pid};
  }

  bool check(PageId pid, uint32_t level, bool has_lo, Key lo, bool has_hi, Key hi, Counts& c,
             std::string& err) {
    Ref n(pager_, pid);
    NodeHeader* h = hdr(n.p);
    Key* keys = node_keys(n.p);
    size_t cnt = h->count;
    bool leaf = level == 1, root = pid == meta_.root;
    auto bad = [&](const char* what) {
      err = std::string(what) + " (page " + std::to_string(pid) + ")";
      return false;
    };

    if (bool(h->is_leaf) != leaf) return bad("leaf found at the wrong depth");
    if (cnt > (leaf ? kLeafCap : kIntCap)) return bad("node over capacity");
    if (!root && cnt < (leaf ? kLeafCap : kIntCap) / 2) return bad("node under half full");
    if (root && !leaf && cnt < 1) return bad("internal root without keys");
    for (size_t i = 0; i < cnt; ++i) {
      if (i > 0 && keys[i] <= keys[i - 1]) return bad("keys not strictly ascending");
      if (has_lo && keys[i] < lo) return bad("key below the parent's separator");
      if (has_hi && keys[i] >= hi) return bad("key above the parent's separator");
    }
    if (leaf) {
      c.keys += cnt;
      ++c.leaves;
      return true;
    }
    ++c.internals;
    PageId* ch = int_children(n.p);
    for (size_t i = 0; i <= cnt; ++i) {
      bool clo = i > 0 || has_lo, chi = i < cnt || has_hi;
      if (!check(ch[i], level - 1, clo, i > 0 ? keys[i - 1] : lo, chi, i < cnt ? keys[i] : hi, c, err))
        return false;
    }
    return true;
  }

  Pager& pager_;
  Meta meta_;
  bool dirty_ = false;
};

}  // namespace bpt
