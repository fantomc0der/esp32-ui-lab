// fs_stub.cpp — the in-memory filesystem behind FS.h.

#include "FS.h"

#include <algorithm>
#include <cstring>

namespace fs {

// ---------------------------------------------------------------- File

File::File(FS *owner, std::string path, bool is_dir, std::vector<uint8_t> *data,
           bool append)
    : owner_(owner), path_(std::move(path)), is_dir_(is_dir), valid_(true), data_(data) {
  // name() is the last path segment, matching the real File.
  const size_t slash = path_.find_last_of('/');
  name_ = (slash == std::string::npos) ? path_ : path_.substr(slash + 1);
  if (append && data_) pos_ = data_->size();
}

size_t File::read(uint8_t *buf, size_t len) {
  if (!valid_ || !data_) return 0;
  const size_t avail = data_->size() - std::min(pos_, data_->size());
  const size_t n = std::min(len, avail);
  if (n) memcpy(buf, data_->data() + pos_, n);
  pos_ += n;
  return n;
}

size_t File::write(const uint8_t *buf, size_t len) {
  if (!valid_ || !data_) return 0;
  if (pos_ + len > data_->size()) data_->resize(pos_ + len);
  memcpy(data_->data() + pos_, buf, len);
  pos_ += len;
  return len;
}

size_t File::size() const {
  if (!data_) return 0;
  return data_->size();
}

File File::openNextFile() {
  if (!valid_ || !is_dir_ || !owner_) return File();
  const std::vector<std::string> kids = owner_->children_of(path_);
  if (child_index_ >= kids.size()) return File();
  const std::string &child = kids[child_index_++];
  const bool dir = owner_->is_directory(child);
  std::vector<uint8_t> *data = nullptr;
  if (!dir) {
    auto it = owner_->files_.find(child);
    if (it != owner_->files_.end()) data = &it->second;
  }
  return File(owner_, child, dir, data, false);
}

// ---------------------------------------------------------------- FS

// Normalizes to a leading slash and no trailing slash, so "/a/" and "a" agree.
static std::string normalize(const char *raw) {
  if (!raw) return "/";
  std::string p(raw);
  if (p.empty()) return "/";
  if (p[0] != '/') p.insert(p.begin(), '/');
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  return p;
}

bool FS::is_directory(const std::string &path) const {
  if (path == "/") return true;
  if (std::find(dirs_.begin(), dirs_.end(), path) != dirs_.end()) return true;
  // Implied by any file living beneath it.
  const std::string prefix = path + "/";
  for (const auto &kv : files_) {
    if (kv.first.compare(0, prefix.size(), prefix) == 0) return true;
  }
  return false;
}

std::vector<std::string> FS::children_of(const std::string &path) const {
  const std::string prefix = (path == "/") ? "/" : path + "/";
  std::vector<std::string> out;
  for (const auto &kv : files_) {
    const std::string &f = kv.first;
    if (f.size() <= prefix.size()) continue;
    if (f.compare(0, prefix.size(), prefix) != 0) continue;
    // Direct children only: take the first segment after the prefix.
    const size_t slash = f.find('/', prefix.size());
    const std::string child =
        (slash == std::string::npos) ? f : f.substr(0, slash);
    if (std::find(out.begin(), out.end(), child) == out.end()) out.push_back(child);
  }
  for (const auto &d : dirs_) {
    if (d.size() <= prefix.size()) continue;
    if (d.compare(0, prefix.size(), prefix) != 0) continue;
    if (d.find('/', prefix.size()) != std::string::npos) continue;
    if (std::find(out.begin(), out.end(), d) == out.end()) out.push_back(d);
  }
  std::sort(out.begin(), out.end());
  return out;
}

File FS::open(const char *path, const char *mode) {
  const std::string p = normalize(path);
  const bool writing = mode && (mode[0] == 'w' || mode[0] == 'a');
  const bool append = mode && mode[0] == 'a';

  if (!writing) {
    if (is_directory(p)) return File(this, p, true, nullptr, false);
    auto it = files_.find(p);
    if (it == files_.end()) return File();
    return File(this, p, false, &it->second, false);
  }

  // Writing to a directory path fails, as it does on a real filesystem.
  if (is_directory(p)) return File();
  if (mode[0] == 'w') files_[p].clear();
  return File(this, p, false, &files_[p], append);
}

bool FS::exists(const char *path) {
  const std::string p = normalize(path);
  return files_.count(p) != 0 || is_directory(p);
}

bool FS::remove(const char *path) {
  return files_.erase(normalize(path)) != 0;
}

bool FS::mkdir(const char *path) {
  const std::string p = normalize(path);
  if (std::find(dirs_.begin(), dirs_.end(), p) == dirs_.end()) dirs_.push_back(p);
  return true;
}

void FS::host_clear() {
  files_.clear();
  dirs_.clear();
}

}  // namespace fs
