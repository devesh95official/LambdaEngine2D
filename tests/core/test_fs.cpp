// File IO: atomic writes, removal, mtime, directories, the user data folder and path helpers.
#include <doctest/doctest.h>

#include <cstdlib>
#include <string>

#include <lam/core/fs.h>
#include <lam/test/support.h>

using namespace lam;

TEST_CASE("fs: write_file_atomic then read back") {
    lam::test::TempDir dir;
    const std::string path = dir.file("save.json");
    CHECK_FALSE(file_exists(path));
    REQUIRE(write_file_atomic(path, "hello").ok());
    CHECK(file_exists(path));
    CHECK_FALSE(file_exists(path + ".tmp")); // the temporary is renamed away
    auto text = read_file(path);
    REQUIRE(text.ok());
    CHECK(*text == "hello");
    // Overwriting replaces the old content in one step.
    REQUIRE(write_file_atomic(path, "world!").ok());
    CHECK(read_file(path).value() == "world!");
    auto bytes = read_file_bytes(path);
    REQUIRE(bytes.ok());
    REQUIRE(bytes->size() == 6);
    CHECK(static_cast<char>((*bytes)[0]) == 'w');
    // Empty and binary data.
    REQUIRE(write_file_atomic(path, "").ok());
    CHECK(read_file(path).value().empty());
    const std::string binary("a\0b\xFF\n", 5);
    REQUIRE(write_file_atomic(path, binary).ok());
    CHECK(read_file(path).value() == binary);
    // Larger than one read buffer.
    const std::string big(200000, 'z');
    REQUIRE(write_file_atomic(path, big).ok());
    CHECK(read_file(path).value() == big);
}

TEST_CASE("fs: errors are Results, never exceptions") {
    lam::test::TempDir dir;
    const auto missing = read_file(dir.file("missing.txt"));
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().code == ErrorCode::NotFound);
    CHECK(missing.error().message.find("missing.txt") != std::string::npos);
    CHECK(read_file_bytes(dir.file("missing.txt")).error().code == ErrorCode::NotFound);
    const Status no_dir = write_file_atomic(dir.file("no/such/dir/file.txt"), "x");
    REQUIRE_FALSE(no_dir.ok());
    CHECK(no_dir.error().code == ErrorCode::Io);
    CHECK(file_mtime(dir.file("missing.txt")).error().code == ErrorCode::NotFound);
}

TEST_CASE("fs: remove_file, make_directories and file_mtime") {
    lam::test::TempDir dir;
    const std::string nested = join_path(dir.path(), "a/b/c");
    REQUIRE(make_directories(nested).ok());
    REQUIRE(make_directories(nested).ok()); // already there
    CHECK(make_directories("").ok());
    const std::string path = join_path(nested, "f.txt");
    REQUIRE(write_file_atomic(path, "1").ok());
    auto t1 = file_mtime(path);
    REQUIRE(t1.ok());
    CHECK(*t1 > 0);
    REQUIRE(write_file_atomic(path, "22").ok());
    auto t2 = file_mtime(path);
    REQUIRE(t2.ok());
    CHECK(*t2 >= *t1);
    REQUIRE(remove_file(path).ok());
    CHECK_FALSE(file_exists(path));
    CHECK(remove_file(path).ok()); // a missing file is not an error
}

TEST_CASE("fs: user_data_dir honours LAM_DATA_DIR and creates it") {
    lam::test::TempDir dir;
    const std::string want = join_path(dir.path(), "data/MyGame");
    const char *old = std::getenv("LAM_DATA_DIR");
    const std::string saved = old ? old : "";
    setenv("LAM_DATA_DIR", want.c_str(), 1);
    auto got = user_data_dir("MyGame");
    if (old)
        setenv("LAM_DATA_DIR", saved.c_str(), 1);
    else
        unsetenv("LAM_DATA_DIR");
    REQUIRE(got.ok());
    CHECK(*got == want);
    CHECK(file_exists(want));
}

TEST_CASE("fs: executable_dir and executable_name") {
    auto d = executable_dir();
    REQUIRE(d.ok());
    CHECK_FALSE(d->empty());
    CHECK(file_exists(*d));
    const std::string name = executable_name();
    CHECK_FALSE(name.empty());
    CHECK(file_exists(join_path(*d, name)));
}

TEST_CASE("fs: extension_of starts at the first dot of the file name") {
    CHECK(extension_of("sfx/hit.sounds.json") == ".sounds.json");
    CHECK(extension_of("a/b.png") == ".png");
    CHECK(extension_of("b.png") == ".png");
    CHECK(extension_of("noext").empty());
    CHECK(extension_of("dir.d/file").empty());
    CHECK(extension_of(".hidden").empty());
    CHECK(extension_of("a/.hidden.json") == ".json");
    CHECK(extension_of("").empty());
    CHECK(extension_of("trailing.").size() == 1);
}

TEST_CASE("fs: join_path and parent_path") {
    CHECK(join_path("a", "b") == "a/b");
    CHECK(join_path("a/", "b") == "a/b");
    CHECK(join_path("", "b") == "b");
    CHECK(join_path("a", "") == "a");
    CHECK(join_path("a", "/abs") == "/abs");
    CHECK(join_path("a/b", "c/d.txt") == "a/b/c/d.txt");
    CHECK(parent_path("a/b/c.txt") == "a/b");
    CHECK(parent_path("c.txt").empty());
    CHECK(parent_path("/c") == "/");
    CHECK(parent_path("a/b/") == "a");
    CHECK(parent_path(join_path("x/y", "z")) == "x/y");
}
