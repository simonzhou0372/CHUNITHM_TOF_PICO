#pragma once
#include <coroutine>
#include <cstddef>
#include <utility>

namespace Chuni245Tof {
// Core1-only, fixed storage. No malloc/newlib allocator or cross-core mutex.
// Compiler-generated state machines preserve the existing register sequence.
struct TaskFrames {
    static constexpr size_t count = 24, size = 1024;
    alignas(std::max_align_t) inline static unsigned char data[count][size]{};
    inline static bool used[count]{};
    inline static volatile unsigned failures = 0;
    static void* allocate(size_t bytes) noexcept {
        if (bytes <= size) {
            for (size_t i = 0; i < count; ++i) {
                if (!used[i]) { used[i] = true; return data[i]; }
            }
        }
        failures = failures + 1;
        return nullptr;
    }
    static void release(void* ptr) noexcept {
        for (size_t i = 0; i < count; ++i) {
            if (ptr == data[i]) { used[i] = false; return; }
        }
    }
};
struct TaskNode {
    std::coroutine_handle<> handle{};
    TaskNode* child = nullptr;
};
template<class T> class Task {
public:
    struct promise_type : TaskNode {
        T value{};
        static void* operator new(size_t n) noexcept { return TaskFrames::allocate(n); }
        static void operator delete(void* p, size_t) noexcept { TaskFrames::release(p); }
        static Task get_return_object_on_allocation_failure() { return {}; }
        Task get_return_object() {
            auto h = std::coroutine_handle<promise_type>::from_promise(*this);
            handle = h;
            return Task(h);
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_value(T result) noexcept { value = result; }
        void unhandled_exception() noexcept { __builtin_trap(); }
    };
    using Handle = std::coroutine_handle<promise_type>;
    Task() = default;
    explicit Task(Handle h) : handle_(h) {}
    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    Task& operator=(Task&& other) noexcept {
        if (this != &other) { clear(); handle_ = std::exchange(other.handle_, {}); }
        return *this;
    }
    Task(const Task&) = delete;
    ~Task() { clear(); }
    void clear() { if (handle_) { handle_.destroy(); handle_ = {}; } }
    bool valid() const { return bool(handle_); }
    bool done() const { return !handle_ || handle_.done(); }
    T result() const { return handle_ ? handle_.promise().value : T{}; }
    // Resume just one leaf, never recursively spin until I/O/delay completes.
    void step() {
        if (done()) return;
        TaskNode* node = &handle_.promise();
        for (unsigned depth = 0; node->child && depth < TaskFrames::count; ++depth) {
            if (node->child->handle.done()) { node->child = nullptr; break; }
            node = node->child;
        }
        node->handle.resume();
    }
    bool await_ready() const { return done(); }
    template<class P> void await_suspend(std::coroutine_handle<P> parent) {
        parent.promise().child = &handle_.promise();
    }
    T await_resume() const { return result(); }
private:
    Handle handle_{};
};
}
