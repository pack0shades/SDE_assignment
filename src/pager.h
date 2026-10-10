// The three storage engines. The B+ tree is written once against this interface:
//
//   uint8_t* pin(PageId)                 -> page contents, valid until unpin
//   void     unpin(PageId, uint8_t*, bool dirty)
//   PageId   alloc(uint8_t** out)        -> new zeroed page, returned pinned
//   bool     load_meta(Meta&) / void store_meta(const Meta&) / void flush()
//
//   MemPager         memory only   - pages live on the heap, nothing is ever written
//   DiskPager        disk only     - no cache: every pin is a pread, every dirty unpin a pwrite
//   BufferPoolPager  memory + disk - fixed-size LRU buffer pool in front of the file
#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

#include "common.h"

namespace bpt {

inline uint8_t* alloc_pages(size_t n) {
  void* p = aligned_alloc(kPageSize, n * kPageSize);
  if (!p) throw std::bad_alloc();
  return static_cast<uint8_t*>(p);
}

// ---------------------------------------------------------------------------------------
// Memory only
// ---------------------------------------------------------------------------------------
class MemPager {
 public:
  MemPager() = default;
  MemPager(const MemPager&) = delete;
  ~MemPager() {
    for (uint8_t* c : chunks_) free(c);
  }

  uint8_t* pin(PageId pid) {
    return chunks_[pid >> kChunkShift] + (size_t(pid) & (kChunkPages - 1)) * kPageSize;
  }
  void unpin(PageId, uint8_t*, bool) {}

  PageId alloc(uint8_t** out) {
    PageId pid = next_++;
    if ((pid >> kChunkShift) >= chunks_.size()) chunks_.push_back(alloc_pages(kChunkPages));
    *out = pin(pid);
    memset(*out, 0, kPageSize);
    return pid;
  }

  bool load_meta(Meta&) { return false; }
  void store_meta(const Meta&) {}
  void flush() {}

  const IoStats& stats() const { return stats_; }
  bool direct() const { return false; }
  size_t num_pages() const { return next_; }
  size_t memory_bytes() const {
    return chunks_.size() * kChunkPages * kPageSize + chunks_.capacity() * sizeof(uint8_t*);
  }
  size_t file_bytes() const { return 0; }

 private:
  static constexpr unsigned kChunkShift = 8;  // pages are carved out of 256-page chunks
  static constexpr size_t kChunkPages = size_t(1) << kChunkShift;

  std::vector<uint8_t*> chunks_;
  PageId next_ = 1;  // keep id 0 free so kNoPage means the same thing in every pager
  IoStats stats_;
};

// ---------------------------------------------------------------------------------------
// Page file shared by the two disk-backed pagers. Page 0 holds the metadata.
// ---------------------------------------------------------------------------------------
class PageFile {
 public:
  // `direct` opens with O_DIRECT so the OS page cache is bypassed and every counted
  // read/write is a real device I/O. Falls back to buffered I/O where unsupported.
  PageFile(const std::string& path, bool create, bool direct) : buf_(alloc_pages(1)) {
    int flags = O_RDWR | (create ? O_CREAT | O_TRUNC : 0);
    if (direct) {
      fd_ = ::open(path.c_str(), flags | O_DIRECT, 0644);
      direct_ = fd_ >= 0;
    }
    if (fd_ < 0) fd_ = ::open(path.c_str(), flags, 0644);
    if (fd_ < 0) fail("open " + path);

    struct stat st;
    if (fstat(fd_, &st) != 0) fail("fstat");
    if (st.st_size > 0) {
      read(0, buf_);
      --stats_.page_reads;  // opening the file is not part of any measured workload
      DiskMeta dm;
      memcpy(&dm, buf_, sizeof(dm));
      if (memcmp(dm.magic, kMagic, sizeof(dm.magic)) != 0 || dm.page_size != kPageSize)
        throw std::runtime_error(path + ": not a B+ tree file with this page size");
      num_pages_ = dm.num_pages;
      meta_ = dm.meta;
      has_meta_ = true;
      reserved_ = st.st_size / kPageSize;
    }
  }
  PageFile(const PageFile&) = delete;
  ~PageFile() {
    // Drop the unused tail of the last preallocated extent.
    if (ftruncate(fd_, off_t(num_pages_) * kPageSize) != 0) { /* best effort */ }
    ::close(fd_);
    free(buf_);
  }

  void read(PageId pid, uint8_t* buf) {
    if (pread(fd_, buf, kPageSize, off_t(pid) * kPageSize) != ssize_t(kPageSize)) fail("pread");
    ++stats_.page_reads;
  }

