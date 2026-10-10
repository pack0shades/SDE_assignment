// Benchmark driver: loads the CSV into one of the three B+ tree variants and measures
// insertion, point lookups, range scans, memory, disk usage, tree shape and disk I/O.
//
//   bench --mode mem|disk|hybrid --csv FILE [--n N] [--queries Q] [--pool-mb M]
//         [--no-direct] [--index FILE] [--keep] [--json FILE]
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "bptree.h"
#include "pager.h"

using namespace bpt;

namespace {

constexpr size_t kRecLen = kKeySize + 1 + kValSize + 1;  // "key,payload\n"

struct Opts {
  std::string mode = "mem";
  std::string csv = "bplus_tree_data_10m.csv";
  std::string index;
  std::string json;
  uint64_t n = UINT64_MAX;
  uint64_t queries = 1000000;
  size_t pool_mb = 128;
  bool direct = true;
  bool keep = false;
};

[[noreturn]] void die(const std::string& msg) {
  fprintf(stderr, "error: %s\n", msg.c_str());
  exit(1);
}

double now() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Resident + swapped-out memory of this process, in bytes.
size_t process_memory() {
  FILE* f = fopen("/proc/self/status", "r");
  if (!f) return 0;
  char line[256];
  size_t kb = 0, v;
  while (fgets(line, sizeof line, f))
    if (sscanf(line, "VmRSS: %zu", &v) == 1 || sscanf(line, "VmSwap: %zu", &v) == 1) kb += v;
  fclose(f);
  return kb * 1024;
}

struct Sample {
  Key key;
  uint64_t val_prefix;  // first 8 payload bytes, to check that lookups return the right record
};

uint64_t prefix_of(const uint8_t* val) {
  uint64_t v;
  memcpy(&v, val, 8);
  return v;
}

struct Phase {
  double secs = 0;
  uint64_t ops = 0;
  IoStats io;  // I/O performed during the phase

