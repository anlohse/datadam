#include "storage/nowait_fs.h"

#include <rocksdb/file_system.h>

#ifdef __linux__
#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#endif

namespace datadam {

namespace {
thread_local bool tl_nowait = false;
}  // namespace

NoWaitReads::NoWaitReads() : previous_(tl_nowait) { tl_nowait = true; }
NoWaitReads::~NoWaitReads() { tl_nowait = previous_; }

#ifdef __linux__

namespace {

// RocksDB's own file for every read, except those made under NoWaitReads:
// they go through a second descriptor of the same file (fd_, -1 if it could
// not be opened) with RWF_NOWAIT.
class NoWaitFile : public rocksdb::FSRandomAccessFileOwnerWrapper {
 public:
  NoWaitFile(std::unique_ptr<rocksdb::FSRandomAccessFile>&& file, int fd)
      : FSRandomAccessFileOwnerWrapper(std::move(file)), fd_(fd) {}
  ~NoWaitFile() override {
    if (fd_ >= 0) ::close(fd_);
  }

  rocksdb::IOStatus Read(uint64_t offset, size_t n, const rocksdb::IOOptions& options, rocksdb::Slice* result,
                         char* scratch, rocksdb::IODebugContext* dbg) const override {
    if (!tl_nowait) return target()->Read(offset, n, options, result, scratch, dbg);
    return read_nowait(offset, n, result, scratch);
  }

  rocksdb::IOStatus MultiRead(rocksdb::FSReadRequest* reqs, size_t num_reqs, const rocksdb::IOOptions& options,
                              rocksdb::IODebugContext* dbg) override {
    if (!tl_nowait) return target()->MultiRead(reqs, num_reqs, options, dbg);
    for (size_t i = 0; i < num_reqs; ++i) {
      reqs[i].status = read_nowait(reqs[i].offset, reqs[i].len, &reqs[i].result, reqs[i].scratch);
    }
    return rocksdb::IOStatus::OK();
  }

  rocksdb::IOStatus Prefetch(uint64_t offset, size_t n, const rocksdb::IOOptions& options,
                             rocksdb::IODebugContext* dbg) override {
    if (tl_nowait) return rocksdb::IOStatus::NotSupported();  // readahead(2) may wait
    return target()->Prefetch(offset, n, options, dbg);
  }

  rocksdb::IOStatus ReadAsync(rocksdb::FSReadRequest& req, const rocksdb::IOOptions& opts,
                              std::function<void(rocksdb::FSReadRequest&, void*)> cb, void* cb_arg,
                              void** io_handle, rocksdb::IOHandleDeleter* del_fn,
                              rocksdb::IODebugContext* dbg) override {
    if (!tl_nowait) return target()->ReadAsync(req, opts, cb, cb_arg, io_handle, del_fn, dbg);
    req.status = read_nowait(req.offset, req.len, &req.result, req.scratch);
    cb(req, cb_arg);
    return rocksdb::IOStatus::OK();
  }

 private:
  // All n bytes from the page cache, or Busy. Short only at the end of the
  // file: RocksDB takes any other short read for corruption.
  rocksdb::IOStatus read_nowait(uint64_t offset, size_t n, rocksdb::Slice* result, char* scratch) const {
    if (fd_ < 0) return rocksdb::IOStatus::Busy("would wait for the disk");
    size_t got = 0;
    while (got < n) {
      iovec v{scratch + got, n - got};
      ssize_t r = ::preadv2(fd_, &v, 1, static_cast<off_t>(offset + got), RWF_NOWAIT);
      if (r > 0) {
        got += static_cast<size_t>(r);
      } else if (r == 0) {
        break;  // the end of the file
      } else if (errno == EINTR) {
        continue;
      } else if (errno == EAGAIN || errno == EOPNOTSUPP) {
        return rocksdb::IOStatus::Busy("would wait for the disk");
      } else {
        return rocksdb::IOStatus::IOError("preadv2", std::strerror(errno));
      }
    }
    *result = rocksdb::Slice(scratch, got);
    return rocksdb::IOStatus::OK();
  }

  int fd_;
};

class NoWaitFileSystem : public rocksdb::FileSystemWrapper {
 public:
  explicit NoWaitFileSystem(const std::shared_ptr<rocksdb::FileSystem>& base) : FileSystemWrapper(base) {}
  const char* Name() const override { return "DatadamNoWaitFileSystem"; }

  rocksdb::IOStatus NewRandomAccessFile(const std::string& name, const rocksdb::FileOptions& opts,
                                        std::unique_ptr<rocksdb::FSRandomAccessFile>* result,
                                        rocksdb::IODebugContext* dbg) override {
    rocksdb::IOStatus st = target()->NewRandomAccessFile(name, opts, result, dbg);
    if (!st.ok() || opts.use_direct_reads || opts.use_mmap_reads) return st;
    // Without its second descriptor (-1: out of descriptors) no-wait reads of the file fail.
    int fd = ::open(name.c_str(), O_RDONLY | O_CLOEXEC);
    *result = std::make_unique<NoWaitFile>(std::move(*result), fd);
    return st;
  }
};

}  // namespace

bool nowait_reads_work(const std::string& dir) {
  std::filesystem::path probe = std::filesystem::path(dir) / "nowait-check";
  std::ofstream(probe) << "datadam";
  int fd = ::open(probe.c_str(), O_RDONLY | O_CLOEXEC);
  bool works = false;
  if (fd >= 0) {
    char c;
    iovec v{&c, 1};
    works = ::preadv2(fd, &v, 1, 0, RWF_NOWAIT) == 1 || errno == EAGAIN;
    ::close(fd);
  }
  std::error_code ignored;
  std::filesystem::remove(probe, ignored);
  return works;
}

std::shared_ptr<rocksdb::FileSystem> nowait_file_system(std::shared_ptr<rocksdb::FileSystem> base) {
  return std::make_shared<NoWaitFileSystem>(base);
}

#else

bool nowait_reads_work(const std::string&) { return false; }

std::shared_ptr<rocksdb::FileSystem> nowait_file_system(std::shared_ptr<rocksdb::FileSystem> base) { return base; }

#endif

}  // namespace datadam
