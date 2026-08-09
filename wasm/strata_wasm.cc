// C ABI for the browser demo. Everything returns either an int status
// (0 = ok) or a pointer to a static string valid until the next call.

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <emscripten/emscripten.h>

#include "strata/db.h"
#include "strata/options.h"
#include "strata/write_batch.h"

namespace strata {
// Hooks exported by wasm_env.cc.
void wasm_env_arm_crash(long long bytes_from_now);
bool wasm_env_is_dead();
void wasm_env_revive();
void wasm_env_wipe();
long long wasm_env_bytes_written();
std::string wasm_env_file_listing();
} // namespace strata

namespace {

strata::DB* g_db = nullptr;
std::string g_last_error;
std::string g_ret; // storage for string returns

const char* ret(std::string s) {
    g_ret = std::move(s);
    return g_ret.c_str();
}

int fail(const strata::Status& s) {
    g_last_error = s.to_string();
    return 1;
}

// Deterministic key/value for index i so recovery can be verified without
// shipping the written data back and forth across the JS boundary.
std::string key_for(std::uint32_t i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "k%09u", i);
    return std::string(buf);
}

std::string value_for(std::uint32_t i, std::size_t value_size) {
    std::string v = "v" + std::to_string(i * 2654435761u) + ":";
    while (v.size() < value_size) {
        v += static_cast<char>('a' + (i + v.size()) % 26);
    }
    v.resize(value_size);
    return v;
}

} // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE
const char* sw_last_error() {
    return g_last_error.c_str();
}

// fsync_policy: 0 = kAlways, 1 = kInterval, 2 = kNever.
EMSCRIPTEN_KEEPALIVE
int sw_open(int write_buffer_kb, int target_file_kb, int l0_trigger, int fsync_policy) {
    if (g_db != nullptr) {
        g_last_error = "already open";
        return 1;
    }
    strata::Options opt;
    opt.create_if_missing = true;
    opt.write_buffer_size = static_cast<std::size_t>(write_buffer_kb) * 1024;
    opt.target_file_size = static_cast<std::uint64_t>(target_file_kb) * 1024;
    opt.l0_compaction_trigger = l0_trigger;
    opt.l1_target_bytes = static_cast<std::uint64_t>(target_file_kb) * 1024 * 4;
    opt.block_cache_bytes = 4u << 20;
    opt.fsync_policy = static_cast<strata::FsyncPolicy>(fsync_policy);
    strata::Status s = strata::DB::open(opt, "/db", &g_db);
    return s.ok() ? 0 : fail(s);
}

EMSCRIPTEN_KEEPALIVE
int sw_close() {
    delete g_db; // joins background threads
    g_db = nullptr;
    return 0;
}

EMSCRIPTEN_KEEPALIVE
int sw_put(const char* key, const char* value) {
    strata::Status s =
        g_db->put(strata::WriteOptions(), strata::Slice(key), strata::Slice(value));
    return s.ok() ? 0 : fail(s);
}

EMSCRIPTEN_KEEPALIVE
int sw_delete(const char* key) {
    strata::Status s = g_db->remove(strata::WriteOptions(), strata::Slice(key));
    return s.ok() ? 0 : fail(s);
}

// Returns the value, "" for not-found, or nullptr on error.
EMSCRIPTEN_KEEPALIVE
const char* sw_get(const char* key) {
    std::string value;
    strata::Status s = g_db->get(strata::ReadOptions(), strata::Slice(key), &value);
    if (s.ok()) {
        return ret(std::move(value));
    }
    if (s.is_not_found()) {
        return ret("");
    }
    g_last_error = s.to_string();
    return nullptr;
}

// Writes [start, start+count) deterministic records. Returns micros elapsed,
// or -1 on error (check sw_last_error; a torn write lands here).
EMSCRIPTEN_KEEPALIVE
double sw_fill(std::uint32_t start, std::uint32_t count, int value_size) {
    const double t0 = emscripten_get_now();
    for (std::uint32_t i = start; i < start + count; i++) {
        strata::Status s = g_db->put(strata::WriteOptions(),
                                     strata::Slice(key_for(i)),
                                     strata::Slice(value_for(i, value_size)));
        if (!s.ok()) {
            g_last_error = s.to_string() + " (at index " + std::to_string(i) + ")";
            return -1;
        }
    }
    return (emscripten_get_now() - t0) * 1000.0;
}

