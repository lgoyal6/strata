// In-memory Env for the browser (WASM) build. Replaces src/util/env.cc, which
// is excluded from this build (unguarded F_FULLFSYNC and raise(SIGKILL) have
// no meaning in a browser tab).
//
// Crash semantics mirror the POSIX fault gate (docs/DESIGN.md §6): every
// buffered byte passes one choke point; the write that crosses the configured
// kill offset is torn at exactly that byte, and instead of SIGKILL the
// environment flips to "dead" so every later operation fails with an IO error.
// Bytes appended before the tear survive, matching SIGKILL-with-page-cache
// semantics of tools/crash_test. The demo "reboots" by clearing the dead flag
// and reopening the DB against the surviving files, which runs real recovery
// (WAL replay + torn-tail truncation).

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../src/util/env.h"

namespace strata {
namespace {

struct MemFile {
    std::string data;
};

struct WasmFs {
    std::mutex mu;
    std::map<std::string, std::shared_ptr<MemFile>> files;
    std::set<std::string> dirs;

    // Fault gate.
    std::atomic<long long> written{0};
    std::atomic<long long> crash_at{-1}; // -1 = disarmed
    std::atomic<bool> dead{false};
};

WasmFs& fs() {
    static WasmFs f;
    return f;
}

Status dead_error() {
    return Status::io_error("simulated power cut: environment is dead until reopen");
}

// The choke point. Returns how many of n bytes may be appended; flips the
// environment to dead when the write crosses the kill offset.
std::size_t gate_append(std::size_t n) {
    WasmFs& f = fs();
    const long long at = f.crash_at.load(std::memory_order_relaxed);
    if (at < 0) {
        f.written.fetch_add(static_cast<long long>(n), std::memory_order_relaxed);
        return n;
    }
    const long long before =
        f.written.fetch_add(static_cast<long long>(n), std::memory_order_relaxed);
    const long long allowed = at - before;
    if (allowed >= static_cast<long long>(n)) {
        return n;
    }
    f.dead.store(true, std::memory_order_release);
    return allowed > 0 ? static_cast<std::size_t>(allowed) : 0;
}

class WasmSequentialFile final : public SequentialFile {
  public:
    explicit WasmSequentialFile(std::shared_ptr<MemFile> file) : file_(std::move(file)) {}

    Status read(std::size_t n, Slice* result, char* scratch) override {
        if (fs().dead.load(std::memory_order_acquire)) {
            return dead_error();
        }
        std::lock_guard<std::mutex> lock(fs().mu);
        const std::string& d = file_->data;
        if (pos_ >= d.size()) {
            *result = Slice(scratch, 0);
            return Status::okay();
        }
        const std::size_t k = std::min(n, d.size() - pos_);
        std::memcpy(scratch, d.data() + pos_, k);
        *result = Slice(scratch, k);
        pos_ += k;
        return Status::okay();
    }

  private:
    std::shared_ptr<MemFile> file_;
    std::size_t pos_ = 0;
};

class WasmRandomAccessFile final : public RandomAccessFile {
  public:
    explicit WasmRandomAccessFile(std::shared_ptr<MemFile> file) : file_(std::move(file)) {}

    Status read(std::uint64_t offset, std::size_t n, Slice* result,
                char* scratch) const override {
        if (fs().dead.load(std::memory_order_acquire)) {
            return dead_error();
        }
        std::lock_guard<std::mutex> lock(fs().mu);
        const std::string& d = file_->data;
        if (offset >= d.size()) {
            *result = Slice(scratch, 0);
            return Status::okay();
        }
        const std::size_t k = std::min<std::uint64_t>(n, d.size() - offset);
        std::memcpy(scratch, d.data() + offset, k);
        *result = Slice(scratch, k);
        return Status::okay();
    }

  private:
    std::shared_ptr<MemFile> file_;
};

class WasmWritableFile final : public WritableFile {
  public:
    explicit WasmWritableFile(std::shared_ptr<MemFile> file) : file_(std::move(file)) {}

    Status append(const Slice& data) override {
        if (fs().dead.load(std::memory_order_acquire)) {
            return dead_error();
        }
        const std::size_t allowed = gate_append(data.size());
        if (allowed > 0) {
            std::lock_guard<std::mutex> lock(fs().mu);
            file_->data.append(data.data(), allowed);
        }
        if (allowed < data.size()) {
            return Status::io_error("simulated power cut: torn write");
        }
        return Status::okay();
    }

    Status flush() override {
        return fs().dead.load(std::memory_order_acquire) ? dead_error() : Status::okay();
    }

    Status sync(bool /*full_fsync*/) override {
        return fs().dead.load(std::memory_order_acquire) ? dead_error() : Status::okay();
    }

    Status close() override {
        return Status::okay();
    }

  private:
    std::shared_ptr<MemFile> file_;
};

class WasmFileLock final : public FileLock {};

class WasmEnv final : public Env {
  public:
    Status new_sequential_file(const std::string& fname,
                               std::unique_ptr<SequentialFile>* result) override {
        std::shared_ptr<MemFile> f;
        Status s = find(fname, &f);
        if (!s.ok()) {
            return s;
        }
        result->reset(new WasmSequentialFile(std::move(f)));
        return Status::okay();
    }

    Status new_random_access_file(const std::string& fname,
                                  std::unique_ptr<RandomAccessFile>* result) override {
        std::shared_ptr<MemFile> f;
        Status s = find(fname, &f);
        if (!s.ok()) {
            return s;
        }
        result->reset(new WasmRandomAccessFile(std::move(f)));
        return Status::okay();
    }