  void write(PageId pid, const uint8_t* buf) {
    if (pid >= reserved_) reserve(pid);
    if (pwrite(fd_, buf, kPageSize, off_t(pid) * kPageSize) != ssize_t(kPageSize)) fail("pwrite");
    ++stats_.page_writes;
  }

  PageId alloc_id() { return num_pages_++; }

  bool load_meta(Meta& m) const {
    if (has_meta_) m = meta_;
    return has_meta_;
  }

  void store_meta(const Meta& m) {
    DiskMeta dm{};
    memcpy(dm.magic, kMagic, sizeof(dm.magic));
    dm.page_size = kPageSize;
    dm.num_pages = num_pages_;
    dm.meta = m;
    memset(buf_, 0, kPageSize);
    memcpy(buf_, &dm, sizeof(dm));
    write(0, buf_);
  }

  void sync() {
    if (fsync(fd_) != 0) fail("fsync");
  }

  IoStats& stats() { return stats_; }
  bool direct() const { return direct_; }
  size_t num_pages() const { return num_pages_; }
  size_t file_bytes() const { return size_t(num_pages_) * kPageSize; }

 private:
  static constexpr char kMagic[8] = {'B', 'P', 'T', 'R', 'E', 'E', '0', '1'};
  struct DiskMeta {
    char magic[8];
    uint32_t page_size;
    PageId num_pages;
    Meta meta;
  };
  static_assert(sizeof(DiskMeta) <= kPageSize);

  // Grow the file a chunk at a time rather than one page per write.
  void reserve(PageId pid) {
    size_t chunk = std::max<size_t>(1, (32u << 20) / kPageSize);
    size_t want = (size_t(pid) / chunk + 1) * chunk;
    if (posix_fallocate(fd_, off_t(reserved_) * kPageSize, off_t(want - reserved_) * kPageSize) != 0)
      want = SIZE_MAX;  // filesystem can't preallocate; plain extending writes still work
    reserved_ = want;
  }

  [[noreturn]] static void fail(const std::string& what) {
    throw std::runtime_error(what + ": " + strerror(errno));
  }

  int fd_ = -1;
  bool direct_ = false;
  uint8_t* buf_;
  PageId num_pages_ = 1;  // next unallocated page id
  size_t reserved_ = 0;   // pages of file space already allocated
  Meta meta_;
  bool has_meta_ = false;
  IoStats stats_;
};

// ---------------------------------------------------------------------------------------
// Disk only: nothing is retained between operations. The handful of scratch frames only
// hold the pages an operation is touching right now (at most one root-to-leaf path).
// ---------------------------------------------------------------------------------------
class DiskPager {
 public:
  DiskPager(const std::string& path, bool create, bool direct = true)
      : file_(path, create, direct), frames_(alloc_pages(kFrames)) {
    for (size_t i = 0; i < kFrames; ++i) free_.push_back(frames_ + i * kPageSize);
  }
  DiskPager(const DiskPager&) = delete;
  ~DiskPager() { free(frames_); }

  uint8_t* pin(PageId pid) {
    uint8_t* f = take();
    file_.read(pid, f);
    return f;
  }

  void unpin(PageId pid, uint8_t* p, bool dirty) {
    if (dirty) file_.write(pid, p);
    free_.push_back(p);
  }

  PageId alloc(uint8_t** out) {
    *out = take();
    memset(*out, 0, kPageSize);
    return file_.alloc_id();
  }

  bool load_meta(Meta& m) { return file_.load_meta(m); }
  void store_meta(const Meta& m) { file_.store_meta(m); }
  void flush() { file_.sync(); }

  const IoStats& stats() { return file_.stats(); }
  bool direct() const { return file_.direct(); }
  size_t num_pages() const { return file_.num_pages(); }
  size_t memory_bytes() const { return kFrames * kPageSize; }
  size_t file_bytes() const { return file_.file_bytes(); }

 private:
  static constexpr size_t kFrames = 32;

  uint8_t* take() {
    if (free_.empty()) throw std::runtime_error("DiskPager: out of scratch frames");
    uint8_t* f = free_.back();
    free_.pop_back();
    return f;
  }

  PageFile file_;
  uint8_t* frames_;
  std::vector<uint8_t*> free_;
};

// ---------------------------------------------------------------------------------------
// Memory + disk: a fixed number of in-memory frames caching file pages. Exact LRU
// replacement over the unpinned frames; dirty pages are written back when evicted
// or on flush().
// ---------------------------------------------------------------------------------------
class BufferPoolPager {
 public:
  BufferPoolPager(const std::string& path, bool create, size_t pool_bytes, bool direct = true)
      : file_(path, create, direct),
        nframes_(std::max<size_t>(pool_bytes / kPageSize, kMinFrames)),
        mem_(alloc_pages(nframes_)),
        frames_(nframes_ + 1) {
    lru_ = int32_t(nframes_);  // sentinel of the circular LRU list
    frames_[lru_].prev = frames_[lru_].next = lru_;
    for (size_t i = nframes_; i-- > 0;) free_.push_back(int32_t(i));
  }
  BufferPoolPager(const BufferPoolPager&) = delete;
  ~BufferPoolPager() { free(mem_); }

