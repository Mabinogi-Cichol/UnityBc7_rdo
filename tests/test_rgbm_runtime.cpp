#include "rgbm_hook_runtime.h"
#include <atomic>
#include <thread>
#include <vector>
#include <stdexcept>
#include <cstdio>
#include <cstring>
using namespace rgbm::runtime;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::atomic<unsigned> failed{0}, completed{0}, callbacks_seen{0};
std::atomic<bool> callback_active{false};
struct Job { Context expected; bool nested; uint64_t token; };
void work(void* data, int begin, int end);
void process(int rows, const void* input, Callback callback) {
    if (rows <= 0) return;
    Job copied = *static_cast<const Job*>(input);
    std::thread worker([&]() {
        ContextScope unrelated(Context::RGBM);
        const auto before = current_context();
        callback(&copied, 0, rows);
        if (current_context() != before) ++failed;
    });
    worker.join(); ++completed;
}
void work(void* data, int begin, int end) {
    const Job copied = *static_cast<Job*>(data);
    auto before = current_context();
    if (before != copied.expected || begin != 0 || end != 9 || copied.token != 0x123456789ABCDEF0ull) ++failed;
    if (copied.nested) {
        ContextScope semantic(Context::Ordinary);
        Job child{Context::Ordinary, false, copied.token};
        invoke_process(&process, 9, &child, &work);
        if (current_context() != Context::Ordinary) ++failed;
    }
    if (current_context() != before) ++failed;
    ++callbacks_seen;
}
void compress(void* image, uint32_t format, uint32_t usage, int quality, bool f0, bool f1, bool f2, uint8_t* out, const void* texture) {
    require(format == 42 && quality == 73 && f0 && !f1 && f2 && image == texture && out, "compress ABI forwarding");
    require(current_context() == context_for_usage(usage), "semantic context");
    Job* job = static_cast<Job*>(image); job->expected = context_for_usage(usage);
    invoke_process(&process, 9, job, &work); *out = 91;
}
void throw_cpp(void*, int, int) { throw std::runtime_error("expected"); }
void throw_seh(void*, int, int) { RaiseException(0xE0427777, 0, 0, nullptr); }
bool catch_seh(Callback cb) {
    __try { cb(nullptr, 0, 1); }
    __except(GetExceptionCode() == 0xE0427777 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return true; }
    return false;
}
void failing_process(int, const void*, Callback) { throw std::runtime_error("unjoined"); }
void failing_seh_process(int, const void*, Callback) { RaiseException(0xE0427777, 0, 0, nullptr); }
void block_callback(void*, int, int) { callback_active = true; Sleep(INFINITE); }
void return_before_join(int, const void*, Callback cb) {
    std::thread([cb]() { cb(nullptr, 0, 1); }).detach();
    while (!callback_active.load()) SwitchToThread();
}
void direct_process(int, const void*, Callback cb) { cb(nullptr, 0, 1); }
void require_unknown(void*, int, int) { require(current_context() == Context::Unknown, "exhaustion did not suppress unrelated TLS"); }
void throw_compress(void*, uint32_t, uint32_t, int, bool, bool, bool, uint8_t*, const void*) { throw std::runtime_error("semantic exception"); }
int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    test_ready();
    if (argc > 1 && !strcmp(argv[1], "unjoined-cpp")) {
        // A handler makes C++ perform phase-two unwinding. Without one the CRT
        // aborts immediately, which does not exercise our __finally policy.
        try { invoke_process(&failing_process, 9, nullptr, &throw_cpp); }
        catch (...) { return 2; }
    }
    if (argc > 1 && !strcmp(argv[1], "unjoined-seh")) invoke_process(&failing_seh_process, 9, nullptr, &throw_seh);
    if (argc > 1 && !strcmp(argv[1], "join-contract-violation")) invoke_process(&return_before_join, 9, nullptr, &block_callback);
    try {
        for (uint32_t usage = 0; usage < 16; ++usage)
            require(context_for_usage(usage) == ((usage == 2 || usage == 5 || usage == 9) ? Context::RGBM : Context::Ordinary), "usage contract");
        require(context_for_usage(UINT32_MAX) == Context::Ordinary, "usage range");
        std::vector<std::thread> callers;
        for (unsigned i = 0; i < 16; ++i) callers.emplace_back([i]() {
            try {
                Job reused{Context::Unknown, true, 0x123456789ABCDEF0ull};
                for (unsigned j = 0; j < 80; ++j) {
                    uint32_t usage = (i + j) % 2 ? 2 : 0; uint8_t out = 0;
                    invoke_compress(&compress, &reused, 42, usage, 73, true, false, true, &out, &reused);
                    require(out == 91 && current_context() == Context::Unknown, "writable flag or TLS leak");
                }
            } catch (...) { ++failed; }
        });
        for (auto& caller : callers) caller.join();
        Job unknown{Context::Unknown, false, 0x123456789ABCDEF0ull};
        invoke_process(&process, 9, &unknown, &work);
        invoke_process(&process, 0, nullptr, nullptr);
        require(!failed && completed == 2561 && callbacks_seen == 2561 && occupied_slots() == 0, "concurrent/copy/nested/address reuse");
        unsigned index;
        { ContextScope outer(Context::Ordinary);
            auto cb = test_acquire(&throw_cpp, Context::RGBM, index);
            bool caught = false; try { cb(nullptr, 0, 1); } catch (...) { caught = true; }
            require(caught && current_context() == Context::Ordinary, "callback C++ TLS restore");
            test_release_after_join(index); // Direct invocation has returned; no queued callers.
            cb = test_acquire(&throw_seh, Context::RGBM, index);
            require(catch_seh(cb) && current_context() == Context::Ordinary, "callback SEH TLS restore");
            test_release_after_join(index);
            caught = false;
            try { invoke_compress(&throw_compress, nullptr, 0, 2, 0, false, false, false, nullptr, nullptr); } catch (...) { caught = true; }
            require(caught && current_context() == Context::Ordinary, "compress C++ TLS restore");
        }
        for (auto value : {State::Disabled, State::Resolving, State::Validating, State::Installing, State::Unavailable, State::Faulted}) {
            transition(value); ContextScope semantic(Context::RGBM);
            auto d = encoding_decision(true, 30, 10); require(d.native_only && !d.rgbm, "non-ready policy");
            d = encoding_decision(false, 30, 10); require(!d.native_only && !d.rgbm && d.oodle_lambda == 30, "explicit disabled policy");
        }
        test_ready(); require(encoding_decision(true, 30, 10).native_only, "Ready Unknown policy");
        { ContextScope ordinary(Context::Ordinary); auto d = encoding_decision(true, 0, 10); require(!d.native_only && !d.rgbm && d.oodle_lambda == 0, "ordinary zero lambda"); }
        { ContextScope semantic(Context::RGBM);
            for (int lambda : {-1, 0, 10}) { auto d = encoding_decision(true, 30, lambda); require(!d.native_only && d.rgbm && d.oodle_lambda == (lambda < 0 ? 30 : lambda), "RGBM lambda inheritance/zero/override"); }
        }
        unsigned full[slot_count];
        for (auto& slot : full) require(test_acquire(&require_unknown, Context::RGBM, slot), "pool capacity");
        { ContextScope dirty(Context::RGBM); invoke_process(&direct_process, 1, nullptr, &require_unknown); }
        require(state() == State::Unavailable && current_context() == Context::Unknown, "sticky exhaustion");
        for (auto slot : full) test_release_after_join(slot);
        require(occupied_slots() == 0 && state() == State::Unavailable, "exhaustion cleanup reenabled classification");
        puts("{\"passed\":true,\"jobs\":2561,\"callbacks\":2561,\"slots_remaining\":0,\"cpp_and_seh_tls_restore\":true,\"sticky_exhaustion_native\":true,\"encoding_policy\":true}");
        return 0;
    } catch (const std::exception& e) { fprintf(stderr, "%s\n", e.what()); return 1; }
}
