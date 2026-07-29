// Crash harness (docs/DESIGN.md §6).
//
//   crash_test orchestrate --dir BASE --iters N --workers W
//                          --fsync always|interval|never
//                          --mode fresh|chain --kill bytes|timer [--seed S]
//
// Each iteration forks a child workload process. The child performs a
// deterministic op stream derived from (seed, op index) and acknowledges
// each op by appending a line to an ack FILE (O_APPEND + raw write(2)) AFTER
// DB::write returns — a file, not a pipe, because appended bytes survive
// SIGKILL in the page cache and there is no fd-inheritance to race between
// concurrently forking workers. The parent kills the child with a real
// SIGKILL — either at a random byte offset inside a write(2) (via the Env
// fault-injection choke point, STRATA_CRASH_AT_BYTES) or at a random
// wall-clock time — waits for it, reads the acks, reopens the database
// in-process and asserts:
//   A  every acknowledged op survives,
//   B  every recovered value verifies its embedded checksum,
//   C  the recovered state equals the model at EXACTLY the acked prefix or
//      the acked prefix + the single in-flight op (single-writer child), so
//      recovery is a prefix — nothing torn, nothing resurrected, nothing
//      reordered.
// chain mode keeps the same directory across kills (crash -> recover ->
// continue), landing kills inside flush/compaction of real recovered state.

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "strata/db.h"
#include "util/crc32c.h"
#include "util/env.h"
#include "util/random.h"