  uint8_t* pin(PageId pid) {
    IoStats& st = file_.stats();
    int32_t f = pid < table_.size() ? table_[pid] : -1;
    if (f >= 0) {
      ++st.cache_hits;
      if (frames_[f].pins++ == 0) lru_remove(f);
      return data(f);
    }
    ++st.cache_misses;
    f = grab_frame();
    file_.read(pid, data(f));
    install(f, pid, false);
    return data(f);
  }

  void unpin(PageId, uint8_t* p, bool dirty) {
    int32_t f = int32_t((p - mem_) / kPageSize);
    Frame& fr = frames_[f];
    fr.dirty |= dirty;
    if (--fr.pins == 0) lru_push_front(f);
  }

  PageId alloc(uint8_t** out) {
    PageId pid = file_.alloc_id();
    int32_t f = grab_frame();
    memset(data(f), 0, kPageSize);
    install(f, pid, true);
    *out = data(f);
    return pid;
  }

  bool load_meta(Meta& m) { return file_.load_meta(m); }
  void store_meta(const Meta& m) { file_.store_meta(m); }

  // Write every dirty page (in file order) and fsync.
  void flush() {
    std::vector<int32_t> dirty;
    for (size_t f = 0; f < nframes_; ++f)
      if (frames_[f].pid != kNoPage && frames_[f].dirty) dirty.push_back(int32_t(f));
    std::sort(dirty.begin(), dirty.end(),
              [&](int32_t a, int32_t b) { return frames_[a].pid < frames_[b].pid; });
    for (int32_t f : dirty) {
      file_.write(frames_[f].pid, data(f));
      frames_[f].dirty = false;
    }
    file_.sync();
  }

  const IoStats& stats() { return file_.stats(); }
  bool direct() const { return file_.direct(); }
  size_t num_pages() const { return file_.num_pages(); }
  size_t num_frames() const { return nframes_; }
  size_t memory_bytes() const {
    return nframes_ * kPageSize + frames_.capacity() * sizeof(Frame) +
           table_.capacity() * sizeof(int32_t);
  }
  size_t file_bytes() const { return file_.file_bytes(); }

 private:
  static constexpr size_t kMinFrames = 16;  // must exceed the deepest root-to-leaf path

  struct Frame {
    PageId pid = kNoPage;
    uint32_t pins = 0;
    bool dirty = false;
    int32_t prev = -1, next = -1;  // LRU links, meaningful while unpinned
  };

  uint8_t* data(int32_t f) { return mem_ + size_t(f) * kPageSize; }

  void install(int32_t f, PageId pid, bool dirty) {
    if (pid >= table_.size()) table_.resize(std::max<size_t>(size_t(pid) + 1, table_.size() * 2), -1);
    table_[pid] = f;
    frames_[f].pid = pid;
    frames_[f].pins = 1;
    frames_[f].dirty = dirty;
  }

  int32_t grab_frame() {
    if (!free_.empty()) {
      int32_t f = free_.back();
      free_.pop_back();
      return f;
    }
    int32_t f = frames_[lru_].prev;  // least recently used
    if (f == lru_) throw std::runtime_error("BufferPoolPager: every frame is pinned");
    lru_remove(f);
    Frame& fr = frames_[f];
    if (fr.dirty) file_.write(fr.pid, data(f));
    table_[fr.pid] = -1;
    ++file_.stats().evictions;
    return f;
  }

  void lru_remove(int32_t f) {
    frames_[frames_[f].prev].next = frames_[f].next;
    frames_[frames_[f].next].prev = frames_[f].prev;
  }

  void lru_push_front(int32_t f) {
    frames_[f].next = frames_[lru_].next;
    frames_[f].prev = lru_;
    frames_[frames_[lru_].next].prev = f;
    frames_[lru_].next = f;
  }

  PageFile file_;
  size_t nframes_;
  uint8_t* mem_;
  std::vector<Frame> frames_;   // [nframes_] is the LRU sentinel
  std::vector<int32_t> table_;  // page id -> frame, -1 if not cached (page ids are dense)
  std::vector<int32_t> free_;
  int32_t lru_;
};

}  // namespace bpt
