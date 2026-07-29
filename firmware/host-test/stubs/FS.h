// FS.h — the host stand-in for the Arduino filesystem interface.
//
// js_bindings.h includes <FS.h> unconditionally (jsvm_set_filesystem takes
// fs::FS* whether or not the fs bindings are compiled in), so this has to exist
// even for a JSVM_WITH_FS=0 build.
//
// Rather than an empty shell, it is a working in-memory filesystem: paths map
// to byte vectors, directories are implied by the paths present. That keeps the
// stub honest — the layer's own read/write/list/remove logic runs for real
// against it — and it is what lets host tests cover path resolution (the
// "flash:" prefix, the card-then-flash fallback) without a card.
//
// What it deliberately does not model: partial writes, ENOSPC, corruption,
// re-mount, or the SD card being pulled mid-read. Those are hardware failure
// modes and stay the board's business.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <string>
#include <vector>

// The real headers expose these as macros over an open mode.
#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"

namespace fs {

class FS;

// One open handle. Copyable and truthiness-testable, matching how the binding
// layer uses it (`File f = fs->open(...); if (!f) ...`).
class File {
 public:
  File() = default;
  File(FS *owner, std::string path, bool is_dir, std::vector<uint8_t> *data,
       bool append);

  operator bool() const { return valid_; }

  size_t read(uint8_t *buf, size_t len);
  size_t write(const uint8_t *buf, size_t len);
  size_t size() const;
  bool isDirectory() const { return is_dir_; }
  const char *name() const { return name_.c_str(); }
  const char *path() const { return path_.c_str(); }
  void close() { valid_ = false; }

  // Directory iteration, as used by fs.list().
  File openNextFile();

 private:
  FS *owner_ = nullptr;
  std::string path_;
  std::string name_;
  bool is_dir_ = false;
  bool valid_ = false;
  std::vector<uint8_t> *data_ = nullptr;
  size_t pos_ = 0;
  size_t child_index_ = 0;
};

class FS {
 public:
  File open(const char *path, const char *mode = FILE_READ);
  bool exists(const char *path);
  bool remove(const char *path);
  bool mkdir(const char *path);

  // Host-side setup, so a test can lay out a card before booting a script.
  void host_write(const std::string &path, const std::string &contents);
  void host_clear();

 private:
  friend class File;

  // Path -> contents. A directory is any prefix of a stored path, plus any
  // path explicitly mkdir'd, tracked as an entry with dir_=true.
  std::map<std::string, std::vector<uint8_t>> files_;
  std::vector<std::string> dirs_;

  bool is_directory(const std::string &path) const;
  std::vector<std::string> children_of(const std::string &path) const;
};

}  // namespace fs

// Sketches say `File` and `fs::FS` interchangeably; the real header does this too.
using fs::File;
