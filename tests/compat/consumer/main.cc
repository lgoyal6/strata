#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include <strata/db.h>
#include <strata/options.h>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: strata_compat_consumer DB_DIR\n";
        return 2;
    }
    std::filesystem::remove_all(argv[1]);
    strata::Options options;
    options.create_if_missing = true;
    strata::DB* raw = nullptr;
    auto status = strata::DB::open(options, argv[1], &raw);
    if (!status.ok()) {
        std::cerr << status.to_string() << '\n';
        return 1;
    }
    std::unique_ptr<strata::DB> db(raw);
    status = db->put(strata::WriteOptions{}, "compat-key", "compat-value");
    std::string value;
    if (!status.ok() || !db->get(strata::ReadOptions{}, "compat-key", &value).ok() ||
        value != "compat-value") {
        std::cerr << "installed package failed the retained consumer workflow\n";
        return 1;
    }
    std::cout << "strata " << STRATA_PACKAGE_VERSION << " consumer ok\n";
}
