// Native handoff/regression fixture for bundled libc++, not Microsoft STL.
// Build as an EXE, with -DMSVC_LIBCXX_DLL -shared, or with
// -DMSVC_LIBCXX_CLIENT plus the DLL's import library. C++20 is required.
#include <atomic>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <new>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>

#ifndef _LIBCPP_VERSION
# error This fixture must use bundled libc++ headers.
#endif

#define CHECK(test) do { if (!(test)) throw std::runtime_error(#test); } while (false)

static void new_handler_probe() { throw std::bad_alloc(); }
struct Base { virtual ~Base() = default; };
struct Derived : Base { int value = 42; };
struct alignas(128) Aligned { int value = 42; };

static int exercise_libcxx() {
    std::string text = "bundled ";
    text += "libc++";
    CHECK(text == "bundled libc++");
    std::vector<int> values{10, 12, 20};
    CHECK(std::accumulate(values.begin(), values.end(), 0) == 42);
    CHECK(std::string(100, 'x').substr(2, 4) == "xxxx");
    try {
        (void)values.at(9);
        CHECK(false);
    } catch (const std::out_of_range& e) {
        CHECK(*e.what() != '\0');
    }
    std::atomic<int> result{0};
    std::thread worker([&] { result.store(42); });
    worker.join();
    CHECK(result.load() == 42);
    auto async = std::async(std::launch::async, [] { return std::string("future"); });
    CHECK(async.get() == "future");
    std::exception_ptr ep;
    try { throw std::runtime_error(text); }
    catch (...) { ep = std::current_exception(); }
    CHECK(ep != nullptr);
    try {
        std::rethrow_exception(ep);
        CHECK(false);
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()) == text);
    }
    std::unique_ptr<Base> base = std::make_unique<Derived>();
    CHECK(dynamic_cast<Derived*>(base.get())->value == 42);
    Base* polymorphic = base.get();
    CHECK(typeid(*polymorphic) == typeid(Derived));
    auto* aligned = new Aligned;
    CHECK(reinterpret_cast<std::size_t>(aligned) % alignof(Aligned) == 0);
    CHECK(aligned->value == 42);
    delete aligned;
    auto old = std::set_new_handler(new_handler_probe);
    CHECK(std::get_new_handler() == new_handler_probe);
    CHECK(std::set_new_handler(old) == new_handler_probe);
    auto* ordinary = new (std::nothrow) int[16];
    CHECK(ordinary != nullptr);
    delete[] ordinary;
    return 42;
}

#if defined(MSVC_LIBCXX_DLL)
extern "C" __declspec(dllexport) int libcxx_dll_check() { return exercise_libcxx(); }
extern "C" __declspec(dllexport) void libcxx_dll_throw() { throw std::runtime_error("DLL exception"); }
#else
# if defined(MSVC_LIBCXX_CLIENT)
extern "C" __declspec(dllimport) int libcxx_dll_check();
extern "C" __declspec(dllimport) void libcxx_dll_throw();
# endif
int main() {
    try {
        CHECK(exercise_libcxx() == 42);
# if defined(MSVC_LIBCXX_CLIENT)
        CHECK(libcxx_dll_check() == 42);
        try {
            libcxx_dll_throw();
            CHECK(false);
        } catch (const std::runtime_error& e) {
            CHECK(std::string(e.what()) == "DLL exception");
        }
# endif
        std::cout << "libc++ msvc: ok 42\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "libc++ msvc: FAIL " << e.what() << '\n';
        return 1;
    }
}
#endif
