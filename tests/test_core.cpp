// Tests for the core primitives: Status, Result, JSON, and file helpers.
#include <cstdio>

#if defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "core/files.hpp"
#include "core/json.hpp"
#include "core/strings.hpp"
#include "core/status.hpp"
#include "test_framework.hpp"

using namespace lx;

LX_TEST(core, status_success_and_error) {
    const Status ok = Status::success();
    LX_CHECK(ok.ok());
    LX_CHECK(ok.code() == ErrorCode::Ok);

    const Status bad = Status::error(ErrorCode::InvalidArgument, "bad input");
    LX_CHECK(!bad.ok());
    LX_CHECK(bad.code() == ErrorCode::InvalidArgument);
    LX_CHECK_EQ(bad.message(), std::string("bad input"));
}

LX_TEST(core, result_carries_value_or_error) {
    Result<int> good{42};
    LX_CHECK(good.ok());
    LX_CHECK_EQ(good.value(), 42);

    Result<int> bad{Status::error(ErrorCode::OutOfRange, "too big")};
    LX_CHECK(!bad.ok());
    LX_CHECK(bad.status().code() == ErrorCode::OutOfRange);
}

LX_TEST(core, json_roundtrip_object) {
    auto parsed = Json::parse(R"({"a": 1, "b": "text", "c": [1,2,3], "d": true})");
    LX_CHECK_OK(parsed.status());
    const Json& root = parsed.value();
    LX_CHECK(root.is_object());
    LX_CHECK_EQ(root.at("a").as_int(), 1);
    LX_CHECK_EQ(root.at("b").as_string(), std::string("text"));
    LX_CHECK_EQ(root.at("c").size(), static_cast<std::size_t>(3));
    LX_CHECK(root.at("d").as_bool());

    const std::string dumped = root.dump(0);
    auto reparsed = Json::parse(dumped);
    LX_CHECK_OK(reparsed.status());
    LX_CHECK_EQ(reparsed.value().at("b").as_string(), std::string("text"));
}

LX_TEST(core, json_rejects_malformed_input) {
    auto bad = Json::parse(R"({"a": })");
    LX_CHECK(!bad.ok());
    LX_CHECK(bad.status().code() == ErrorCode::ParseError);

    auto truncated = Json::parse("[1, 2");
    LX_CHECK(!truncated.ok());
}

LX_TEST(core, json_rejects_excessive_nesting) {
    // Deeply nested containers used to recurse without bound and overflow the
    // stack. The parser must reject them with a clean error instead.
    std::string deep(2000, '[');
    const auto parsed = Json::parse(deep);
    LX_CHECK(!parsed.ok());

    // A modestly nested document still parses.
    const auto shallow = Json::parse("[[[[1]]]]");
    LX_CHECK_OK(shallow.status());
}

LX_TEST(core, json_as_int_clamps_out_of_range_numbers) {
    // A double far outside the integer range must not make the conversion UB;
    // as_int saturates, and a non-number yields the fallback.
    auto parsed = Json::parse(R"({"big": 1e300, "neg": -1e300, "s": "x"})");
    LX_CHECK_OK(parsed.status());
    const Json& root = parsed.value();
    LX_CHECK(root.at("big").as_int() > 0);
    LX_CHECK(root.at("neg").as_int() < 0);
    LX_CHECK_EQ(root.at("s").as_int(7), 7);
}

LX_TEST(core, json_missing_key_returns_null) {
    auto parsed = Json::parse(R"({"present": 1})");
    LX_CHECK_OK(parsed.status());
    LX_CHECK(parsed.value().at("absent").is_null());
    LX_CHECK(!parsed.value().has("absent"));
}

LX_TEST(core, file_roundtrip) {
    const std::string path = "/tmp/lx_test_core_roundtrip.txt";
    const std::string content = "loucuraxxxx gpu";
    LX_CHECK_OK(write_file(path, content));
    LX_CHECK(file_exists(path));
    auto read = read_file(path);
    LX_CHECK_OK(read.status());
    LX_CHECK_EQ(read.value(), content);
    std::remove(path.c_str());
}

LX_TEST(core, make_directories_creates_nested_paths) {
    // A multi-level path whose intermediate directories do not exist yet: a
    // single mkdir would fail, so this must create every ancestor.
    const std::string dir = "lx_test_nested/a/b/c";
    LX_CHECK_OK(make_directories(dir));
    const std::string file = dir + "/leaf.txt";
    LX_CHECK_OK(write_file(file, "ok"));
    auto read = read_file(file);
    LX_CHECK_OK(read.status());
    LX_CHECK_EQ(read.value(), std::string("ok"));
    std::remove(file.c_str());
    // make_directories is idempotent on an existing tree.
    LX_CHECK_OK(make_directories(dir));
    // Leave the working directory as we found it: remove the tree we created.
    for (const char* d : {"lx_test_nested/a/b/c", "lx_test_nested/a/b", "lx_test_nested/a",
                          "lx_test_nested"}) {
#if defined(_WIN32)
        _rmdir(d);
#else
        ::rmdir(d);
#endif
    }
}

LX_TEST(core, read_missing_file_reports_error) {
    auto missing = read_file("/tmp/lx_definitely_missing_file_xyz");
    LX_CHECK(!missing.ok());
    LX_CHECK(missing.status().code() == ErrorCode::IoError);
}

LX_TEST(core, device_name_match_ignores_case) {
    // GPU preference matching is ASCII-case-insensitive and substring-based, so
    // "4090" selects "NVIDIA GeForce RTX 4090" without the exact string. This is
    // the rule the Vulkan backend applies to preferred_device.
    LX_CHECK(contains_ci("NVIDIA GeForce RTX 4090", "4090"));
    LX_CHECK(contains_ci("NVIDIA GeForce RTX 4090", "geforce"));
    LX_CHECK(contains_ci("NVIDIA GeForce RTX 4090", "GEFORCE"));
    LX_CHECK(contains_ci("Apple M4", "apple m4"));
    LX_CHECK(!contains_ci("llvmpipe (LLVM 15.0.7, 256 bits)", "4090"));
    // An empty preference means "no preference", not "matches nothing".
    LX_CHECK(contains_ci("anything", ""));
    // Non-ASCII bytes are compared unchanged rather than mangled.
    LX_CHECK_EQ(ascii_lower("ABC"), std::string("abc"));
    LX_CHECK_EQ(ascii_lower("abc"), std::string("abc"));
    LX_CHECK_EQ(ascii_lower("A1-_z"), std::string("a1-_z"));
}

LX_TEST_MAIN("core")
