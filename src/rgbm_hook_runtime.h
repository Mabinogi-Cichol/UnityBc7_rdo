#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

namespace rgbm::runtime {
enum class State : uint32_t { Disabled, Resolving, Validating, Installing, Ready, Unavailable, Faulted };
enum class Context : uint32_t { Unknown, Ordinary, RGBM };
using Callback = void (*)(void*, int, int);
using Process = void (*)(int, const void*, Callback);
using Compress = void (*)(void*, uint32_t, uint32_t, int, bool, bool, bool, uint8_t*, const void*);
constexpr unsigned slot_count = 64;
State state();
const char* state_name(State);
Context current_context();
Context context_for_usage(uint32_t usage);
State initialize(bool enabled, const wchar_t* cache, const wchar_t* coordinator_directory, std::string& error);
// Never remove Unity hooks: original slots, trampolines and modules live to exit.
// The native Process contract guarantees a synchronous join on NORMAL return.
void invoke_process(Process native, int rows, const void* job, Callback callback);
void invoke_compress(Compress native, void* image, uint32_t format, uint32_t usage,
                     int quality, bool flag0, bool flag1, bool flag2, uint8_t* out, const void* texture);
struct EncodingDecision { bool native_only, rgbm; int oodle_lambda; };
EncodingDecision encoding_decision(bool enabled, int ordinary_lambda, int rgbm_lambda);
// Internal state publication shared by binding installation and callback engine.
void transition(State);
bool publish_ready();
[[noreturn]] void fail_closed(unsigned code);
class ContextScope {
    Context previous_;
public:
    explicit ContextScope(Context);
    ~ContextScope();
    ContextScope(const ContextScope&) = delete;
    ContextScope& operator=(const ContextScope&) = delete;
};
#ifdef RGBM_RUNTIME_TESTING
void test_ready();
unsigned occupied_slots();
Callback test_acquire(Callback, Context, unsigned& slot);
void test_release_after_join(unsigned slot);
#endif
} // namespace rgbm::runtime
