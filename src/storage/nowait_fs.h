#pragma once

#include <memory>
#include <string>

namespace rocksdb {
class FileSystem;
}

namespace datadam {

// While a thread holds a NoWaitReads, the reads RocksDB makes of its table
// files on that thread come from the OS page cache or fail (IOStatus::Busy)
// instead of waiting for the disk. Only files opened through
// nowait_file_system honor it.
class NoWaitReads {
 public:
  NoWaitReads();
  ~NoWaitReads();
  NoWaitReads(const NoWaitReads&) = delete;
  NoWaitReads& operator=(const NoWaitReads&) = delete;

 private:
  bool previous_;
};

// Whether the file system of directory `dir` can tell a cached page from one
// on disk (Linux: preadv2 with RWF_NOWAIT). ext4 and xfs can; overlayfs (a
// container's own files) and tmpfs cannot, and nor can any file system elsewhere.
bool nowait_reads_work(const std::string& dir);

// A file system whose random-access files honor NoWaitReads (a no-wait read
// that the file system refuses fails too). Elsewhere than on Linux it is
// base itself.
std::shared_ptr<rocksdb::FileSystem> nowait_file_system(std::shared_ptr<rocksdb::FileSystem> base);

}  // namespace datadam
