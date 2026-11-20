// LoucuraXXXX GPU - tiny self-contained test framework.
//
// The project avoids external test dependencies so the suite builds anywhere
// the library builds. A test file declares cases with LX_TEST and asserts with
// the LX_CHECK* macros; main() is provided by LX_TEST_MAIN.
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace lx::test {

struct TestCase {
    std::string name;
    std::function<void()> body;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

inline int& failure_count() {
    static int failures = 0;
    return failures;
}

inline int& check_count() {
    static int checks = 0;
    return checks;
}

struct Registrar {
    Registrar(const std::string& name, std::function<void()> body) {
        registry().push_back(TestCase{name, std::move(body)});
    }
};

inline void report_failure(const char* file, int line, const std::string& message) {
    ++failure_count();
    std::cerr << "  FAIL " << file << ":" << line << ": " << message << "\n";
}

// Uniform access to ok()/message() across Status and Result<T>, so a single
// assertion macro can accept either.
inline bool status_ok(const lx::Status& status) { return status.ok(); }
inline std::string status_message(const lx::Status& status) { return status.message(); }
template <typename T>
inline bool status_ok(const lx::Result<T>& result) {
    return result.ok();
}
template <typename T>
inline std::string status_message(const lx::Result<T>& result) {
    return result.status().message();
}

inline int run_all(const std::string& suite_name) {
    std::cout << "== " << suite_name << " ==\n";
    int failed_tests = 0;
    for (auto& test : registry()) {
        const int before = failure_count();
        std::cout << "[ RUN  ] " << test.name << "\n";
        try {
            test.body();
        } catch (const std::exception& ex) {
            report_failure(__FILE__, __LINE__,
                           std::string("uncaught exception: ") + ex.what());
        } catch (...) {
            report_failure(__FILE__, __LINE__, "uncaught non-standard exception");
        }
        if (failure_count() == before) {
            std::cout << "[  OK  ] " << test.name << "\n";
        } else {
            std::cout << "[ FAIL ] " << test.name << "\n";
            ++failed_tests;
        }
    }
    std::cout << "== " << suite_name << ": " << (registry().size() - failed_tests) << "/"
              << registry().size() << " tests passed, " << check_count() << " checks, "
              << failure_count() << " failures ==\n";
    return failed_tests == 0 ? 0 : 1;
}

}  // namespace lx::test

#define LX_TEST(suite, name)                                                     \
    static void suite##_##name##_body();                                         \
    static ::lx::test::Registrar suite##_##name##_registrar(                     \
        #suite "." #name, &suite##_##name##_body);                               \
    static void suite##_##name##_body()

#define LX_CHECK(condition)                                                      \
    do {                                                                         \
        ++::lx::test::check_count();                                             \
        if (!(condition)) {                                                      \
            ::lx::test::report_failure(__FILE__, __LINE__, #condition);          \
        }                                                                        \
    } while (0)

#define LX_CHECK_EQ(a, b)                                                        \
    do {                                                                         \
        ++::lx::test::check_count();                                             \
        auto&& _a = (a);                                                         \
        auto&& _b = (b);                                                         \
        if (!(_a == _b)) {                                                       \
            ::lx::test::report_failure(__FILE__, __LINE__,                       \
                                       std::string(#a " == " #b));               \
        }                                                                        \
    } while (0)

#define LX_CHECK_NEAR(a, b, tolerance)                                           \
    do {                                                                         \
        ++::lx::test::check_count();                                             \
        const double _a = static_cast<double>(a);                                \
        const double _b = static_cast<double>(b);                                \
        if (std::fabs(_a - _b) > (tolerance)) {                                  \
            ::lx::test::report_failure(                                          \
                __FILE__, __LINE__,                                              \
                std::string(#a " ~= " #b " (") + std::to_string(_a) + " vs " +   \
                    std::to_string(_b) + ")");                                   \
        }                                                                        \
    } while (0)

#define LX_CHECK_OK(expr)                                                        \
    do {                                                                         \
        ++::lx::test::check_count();                                             \
        auto&& _status = (expr);                                                 \
        if (!::lx::test::status_ok(_status)) {                                   \
            ::lx::test::report_failure(                                          \
                __FILE__, __LINE__,                                              \
                std::string(#expr " failed: ") + ::lx::test::status_message(_status)); \
        }                                                                        \
    } while (0)

#define LX_TEST_MAIN(suite_name)                                                 \
    int main() { return ::lx::test::run_all(suite_name); }
