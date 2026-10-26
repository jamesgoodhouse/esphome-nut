#pragma once

// Minimal test harness: no dependencies beyond the standard library.

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace test {

struct Case {
  const char *name;
  std::function<void()> fn;
};

inline std::vector<Case> &cases() {
  static std::vector<Case> all;
  return all;
}

inline int &failures() {
  static int count = 0;
  return count;
}

struct Registrar {
  Registrar(const char *name, std::function<void()> fn) { cases().push_back({name, std::move(fn)}); }
};

inline int run_all() {
  for (const auto &c : cases()) {
    int before = failures();
    c.fn();
    std::printf("%s %s\n", failures() == before ? "PASS" : "FAIL", c.name);
  }
  std::printf("%zu tests, %d failed checks\n", cases().size(), failures());
  return failures() == 0 ? 0 : 1;
}

template<typename A, typename B> std::string describe(const A &a, const B &b) {
  std::ostringstream out;
  out << "got [" << a << "] expected [" << b << "]";
  return out.str();
}

}  // namespace test

#define TEST_CONCAT2(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT2(a, b)
#define TEST(name) \
  static void name(); \
  static test::Registrar TEST_CONCAT(registrar_, name)(#name, name); \
  static void name()

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
      test::failures()++; \
    } \
  } while (0)

#define CHECK_EQ(a, b) \
  do { \
    auto _a = (a); \
    auto _b = (b); \
    if (!(_a == _b)) { \
      std::printf("  %s:%d: CHECK_EQ(%s, %s) %s\n", __FILE__, __LINE__, #a, #b, test::describe(_a, _b).c_str()); \
      test::failures()++; \
    } \
  } while (0)
