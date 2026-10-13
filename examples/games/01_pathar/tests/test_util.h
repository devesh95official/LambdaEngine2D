#pragma once
// Shared helpers for Pathar's tests.
#include <string>
#include <doctest/doctest.h>
#include <lam/core/fs.h>
#include "sim/catalogue.h"

namespace pathar::test {

// The shipped level list (assets/data/levels.json), parsed once.
inline const Catalogue &catalogue() {
    static const Catalogue cat = [] {
        lam::Result<std::string> text = lam::read_file(std::string(PATHAR_ASSETS) + "/data/levels.json");
        REQUIRE(text.ok());
        lam::Result<Catalogue> c = Catalogue::parse(text.value());
        REQUIRE_MESSAGE(c.ok(), (c.ok() ? std::string() : c.error().message));
        return c.value();
    }();
    return cat;
}

}  // namespace pathar::test