// Concurrent writers: `threads` C++ threads each writing `per_thread` records
// (disjoint ranges above `start`). This is what group commit batches.
// Returns micros elapsed or -1 on error.
EMSCRIPTEN_KEEPALIVE
double sw_fill_concurrent(std::uint32_t start, int threads, std::uint32_t per_thread,
                          int value_size) {
    std::vector<std::thread> workers;
    std::atomic<bool> failed{false};
    const double t0 = emscripten_get_now();
    for (int t = 0; t < threads; t++) {
        const std::uint32_t base = start + static_cast<std::uint32_t>(t) * per_thread;
        workers.emplace_back([base, per_thread, value_size, &failed] {
            for (std::uint32_t i = base; i < base + per_thread; i++) {
                strata::Status s = g_db->put(strata::WriteOptions(),
                                             strata::Slice(key_for(i)),
                                             strata::Slice(value_for(i, value_size)));
                if (!s.ok()) {
                    failed.store(true);
                    return;
                }
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }
    if (failed.load()) {
        g_last_error = "write failed during concurrent fill (crash armed?)";
        return -1;
    }
    return (emscripten_get_now() - t0) * 1000.0;
}

// Verifies records [0, n): every key present with the exact expected value.
// Returns -1 if all present, else the first missing/corrupt index.
EMSCRIPTEN_KEEPALIVE
long long sw_verify(std::uint32_t n, int value_size) {
    std::string value;
    for (std::uint32_t i = 0; i < n; i++) {
        strata::Status s = g_db->get(strata::ReadOptions(), strata::Slice(key_for(i)), &value);
        if (!s.ok() || value != value_for(i, value_size)) {
            return i;
        }
    }
    return -1;
}

EMSCRIPTEN_KEEPALIVE
int sw_flush() {
    strata::Status s = g_db->flush();
    return s.ok() ? 0 : fail(s);
}

EMSCRIPTEN_KEEPALIVE
int sw_compact_all() {
    strata::Status s = g_db->compact_all();
    return s.ok() ? 0 : fail(s);
}

// {stats: DbStats fields, files: [{name,size}], bytes_written, dead}
EMSCRIPTEN_KEEPALIVE
const char* sw_stats() {
    std::string out = "{";
    if (g_db != nullptr) {
        const strata::DbStats st = g_db->stats();
        char buf[640];
        std::snprintf(
            buf, sizeof(buf),
            "\"user_bytes_written\":%" PRIu64 ",\"wal_bytes_written\":%" PRIu64
            ",\"flush_bytes_written\":%" PRIu64 ",\"compaction_bytes_read\":%" PRIu64
            ",\"compaction_bytes_written\":%" PRIu64 ",\"flush_count\":%" PRIu64
            ",\"compaction_count\":%" PRIu64 ",\"write_stall_micros\":%" PRIu64
            ",\"bloom_checks\":%" PRIu64 ",\"bloom_skips\":%" PRIu64
            ",\"block_cache_hits\":%" PRIu64 ",\"block_cache_misses\":%" PRIu64
            ",\"write_amplification\":%.3f,",
            st.user_bytes_written, st.wal_bytes_written, st.flush_bytes_written,
            st.compaction_bytes_read, st.compaction_bytes_written, st.flush_count,
            st.compaction_count, st.write_stall_micros, st.bloom_checks, st.bloom_skips,
            st.block_cache_hits, st.block_cache_misses, st.write_amplification());
        out += buf;
    }
    out += "\"bytes_written\":" + std::to_string(strata::wasm_env_bytes_written());
    out += ",\"dead\":" + std::string(strata::wasm_env_is_dead() ? "true" : "false");
    out += ",\"open\":" + std::string(g_db != nullptr ? "true" : "false");
    out += ",\"files\":" + strata::wasm_env_file_listing();
    out += "}";
    return ret(std::move(out));
}

// --- crash machinery ---

EMSCRIPTEN_KEEPALIVE
void sw_crash_in(double bytes_from_now) {
    strata::wasm_env_arm_crash(static_cast<long long>(bytes_from_now));
}

EMSCRIPTEN_KEEPALIVE
int sw_is_dead() {
    return strata::wasm_env_is_dead() ? 1 : 0;
}

// After a crash: drop the dead DB handle (threads see errors and stop at
// join), revive the filesystem, and reopen. Real recovery runs here: WAL
// replay and torn-tail truncation against whatever bytes survived the tear.
EMSCRIPTEN_KEEPALIVE
int sw_recover(int write_buffer_kb, int target_file_kb, int l0_trigger, int fsync_policy) {
    delete g_db;
    g_db = nullptr;
    strata::wasm_env_revive();
    return sw_open(write_buffer_kb, target_file_kb, l0_trigger, fsync_policy);
}

EMSCRIPTEN_KEEPALIVE
int sw_wipe() {
    delete g_db;
    g_db = nullptr;
    strata::wasm_env_wipe();
    return 0;
}

} // extern "C"