  double us_per_op() const { return ops ? secs * 1e6 / ops : 0; }
  double ops_per_s() const { return secs > 0 ? ops / secs : 0; }
};

IoStats diff(const IoStats& a, const IoStats& b) {
  return {a.page_reads - b.page_reads, a.page_writes - b.page_writes, a.cache_hits - b.cache_hits,
          a.cache_misses - b.cache_misses, a.evictions - b.evictions};
}

struct Result {
  uint64_t n = 0;
  bool direct = false;
  Meta meta;
  size_t structure_bytes = 0;  // memory owned by the pager (exact)
  size_t process_bytes = 0;    // growth of the process (RSS + swap) while building
  size_t file_bytes = 0;
  size_t pool_frames = 0;
  double flush_secs = 0;
  Phase insert, hit, miss, range, full;
  uint64_t range_records = 0;
};

// Runs fn, filling in the phase's elapsed time and I/O delta.
template <class Pager, class Fn>
void timed(Pager& pager, Phase& ph, Fn&& fn) {
  IoStats before = pager.stats();
  double t0 = now();
  fn();
  ph.secs += now() - t0;
  IoStats d = diff(pager.stats(), before);
  ph.io.page_reads += d.page_reads;
  ph.io.page_writes += d.page_writes;
  ph.io.cache_hits += d.cache_hits;
  ph.io.cache_misses += d.cache_misses;
  ph.io.evictions += d.evictions;
}

template <class Pager>
Result run(const Opts& o, bool persistent, const std::function<std::unique_ptr<Pager>(bool)>& make) {
  Result r;
  FILE* f = fopen(o.csv.c_str(), "rb");
  if (!f) die("cannot open " + o.csv);
  char header[64];
  if (!fgets(header, sizeof header, f) || strcmp(header, "key,payload\n") != 0)
    die("expected CSV header 'key,payload'");

  // Query keys are a uniform reservoir sample of the inserted records.
  std::vector<Sample> samples(o.queries);
  for (auto& s : samples) s = {0, 0};  // touch now so it is part of the memory baseline
  const size_t kBatch = 1 << 16;
  std::vector<char> buf(kBatch * kRecLen);
  std::vector<Key> keys(kBatch);
  std::mt19937_64 rng(12345);

  size_t mem_before = process_memory();
  auto pager = make(true);
  auto tree = std::make_unique<BPlusTree<Pager>>(*pager);
  r.direct = persistent && pager->direct();

  // ---- build: insert records one at a time, in file order -----------------------------
  double last_report = now();
  while (r.n < o.n) {
    size_t want = std::min<uint64_t>(kBatch, o.n - r.n);
    size_t got = fread(buf.data(), kRecLen, want, f);
    if (got == 0) break;
    for (size_t i = 0; i < got; ++i) {
      const char* rec = &buf[i * kRecLen];
      if (rec[kKeySize] != ',' || rec[kRecLen - 1] != '\n')
        die("malformed record at line " + std::to_string(r.n + i + 2) +
            " (expected an 8-char key and a 64-char payload)");
      keys[i] = make_key(rec);
      uint64_t seen = r.n + i;
      size_t slot = seen < samples.size() ? seen : rng() % (seen + 1);
      if (slot < samples.size())
        samples[slot] = {keys[i], prefix_of(reinterpret_cast<const uint8_t*>(rec) + kKeySize + 1)};
    }
    uint64_t added = 0;
    timed(*pager, r.insert, [&] {
      for (size_t i = 0; i < got; ++i)
        added += tree->insert(keys[i], reinterpret_cast<const uint8_t*>(&buf[i * kRecLen]) + kKeySize + 1);
    });
    if (added != got) die("duplicate key in the input");
    r.n += got;
    if (now() - last_report > 10) {
      fprintf(stderr, "  inserted %llu (%.0f s, %.0f inserts/s overall)\n", (unsigned long long)r.n,
              r.insert.secs, r.n / r.insert.secs);
      last_report = now();
    }
  }
  if (!feof(f) && r.n < o.n) die("trailing partial record in the CSV");
  fclose(f);
  if (r.n == 0) die("no records");
  samples.resize(std::min<uint64_t>(samples.size(), r.n));
  std::shuffle(samples.begin(), samples.end(), rng);

  r.process_bytes = process_memory() - std::min(process_memory(), mem_before);
  r.structure_bytes = pager->memory_bytes();
  // Making the tree durable (write-back of dirty pages + fsync) counts as insert time.
  // The queries that follow run against the tree as the build left it (warm cache).
  double t0 = now();
  timed(*pager, r.insert, [&] { tree->flush(); });
  r.flush_secs = now() - t0;
  r.insert.ops = r.n;
  r.meta = tree->meta();
  r.file_bytes = pager->file_bytes();


  // ---- point lookups of existing keys --------------------------------------------------
  uint8_t val[kValSize];
  uint64_t ok = 0;
  timed(*pager, r.hit, [&] {
    for (const Sample& s : samples) ok += tree->search(s.key, val) && prefix_of(val) == s.val_prefix;
  });
  r.hit.ops = samples.size();
  if (ok != samples.size()) die("lookup returned a wrong or missing record");

  // ---- point lookups of absent keys: an existing key with its last character changed
  //      to one the dataset never uses, so the probe lands inside the populated range ----
  size_t nmiss = samples.size() / 10;
  uint64_t found = 0;
  timed(*pager, r.miss, [&] {
    for (size_t i = 0; i < nmiss; ++i) found += tree->search((samples[i].key & ~Key(0xff)) | '_', val);
  });
  r.miss.ops = nmiss;
  if (found != 0) die("lookup found a key that was never inserted");

  // ---- short range scans: 100 consecutive records from a random start key -------------
  size_t nrange = std::min<size_t>(samples.size(), 10000);
  uint64_t sum = 0;
  timed(*pager, r.range, [&] {
    for (size_t i = 0; i < nrange; ++i)
      r.range_records += tree->scan(samples[i].key, 100, [&](Key, const uint8_t* v) { sum += v[0]; });
  });
  r.range.ops = nrange;

  // ---- full scan along the leaf chain; also verifies global key order and count --------
  uint64_t count = 0;
  Key prev = 0;
  bool ordered = true;
  timed(*pager, r.full, [&] {
    tree->scan(0, UINT64_MAX, [&](Key k, const uint8_t* v) {
      ordered &= count++ == 0 || k > prev;
      prev = k;
      sum += v[0];
    });
  });
  r.full.ops = count;
  if (!ordered || count != r.n) die("full scan is out of order or incomplete");
  if (sum == 0) die("payloads are empty");

  // ---- persistence check (not timed): close, reopen from the file, look records up -----
  if (persistent) {
    tree.reset();
    pager.reset();
    pager = make(false);
    tree = std::make_unique<BPlusTree<Pager>>(*pager);
    if (tree->size() != r.n || tree->height() != r.meta.height) die("tree did not survive a reopen");
    for (size_t i = 0; i < std::min<size_t>(samples.size(), 1000); ++i)
      if (!tree->search(samples[i].key, val) || prefix_of(val) != samples[i].val_prefix)
        die("record lost across a reopen");
  }

  return r;
}

void print_phase(const char* name, const Phase& p, bool io, bool pool) {
  printf("%-22s %10.2f s  %12llu ops  %10.3f us/op  %12.0f ops/s\n", name, p.secs,
         (unsigned long long)p.ops, p.us_per_op(), p.ops_per_s());
  if (io)
    printf("%-22s reads %llu (%.3f/op)  writes %llu (%.3f/op)\n", "", (unsigned long long)p.io.page_reads,
           double(p.io.page_reads) / p.ops, (unsigned long long)p.io.page_writes,
           double(p.io.page_writes) / p.ops);
  if (pool && p.io.cache_hits + p.io.cache_misses)
    printf("%-22s pool hit ratio %.2f%%  evictions %llu\n", "",
           100.0 * p.io.cache_hits / (p.io.cache_hits + p.io.cache_misses),
           (unsigned long long)p.io.evictions);
}

void json_phase(FILE* f, const char* name, const Phase& p) {
  fprintf(f,
          "  \"%s\": {\"secs\": %.4f, \"ops\": %llu, \"us_per_op\": %.4f, \"page_reads\": %llu, "
          "\"page_writes\": %llu, \"cache_hits\": %llu, \"cache_misses\": %llu, \"evictions\": %llu},\n",
          name, p.secs, (unsigned long long)p.ops, p.us_per_op(), (unsigned long long)p.io.page_reads,
          (unsigned long long)p.io.page_writes, (unsigned long long)p.io.cache_hits,
          (unsigned long long)p.io.cache_misses, (unsigned long long)p.io.evictions);
}

void report(const Opts& o, const Result& r) {
  const bool io = o.mode != "mem", pool = o.mode == "hybrid";
  const Meta& m = r.meta;
  double mb = 1024.0 * 1024.0;
  printf("\n=== %s B+ tree ===\n", o.mode == "mem" ? "memory-only" : o.mode == "disk" ? "disk-only" : "memory+disk");
  printf("records                %llu\n", (unsigned long long)r.n);
  printf("page size              %zu bytes\n", kPageSize);
  printf("leaf capacity          %zu records\n", kLeafCap);
  printf("internal order         %zu children (%zu keys)\n", kIntCap + 1, kIntCap);
  if (io) printf("file I/O               %s\n", r.direct ? "O_DIRECT (OS page cache bypassed)" : "buffered (through the OS page cache)");
  if (pool) printf("buffer pool            %zu frames = %.1f MiB\n", r.pool_frames, r.pool_frames * kPageSize / mb);
  printf("tree height            %u levels\n", m.height);
  printf("nodes                  %llu leaf + %llu internal\n", (unsigned long long)m.leaf_pages,
         (unsigned long long)m.internal_pages);
  printf("avg leaf fill          %.1f%%\n", 100.0 * m.num_keys / (m.leaf_pages * kLeafCap));
  if (m.internal_pages)
    printf("avg internal fan-out   %.1f\n", double(m.leaf_pages + m.internal_pages - 1) / m.internal_pages);
  printf("memory (structure)     %.1f MiB\n", r.structure_bytes / mb);
  printf("memory (process grew)  %.1f MiB\n", r.process_bytes / mb);
  printf("disk usage             %.1f MiB\n", r.file_bytes / mb);
  print_phase("insert (incl. flush)", r.insert, io, pool);
  if (io) printf("%-22s of which final flush + fsync %.2f s\n", "", r.flush_secs);
  print_phase("search (hit)", r.hit, io, pool);
  print_phase("search (miss)", r.miss, io, pool);
  print_phase("range scan (100 rec)", r.range, io, pool);
  print_phase("full scan", r.full, io, pool);

  if (o.json.empty()) return;
  FILE* f = fopen(o.json.c_str(), "w");
  if (!f) die("cannot write " + o.json);
  fprintf(f, "{\n  \"mode\": \"%s\", \"records\": %llu, \"page_size\": %zu, \"leaf_capacity\": %zu,\n", o.mode.c_str(),
          (unsigned long long)r.n, kPageSize, kLeafCap);
  fprintf(f, "  \"internal_order\": %zu, \"direct_io\": %s, \"pool_frames\": %zu,\n", kIntCap + 1,
          r.direct ? "true" : "false", r.pool_frames);
  fprintf(f, "  \"height\": %u, \"leaf_pages\": %llu, \"internal_pages\": %llu,\n", m.height,
          (unsigned long long)m.leaf_pages, (unsigned long long)m.internal_pages);
  fprintf(f, "  \"structure_bytes\": %zu, \"process_bytes\": %zu, \"file_bytes\": %zu, \"flush_secs\": %.4f,\n",
          r.structure_bytes, r.process_bytes, r.file_bytes, r.flush_secs);
  json_phase(f, "insert", r.insert);
  json_phase(f, "search_hit", r.hit);
  json_phase(f, "search_miss", r.miss);
  json_phase(f, "range_scan", r.range);
  json_phase(f, "full_scan", r.full);
  fprintf(f, "  \"range_records\": %llu\n}\n", (unsigned long long)r.range_records);
  fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
  Opts o;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) die("missing value for " + a);
      return argv[++i];
    };
    if (a == "--mode") o.mode = next();
    else if (a == "--csv") o.csv = next();
    else if (a == "--index") o.index = next();
    else if (a == "--json") o.json = next();
    else if (a == "--n") o.n = strtoull(next().c_str(), nullptr, 10);
    else if (a == "--queries") o.queries = strtoull(next().c_str(), nullptr, 10);
    else if (a == "--pool-mb") o.pool_mb = strtoull(next().c_str(), nullptr, 10);
    else if (a == "--no-direct") o.direct = false;
    else if (a == "--keep") o.keep = true;
    else die("unknown argument " + a + "\nusage: bench --mode mem|disk|hybrid --csv FILE [--n N] "
             "[--queries Q] [--pool-mb M] [--no-direct] [--index FILE] [--keep] [--json FILE]");
  }
  if (o.index.empty()) o.index = "bptree_" + o.mode + ".idx";

  try {
    Result r;
    if (o.mode == "mem") {
      r = run<MemPager>(o, false, [](bool) { return std::make_unique<MemPager>(); });
    } else if (o.mode == "disk") {
      r = run<DiskPager>(o, true, [&](bool create) { return std::make_unique<DiskPager>(o.index, create, o.direct); });
    } else if (o.mode == "hybrid") {
      size_t frames = 0;
      r = run<BufferPoolPager>(o, true, [&](bool create) {
        auto p = std::make_unique<BufferPoolPager>(o.index, create, o.pool_mb << 20, o.direct);
        frames = p->num_frames();
        return p;
      });
      r.pool_frames = frames;
    } else {
      die("--mode must be mem, disk or hybrid");
    }
    if (o.mode != "mem" && !o.keep) unlink(o.index.c_str());
    report(o, r);
  } catch (const std::exception& e) {
    die(e.what());
  }
  return 0;
}
