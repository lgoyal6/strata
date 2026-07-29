// No-framework build/link/behavior canary (taut style): returns nonzero on
// any failure. The real coverage lives in test/unit/.
#include <cassert>
#include <cstdio>
#include <string>

#include "db/dbformat.h"
#include "db/memtable.h"
#include "db/write_batch_internal.h"
#include "strata/write_batch.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "util/env.h"
#include "wal/wal_reader.h"
#include "wal/wal_writer.h"

using namespace strata;

int main() {
    // CRC32C known-answer vector.
    assert(crc32c("123456789", 9) == 0xE3069283u);
    assert(crc32c_unmask(crc32c_mask(0x12345678u)) == 0x12345678u);

    // Varint round-trip incl. boundaries.
    for (std::uint64_t v :
         {0ull, 1ull, 127ull, 128ull, 16383ull, 16384ull, (1ull << 32) - 1, 1ull << 32, ~0ull}) {
        std::string s;
        put_varint64(&s, v);
        Slice in(s);
        std::uint64_t got = 0;
        const bool ok = get_varint64(&in, &got) && got == v && in.empty();
        assert(ok);
        (void)ok;
    }

    // Internal key ordering: same user key, higher seq sorts first.
    InternalKeyComparator icmp;
    std::string k1, k2;
    append_internal_key(&k1, "apple", 5, kTypeValue);
    append_internal_key(&k2, "apple", 9, kTypeValue);
    assert(icmp.compare(Slice(k2), Slice(k1)) < 0);

    // Memtable visibility.
    MemTable mem(icmp);
    mem.add(10, kTypeValue, "k", "v10");
    mem.add(20, kTypeValue, "k", "v20");
    mem.add(30, kTypeDeletion, "k", "");
    std::string val;
    Status st;
    assert(mem.get(LookupKey("k", 25), &val, &st) && st.ok() && val == "v20");
    assert(mem.get(LookupKey("k", 15), &val, &st) && st.ok() && val == "v10");
    assert(mem.get(LookupKey("k", 35), &val, &st) && st.is_not_found()); // tombstone
    assert(!mem.get(LookupKey("nope", 35), &val, &st));

    // WAL round-trip + torn-tail rejection.
    Env* env = Env::default_env();
    const std::string wal_path = "/tmp/strata-smoke.wal";
    {
        std::unique_ptr<WalWriter> w;
        assert(WalWriter::create(env, wal_path, 0xabcdefull, &w).ok());
        WriteBatch b;
        b.put("key1", "value1");
        b.remove("key2");
        WriteBatchInternal::set_sequence(&b, 42);
        assert(w->add_record(WriteBatchInternal::contents(&b)).ok());
        assert(w->add_record("second-record").ok());
        assert(w->sync(false).ok());
    }
    {
        std::unique_ptr<WalReader> r;
        assert(WalReader::open(env, wal_path, 0xabcdefull, &r).ok());
        std::string rec;
        assert(r->read_record(&rec));
        assert(WriteBatchInternal::check(Slice(rec)).ok());
        WriteBatch b2;
        WriteBatchInternal::set_contents(&b2, Slice(rec));
        assert(WriteBatchInternal::sequence(&b2) == 42 && b2.count() == 2);
        assert(r->read_record(&rec) && rec == "second-record");
        assert(!r->read_record(&rec) && !r->truncated_tail());
    }
    // Torn tail: truncate the file at every byte length and verify no
    // garbage record is ever surfaced.
    std::uint64_t full_size = 0;
    assert(env->get_file_size(wal_path, &full_size).ok());
    for (std::uint64_t cut = 0; cut < full_size; ++cut) {
        std::string data;
        {
            std::unique_ptr<SequentialFile> f;
            assert(env->new_sequential_file(wal_path, &f).ok());
            data.resize(cut);
            Slice out;
            if (cut > 0) {
                assert(f->read(cut, &out, data.data()).ok());
            }
        }
        const std::string cut_path = "/tmp/strata-smoke-cut.wal";
        {
            std::unique_ptr<WritableFile> f;
            assert(env->new_writable_file(cut_path, &f).ok());
            assert(f->append(Slice(data.data(), cut)).ok());
            assert(f->close().ok());
        }
        std::unique_ptr<WalReader> r;
        const Status s = WalReader::open(env, cut_path, 0xabcdefull, &r);
        if (!s.ok()) {
            continue; // torn file header: rejected at open, fine
        }
        std::string rec;
        int n = 0;
        while (r->read_record(&rec)) {
            assert(WriteBatchInternal::check(Slice(rec)).ok());
            ++n;
        }
        assert(n <= 2);
    }

    std::printf("smoke: all assertions passed\n");
    return 0;
}
