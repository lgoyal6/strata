// A per-device event log on strata: append timestamped events, read one
// device's window back, and survive a reopen.
//
// This is the shape strata is actually good at - ordered keys, append-heavy
// writes, range reads - and it exercises the API an outside consumer needs:
// WriteBatch atomicity, range scan via seek + prefix bound, snapshot
// isolation, and durability across a close/open cycle.
//
// Keys are "dev/<device>/<zero-padded millis>" so that a scan from
// "dev/<device>/<start>" walks that device's events in time order and stops
// at the first key outside the prefix.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <strata/db.h>
#include <strata/iterator.h>
#include <strata/options.h>
#include <strata/write_batch.h>

namespace {

std::string event_key(const std::string& device, std::uint64_t millis) {
    std::ostringstream out;
    out << "dev/" << device << "/" << std::setw(13) << std::setfill('0') << millis;
    return out.str();
}

// Fails loudly: an example that ignores Status teaches the wrong thing.
void must(const strata::Status& s, const char* what) {
    if (!s.ok()) {
        std::fprintf(stderr, "%s: %s\n", what, s.to_string().c_str());
        std::exit(1);
    }
}

int count_window(strata::DB* db, const std::string& device, std::uint64_t from,
                 const strata::Snapshot* snap = nullptr) {
    strata::ReadOptions ro;
    ro.snapshot = snap;
    const std::unique_ptr<strata::Iterator> it(db->new_iterator(ro));
    const std::string prefix = "dev/" + device + "/";
    int n = 0;
    for (it->seek(event_key(device, from)); it->valid(); it->next()) {
        const std::string key = it->key().to_string();
        if (key.compare(0, prefix.size(), prefix) != 0) {
            break; // walked out of this device's range
        }
        ++n;
    }
    must(it->status(), "scan");
    return n;
}

} // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "/tmp/strata-eventlog-db";

    strata::Options options;
    options.create_if_missing = true;
    strata::DB* raw = nullptr;
    must(strata::DB::open(options, dir, &raw), "open");
    std::unique_ptr<strata::DB> db(raw);

    // One batch per device tick: either every event lands or none does.
    const std::vector<std::string> devices = {"sensor-a", "sensor-b", "sensor-c"};
    constexpr int kTicks = 2000;
    for (int tick = 0; tick < kTicks; ++tick) {
        strata::WriteBatch batch;
        for (const std::string& device : devices) {
            batch.put(event_key(device, 1'700'000'000'000ull + static_cast<std::uint64_t>(tick)),
                      "{\"tick\":" + std::to_string(tick) + "}");
        }
        must(db->write(strata::WriteOptions(), &batch), "write");
    }
    std::printf("wrote %d events (%d ticks x %zu devices)\n",
                kTicks * static_cast<int>(devices.size()), kTicks, devices.size());

    // Range read: one device's events, not the whole keyspace.
    const int all_a = count_window(db.get(), "sensor-a", 0);
    const int tail_a = count_window(db.get(), "sensor-a", 1'700'000'000'000ull + 1500);
    std::printf("sensor-a: %d events total, %d in the last window\n", all_a, tail_a);

    // Snapshot isolation: the snapshot must not see writes taken after it.
    const strata::Snapshot* snap = db->get_snapshot();
    strata::WriteBatch late;
    for (int tick = kTicks; tick < kTicks + 50; ++tick) {
        late.put(event_key("sensor-a", 1'700'000'000'000ull + static_cast<std::uint64_t>(tick)),
                 "late");
    }
    must(db->write(strata::WriteOptions(), &late), "late write");
    const int at_snapshot = count_window(db.get(), "sensor-a", 0, snap);
    const int now = count_window(db.get(), "sensor-a", 0);
    db->release_snapshot(snap);
    std::printf("snapshot sees %d, current sees %d (expected %d and %d)\n", at_snapshot, now,
                kTicks, kTicks + 50);

    // Durability: close, reopen, and confirm the data is still there.
    db.reset();
    must(strata::DB::open(options, dir, &raw), "reopen");
    db.reset(raw);
    const int after_reopen = count_window(db.get(), "sensor-a", 0);
    std::printf("after reopen: %d events\n", after_reopen);

    const bool ok = all_a == kTicks && at_snapshot == kTicks && now == kTicks + 50 &&
                    after_reopen == kTicks + 50;
    std::printf("%s\n", ok ? "OK" : "MISMATCH");
    return ok ? 0 : 1;
}
