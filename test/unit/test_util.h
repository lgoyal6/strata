#pragma once

#include <cstdlib>
#include <string>
#include <vector>

#include <unistd.h>

#include "util/env.h"

namespace strata::test {

// Fresh temp directory per test; recursively destroyed by destroy_dir.
inline std::string make_temp_dir(const std::string& tag) {
    std::string tmpl = "/tmp/strata-test-" + tag + "-XXXXXX";
    char* got = ::mkdtemp(tmpl.data());
    return got != nullptr ? std::string(got) : std::string();
}

inline void destroy_dir(const std::string& dir) {
    if (dir.empty()) {
        return;
    }
    Env* env = Env::default_env();
    std::vector<std::string> children;
    if (env->get_children(dir, &children).ok()) {
        for (const auto& child : children) {
            env->remove_file(dir + "/" + child);
        }
    }
    ::rmdir(dir.c_str());
}

} // namespace strata::test