namespace {

using namespace strata;

constexpr std::uint32_t kKeySpace = 256;

// ---------------------------------------------------------------------------
// Deterministic op stream: everything derives from (stream_seed, op_index),
// so the parent can reconstruct the child's intent without a side channel.
// ---------------------------------------------------------------------------

struct Op {
    bool is_put;
    std::string key;
    std::string value; // empty for deletes
};

std::uint64_t mix(std::uint64_t seed, std::uint64_t i) {
    std::uint64_t x = seed ^ (i * 0x9e3779b97f4a7c15ull);
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

Op derive_op(std::uint64_t seed, std::uint64_t i) {
    const std::uint64_t h = mix(seed, i);
    Op op;
    op.is_put = (h % 100) < 80;
    char key[24];
    std::snprintf(key, sizeof(key), "k%05llu",
                  static_cast<unsigned long long>((h >> 8) % kKeySpace));
    op.key = key;
    if (op.is_put) {
        // The value embeds (seed, i, crc) so any accepted-but-torn or
        // cross-run value is detectable, plus deterministic padding.
        char head[96];
        const std::string crc_src = op.key + "|" + std::to_string(seed) + "|" + std::to_string(i);
        std::snprintf(head, sizeof(head), "seed=%llu i=%llu crc=%08x|",
                      static_cast<unsigned long long>(seed), static_cast<unsigned long long>(i),
                      crc32c(crc_src.data(), crc_src.size()));
        op.value = head;
        op.value.append(20 + (h >> 32) % 130, static_cast<char>('a' + (h % 26)));
    }
    return op;
}

// Applies ops [0, count) of the stream to a model map.
void apply_to_model(std::map<std::string, std::string>* model, std::uint64_t seed,
                    std::uint64_t from, std::uint64_t count) {
    for (std::uint64_t i = from; i < count; ++i) {
        Op op = derive_op(seed, i);
        if (op.is_put) {
            (*model)[op.key] = std::move(op.value);
        } else {
            model->erase(op.key);
        }
    }
}

// flock() lives on the open file description, so a concurrent worker's
// fork()->exec() window briefly co-owns our LOCK fd (CLOEXEC releases it at
// exec). That transient Busy is a multi-process-harness artifact, not an
// engine defect — retry through it.
Status open_with_retry(const Options& options, const std::string& dir, DB** db) {
    Status s;
    for (int attempt = 0; attempt < 200; ++attempt) {
        s = DB::open(options, dir, db);
        if (s.ok() || s.code() != Status::Code::kBusy) {
            return s;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return s;
}

Options make_db_options(const std::string& fsync) {
    Options options;
    // Tiny buffers: flushes and compactions constantly in-flight, so kills
    // land inside SST/MANIFEST writes, not just WAL appends.
    options.write_buffer_size = 16 * 1024;
    options.target_file_size = 32 * 1024;
    options.l1_target_bytes = 128 * 1024;
    options.l0_compaction_trigger = 2;
    options.block_size = 1024;
    if (fsync == "always") {
        options.fsync_policy = FsyncPolicy::kAlways;
    } else if (fsync == "interval") {
        options.fsync_policy = FsyncPolicy::kInterval;
        options.wal_sync_interval_ms = 2;
    } else {
        options.fsync_policy = FsyncPolicy::kNever;
    }
    return options;
}

void remove_tree(const std::string& dir) {
    Env* env = Env::default_env();
    std::vector<std::string> children;
    if (env->get_children(dir, &children).ok()) {
        for (const auto& c : children) {
            env->remove_file(dir + "/" + c);
        }
    }
    ::rmdir(dir.c_str());
}

// ---------------------------------------------------------------------------
// Child mode: run the deterministic workload, ack each committed op on fd 3.
// ---------------------------------------------------------------------------

std::string ack_path_for(const std::string& dir) {
    return dir + "/.acks";
}

int child_main(const std::string& dir, std::uint64_t seed, std::uint64_t start, std::uint64_t ops,
               const std::string& fsync) {
    DB* db = nullptr;
    const Status s = open_with_retry(make_db_options(fsync), dir, &db);
    if (!s.ok()) {
        std::fprintf(stderr, "child: open failed: %s\n", s.to_string().c_str());
        return 2;
    }
    const int ack_fd = ::open(ack_path_for(dir).c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (ack_fd < 0) {
        return 5;
    }
    const WriteOptions wo;
    for (std::uint64_t i = start; i < start + ops; ++i) {
        const Op op = derive_op(seed, i);
        const Status ws = op.is_put ? db->put(wo, op.key, op.value) : db->remove(wo, op.key);
        if (!ws.ok()) {
            std::fprintf(stderr, "child: write %llu failed: %s\n",
                         static_cast<unsigned long long>(i), ws.to_string().c_str());
            return 3;
        }
        // Raw write(2): no userspace buffering between commit and ack, and
        // an appended byte survives SIGKILL (page cache outlives us).
        char line[32];
        const int n =
            std::snprintf(line, sizeof(line), "A %llu\n", static_cast<unsigned long long>(i));
        if (::write(ack_fd, line, static_cast<std::size_t>(n)) != n) {
            return 4;
        }
    }
    ::close(ack_fd);
    delete db;
    return 0;
}

// ---------------------------------------------------------------------------
// Orchestrator
// ---------------------------------------------------------------------------

struct IterationConfig {
    std::string dir;
    std::uint64_t seed = 0;
    std::uint64_t start = 0; // chain mode: continue the stream
    std::uint64_t ops = 0;
    std::string fsync;
    std::string kill; // "bytes" | "timer"
    long long crash_at_bytes = -1;
    std::uint64_t timer_micros = 0;
};

struct IterationResult {
    bool ok = false;
    std::string error;
    std::uint64_t acked = 0;   // count of acked ops in this run
    std::uint64_t matched = 0; // prefix length S the recovered state matched
    bool killed = false;       // child died by our SIGKILL
};

std::string g_self_path;

IterationResult run_iteration(const IterationConfig& cfg) {
    IterationResult result;

    // Fresh ack file per run (chain mode reuses the directory).
    ::unlink(ack_path_for(cfg.dir).c_str());
    ::mkdir(cfg.dir.c_str(), 0755); // ensure it exists so the child can ack

    // Environment is assembled before fork: only async-signal-safe calls may
    // run between fork and exec.
    std::string crash_env;
    std::vector<char*> envp;
    if (cfg.crash_at_bytes >= 0) {
        crash_env = "STRATA_CRASH_AT_BYTES=" + std::to_string(cfg.crash_at_bytes);
        envp.push_back(crash_env.data());
    }
    envp.push_back(nullptr);

    const std::string seed_s = std::to_string(cfg.seed);
    const std::string start_s = std::to_string(cfg.start);
    const std::string ops_s = std::to_string(cfg.ops);
    std::vector<const char*> argv = {
        g_self_path.c_str(), "child",           "--dir",         cfg.dir.c_str(), "--seed",
        seed_s.c_str(),      "--start",         start_s.c_str(), "--ops",         ops_s.c_str(),
        "--fsync",           cfg.fsync.c_str(), nullptr};

    const pid_t pid = ::fork();
    if (pid < 0) {
        result.error = "fork() failed";
        return result;
    }
    if (pid == 0) {
        ::execve(g_self_path.c_str(), const_cast<char* const*>(argv.data()), envp.data());
        ::_exit(90); // exec failed
    }

    std::atomic<bool> reaped{false};
    std::thread killer;
    if (cfg.kill == "timer") {
        killer = std::thread([&, pid] {
            std::this_thread::sleep_for(std::chrono::microseconds(cfg.timer_micros));
            if (!reaped.load(std::memory_order_acquire)) {
                ::kill(pid, SIGKILL);
            }
        });
    }

    int status = 0;
    ::waitpid(pid, &status, 0);
    reaped.store(true, std::memory_order_release);
    if (killer.joinable()) {
        killer.join();
    }

    // Read the acks post-mortem. Contiguous by construction; a torn final
    // line (killed mid-append) is ignored.
    std::uint64_t last_acked = cfg.start; // exclusive: ops < last_acked are acked
    {
        std::string pending;
        const int fd = ::open(ack_path_for(cfg.dir).c_str(), O_RDONLY);
        if (fd >= 0) {
            char chunk[4096];
            ssize_t n;
            while ((n = ::read(fd, chunk, sizeof(chunk))) > 0) {
                pending.append(chunk, static_cast<std::size_t>(n));
            }
            ::close(fd);
        }
        std::size_t pos = 0, nl;
        while ((nl = pending.find('\n', pos)) != std::string::npos) {
            unsigned long long idx;
            if (std::sscanf(pending.c_str() + pos, "A %llu", &idx) == 1) {
                if (idx != last_acked) {
                    result.error = "non-contiguous ack " + std::to_string(idx) + " (expected " +
                                   std::to_string(last_acked) + ")";
                    return result;
                }
                last_acked = idx + 1;
            }
            pos = nl + 1;
        }
    }

    const bool clean_exit = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    result.killed = WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
    if (!clean_exit && !result.killed) {
        result.error = "child died abnormally (status=" + std::to_string(status) + ")";
        return result;
    }
    result.acked = last_acked - cfg.start;

    // ---- Verify: reopen and compare against the model. ----
    DB* db = nullptr;
    const Status open_s = open_with_retry(make_db_options(cfg.fsync), cfg.dir, &db);
    if (!open_s.ok()) {
        result.error = "recovery open failed: " + open_s.to_string();
        return result;
    }
    const std::unique_ptr<DB> db_owner(db);

    // Candidate recovered prefixes: exactly the acked count, or acked + the
    // single in-flight op (present-but-unacked is legal). A clean exit pins
    // the prefix to the full run.
    std::vector<std::uint64_t> candidates;
    if (clean_exit) {
        candidates = {cfg.start + cfg.ops};
    } else {
        candidates = {last_acked, last_acked + 1};
    }

    std::vector<std::pair<std::string, std::string>> actual;
    {
        std::unique_ptr<Iterator> it(db->new_iterator(ReadOptions()));
        for (it->seek_to_first(); it->valid(); it->next()) {
            actual.emplace_back(it->key().to_string(), it->value().to_string());
        }
        if (!it->status().ok()) {
            result.error = "scan failed: " + it->status().to_string();
            return result;
        }
    }
    // Assertion B: every surfaced value must verify its embedded checksum —
    // a torn record accepted anywhere would fail here.
    for (const auto& [key, value] : actual) {
        unsigned long long vseed, vi;
        unsigned int vcrc;
        if (std::sscanf(value.c_str(), "seed=%llu i=%llu crc=%08x", &vseed, &vi, &vcrc) != 3) {
            result.error = "unparseable value for key " + key;
            return result;
        }
        const std::string crc_src = key + "|" + std::to_string(vseed) + "|" + std::to_string(vi);
        if (crc32c(crc_src.data(), crc_src.size()) != vcrc) {
            result.error = "embedded checksum mismatch for key " + key;
            return result;
        }
    }

    // Assertions A + C: state equals the model at some candidate prefix.
    for (const std::uint64_t s_ops : candidates) {
        std::map<std::string, std::string> model;
        apply_to_model(&model, cfg.seed, 0, s_ops);
        if (model.size() != actual.size()) {
            continue;
        }
        bool equal = true;
        auto mit = model.begin();
        for (std::size_t i = 0; i < actual.size(); ++i, ++mit) {
            if (actual[i].first != mit->first || actual[i].second != mit->second) {
                equal = false;
                break;
            }
        }
        if (equal) {
            result.matched = s_ops;
            result.ok = true;
            return result;
        }
    }
    // Build a compact diff against the closest candidate for the report.
    std::map<std::string, std::string> model;
    apply_to_model(&model, cfg.seed, 0, candidates.back());
    std::string diff;
    int shown = 0;
    auto ait = actual.begin();
    auto mit = model.begin();
    while ((ait != actual.end() || mit != model.end()) && shown < 4) {
        if (ait == actual.end() || (mit != model.end() && mit->first < ait->first)) {
            diff += " missing:" + mit->first;
            ++mit;
            ++shown;
        } else if (mit == model.end() || ait->first < mit->first) {
            diff += " extra:" + ait->first;
            ++ait;
            ++shown;
        } else {
            if (ait->second != mit->second) {
                diff += " differs:" + ait->first;
                ++shown;
            }
            ++ait;
            ++mit;
        }
    }
    result.error = "recovered state matches no legal prefix (acked=" + std::to_string(last_acked) +
                   " actual_keys=" + std::to_string(actual.size()) +
                   " model_keys=" + std::to_string(model.size()) +
                   " vs S=" + std::to_string(candidates.back()) + ":" + diff + ")";
    return result;
}

int orchestrate(const std::string& base_dir, std::uint64_t iters, unsigned workers,
                const std::string& fsync, const std::string& mode, const std::string& kill,
                std::uint64_t seed) {
    ::mkdir(base_dir.c_str(), 0755);
    std::atomic<std::uint64_t> next_iteration{0};
    std::atomic<std::uint64_t> total_acked{0};
    std::atomic<std::uint64_t> byte_kills{0};
    std::atomic<std::uint64_t> clean_exits{0};
    std::atomic<std::uint64_t> failures{0};
    std::atomic<std::uint64_t> completed{0};

    auto worker_fn = [&](unsigned worker_id) {
        Random rnd(seed * 2654435761u + worker_id + 1);
        const std::string worker_dir = base_dir + "/w" + std::to_string(worker_id);

        std::string chain_dir;
        std::uint64_t chain_seed = 0;
        std::uint64_t chain_next_start = 0;
        int chain_kills_left = 0;

        for (std::uint64_t it = next_iteration.fetch_add(1); it < iters;
             it = next_iteration.fetch_add(1)) {
            IterationConfig cfg;
            cfg.fsync = fsync;
            cfg.kill = kill;
            // Timer children get long runs so the timer usually wins the
            // race and the iteration is a real kill point.
            cfg.ops = kill == "timer" ? 400 + rnd.uniform(1200) : 40 + rnd.uniform(260);

            if (mode == "chain" && chain_kills_left > 0) {
                cfg.dir = chain_dir;
                cfg.seed = chain_seed;
                cfg.start = chain_next_start;
                --chain_kills_left;
            } else {
                if (!chain_dir.empty()) {
                    remove_tree(chain_dir);
                }
                cfg.dir = worker_dir + "-" + std::to_string(it);
                cfg.seed = rnd.next();
                cfg.start = 0;
                if (mode == "chain") {
                    chain_dir = cfg.dir;
                    chain_seed = cfg.seed;
                    chain_kills_left = 7; // kills over the same directory
                }
            }

            if (kill == "bytes") {
                // ~120 WAL bytes/op plus flush/compaction traffic: sweep the
                // whole plausible write range so kills land in WAL records,
                // SSTs, and MANIFEST renames alike.
                cfg.crash_at_bytes = 1 + static_cast<long long>(rnd.next() % (cfg.ops * 240ull));
            } else {
                cfg.timer_micros = 500 + rnd.next() % 30000;
            }

            const IterationResult res = run_iteration(cfg);
            total_acked.fetch_add(res.acked);
            if (res.killed) {
                byte_kills.fetch_add(1);
            } else {
                clean_exits.fetch_add(1);
            }
            if (!res.ok) {
                failures.fetch_add(1);
                std::fprintf(stderr,
                             "FAIL iter=%llu dir=%s seed=%llu start=%llu ops=%llu "
                             "crash_at=%lld timer=%llu: %s\n",
                             static_cast<unsigned long long>(it), cfg.dir.c_str(),
                             static_cast<unsigned long long>(cfg.seed),
                             static_cast<unsigned long long>(cfg.start),
                             static_cast<unsigned long long>(cfg.ops), cfg.crash_at_bytes,
                             static_cast<unsigned long long>(cfg.timer_micros), res.error.c_str());
                chain_kills_left = 0; // abandon a poisoned chain
            } else if (mode == "chain" && cfg.dir == chain_dir) {
                chain_next_start = res.matched;
            }
            if (mode != "chain" || cfg.dir != chain_dir) {
                remove_tree(cfg.dir);
            }
            const std::uint64_t done = completed.fetch_add(1) + 1;
            if (done % 500 == 0) {
                std::fprintf(stderr, "... %llu/%llu iterations (failures=%llu)\n",
                             static_cast<unsigned long long>(done),
                             static_cast<unsigned long long>(iters),
                             static_cast<unsigned long long>(failures.load()));
            }
        }
        if (!chain_dir.empty()) {
            remove_tree(chain_dir);
        }
    };

    std::vector<std::thread> pool;
    for (unsigned w = 0; w < workers; ++w) {
        pool.emplace_back(worker_fn, w);
    }
    for (auto& t : pool) {
        t.join();
    }

    std::printf("crash_test: mode=%s kill=%s fsync=%s iters=%llu kills=%llu "
                "clean_exits=%llu acked_writes_verified=%llu failures=%llu\n",
                mode.c_str(), kill.c_str(), fsync.c_str(), static_cast<unsigned long long>(iters),
                static_cast<unsigned long long>(byte_kills.load()),
                static_cast<unsigned long long>(clean_exits.load()),
                static_cast<unsigned long long>(total_acked.load()),
                static_cast<unsigned long long>(failures.load()));
    return failures.load() == 0 ? 0 : 1;
}

const char* arg_value(int argc, char** argv, const char* name, const char* fallback) {
    for (int i = 0; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return fallback;
}

} // namespace

int main(int argc, char** argv) {
    char resolved[PATH_MAX];
    g_self_path = ::realpath(argv[0], resolved) != nullptr ? resolved : argv[0];

    if (argc >= 2 && std::strcmp(argv[1], "child") == 0) {
        return child_main(arg_value(argc, argv, "--dir", ""),
                          std::strtoull(arg_value(argc, argv, "--seed", "1"), nullptr, 10),
                          std::strtoull(arg_value(argc, argv, "--start", "0"), nullptr, 10),
                          std::strtoull(arg_value(argc, argv, "--ops", "100"), nullptr, 10),
                          arg_value(argc, argv, "--fsync", "always"));
    }
    if (argc >= 2 && std::strcmp(argv[1], "repro") == 0) {
        // Deterministically replay one byte-kill iteration from a FAIL line.
        IterationConfig cfg;
        cfg.dir = arg_value(argc, argv, "--dir", "/tmp/strata-crash-repro");
        cfg.seed = std::strtoull(arg_value(argc, argv, "--seed", "1"), nullptr, 10);
        cfg.start = std::strtoull(arg_value(argc, argv, "--start", "0"), nullptr, 10);
        cfg.ops = std::strtoull(arg_value(argc, argv, "--ops", "100"), nullptr, 10);
        cfg.fsync = arg_value(argc, argv, "--fsync", "always");
        cfg.kill = "bytes";
        cfg.crash_at_bytes = std::strtoll(arg_value(argc, argv, "--crash-at", "-1"), nullptr, 10);
        remove_tree(cfg.dir);
        const IterationResult res = run_iteration(cfg);
        std::printf("repro: ok=%d acked=%llu matched=%llu killed=%d error=%s\n", res.ok,
                    static_cast<unsigned long long>(res.acked),
                    static_cast<unsigned long long>(res.matched), res.killed, res.error.c_str());
        return res.ok ? 0 : 1;
    }
    if (argc >= 2 && std::strcmp(argv[1], "orchestrate") == 0) {
        return orchestrate(arg_value(argc, argv, "--dir", "/tmp/strata-crash"),
                           std::strtoull(arg_value(argc, argv, "--iters", "100"), nullptr, 10),
                           static_cast<unsigned>(
                               std::strtoul(arg_value(argc, argv, "--workers", "4"), nullptr, 10)),
                           arg_value(argc, argv, "--fsync", "always"),
                           arg_value(argc, argv, "--mode", "fresh"),
                           arg_value(argc, argv, "--kill", "bytes"),
                           std::strtoull(arg_value(argc, argv, "--seed", "42"), nullptr, 10));
    }
    std::fprintf(stderr,
                 "usage: %s orchestrate --dir D --iters N --workers W "
                 "--fsync always|interval|never --mode fresh|chain "
                 "--kill bytes|timer [--seed S]\n",
                 argv[0]);
    return 64;
}