    Status new_writable_file(const std::string& fname,
                             std::unique_ptr<WritableFile>* result) override {
        if (fs().dead.load(std::memory_order_acquire)) {
            return dead_error();
        }
        std::lock_guard<std::mutex> lock(fs().mu);
        auto f = std::make_shared<MemFile>();
        fs().files[fname] = f;
        result->reset(new WasmWritableFile(std::move(f)));
        return Status::okay();
    }

    bool file_exists(const std::string& fname) override {
        std::lock_guard<std::mutex> lock(fs().mu);
        return fs().files.count(fname) > 0;
    }

    Status get_children(const std::string& dir, std::vector<std::string>* result) override {
        result->clear();
        const std::string prefix = dir.back() == '/' ? dir : dir + "/";
        std::lock_guard<std::mutex> lock(fs().mu);
        for (const auto& [name, _] : fs().files) {
            if (name.rfind(prefix, 0) == 0) {
                const std::string rest = name.substr(prefix.size());
                if (rest.find('/') == std::string::npos) {
                    result->push_back(rest);
                }
            }
        }
        return Status::okay();
    }

    Status remove_file(const std::string& fname) override {
        if (fs().dead.load(std::memory_order_acquire)) {
            return dead_error();
        }
        std::lock_guard<std::mutex> lock(fs().mu);
        return fs().files.erase(fname) > 0
                   ? Status::okay()
                   : Status::io_error(fname + ": not found");
    }

    Status truncate_file(const std::string& fname, std::uint64_t size) override {
        if (fs().dead.load(std::memory_order_acquire)) {
            return dead_error();
        }
        std::shared_ptr<MemFile> f;
        Status s = find(fname, &f);
        if (!s.ok()) {
            return s;
        }
        std::lock_guard<std::mutex> lock(fs().mu);
        if (size < f->data.size()) {
            f->data.resize(size);
        }
        return Status::okay();
    }

    Status rename_file(const std::string& src, const std::string& dst) override {
        if (fs().dead.load(std::memory_order_acquire)) {
            return dead_error();
        }
        std::lock_guard<std::mutex> lock(fs().mu);
        auto it = fs().files.find(src);
        if (it == fs().files.end()) {
            return Status::io_error(src + ": not found");
        }
        fs().files[dst] = it->second;
        fs().files.erase(it);
        return Status::okay();
    }

    Status create_dir_if_missing(const std::string& dirname) override {
        std::lock_guard<std::mutex> lock(fs().mu);
        fs().dirs.insert(dirname);
        return Status::okay();
    }

    Status get_file_size(const std::string& fname, std::uint64_t* size) override {
        std::shared_ptr<MemFile> f;
        Status s = find(fname, &f);
        if (!s.ok()) {
            return s;
        }
        std::lock_guard<std::mutex> lock(fs().mu);
        *size = f->data.size();
        return Status::okay();
    }

    Status sync_dir(const std::string& /*dirname*/) override {
        return fs().dead.load(std::memory_order_acquire) ? dead_error() : Status::okay();
    }

    Status lock_file(const std::string& /*fname*/, FileLock** lock) override {
        *lock = new WasmFileLock();
        return Status::okay();
    }

    Status unlock_file(FileLock* lock) override {
        delete lock;
        return Status::okay();
    }

    std::uint64_t now_micros() override {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    void sleep_micros(std::uint64_t micros) override {
        std::this_thread::sleep_for(std::chrono::microseconds(micros));
    }

  private:
    static Status find(const std::string& fname, std::shared_ptr<MemFile>* out) {
        if (fs().dead.load(std::memory_order_acquire)) {
            return dead_error();
        }
        std::lock_guard<std::mutex> lock(fs().mu);
        auto it = fs().files.find(fname);
        if (it == fs().files.end()) {
            return Status::io_error(fname + ": not found");
        }
        *out = it->second;
        return Status::okay();
    }
};

} // namespace

// env.cc is excluded from the WASM build, so the default Env lives here.
Env* Env::default_env() {
    static WasmEnv env;
    return &env;
}

// --- hooks for the demo bindings (strata_wasm.cc) ---

void wasm_env_arm_crash(long long bytes_from_now) {
    WasmFs& f = fs();
    f.crash_at.store(f.written.load(std::memory_order_relaxed) + bytes_from_now,
                     std::memory_order_relaxed);
}

bool wasm_env_is_dead() {
    return fs().dead.load(std::memory_order_acquire);
}

// Clears the dead flag and disarms the gate; files survive (the "reboot").
void wasm_env_revive() {
    WasmFs& f = fs();
    f.crash_at.store(-1, std::memory_order_relaxed);
    f.dead.store(false, std::memory_order_release);
}

// Wipes everything for a fresh database.
void wasm_env_wipe() {
    WasmFs& f = fs();
    std::lock_guard<std::mutex> lock(f.mu);
    f.files.clear();
    f.dirs.clear();
    f.crash_at.store(-1, std::memory_order_relaxed);
    f.dead.store(false, std::memory_order_release);
    f.written.store(0, std::memory_order_relaxed);
}

long long wasm_env_bytes_written() {
    return fs().written.load(std::memory_order_relaxed);
}

// JSON array of {name, size} for every live file, for the LSM visualizer.
std::string wasm_env_file_listing() {
    WasmFs& f = fs();
    std::lock_guard<std::mutex> lock(f.mu);
    std::string out = "[";
    bool first = true;
    for (const auto& [name, file] : f.files) {
        if (!first) {
            out += ",";
        }
        first = false;
        out += "{\"name\":\"" + name + "\",\"size\":" + std::to_string(file->data.size()) + "}";
    }
    out += "]";
    return out;
}

} // namespace strata
