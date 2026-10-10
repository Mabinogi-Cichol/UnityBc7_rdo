#include "rgbm_hook_runtime.h"
#include <array>
#include <atomic>
#include <utility>
#include <intrin.h>

namespace rgbm::runtime {
namespace {
std::atomic<State> status{State::Unavailable};
thread_local Context tls = Context::Unknown;
struct Slot { Callback original = nullptr; Context context = Context::Unknown; unsigned active = 0; bool occupied = false; };
std::array<Slot, slot_count> slots{};
SRWLOCK slot_lock = SRWLOCK_INIT;
unsigned acquire(Callback original, Context context) {
    if (!original) return slot_count;
    AcquireSRWLockExclusive(&slot_lock);
    unsigned index = 0;
    for (; index < slot_count; ++index) if (!slots[index].occupied) {
        slots[index] = {original, context, 0, true}; break;
    }
    ReleaseSRWLockExclusive(&slot_lock);
    return index;
}
void release(unsigned index) {
    AcquireSRWLockExclusive(&slot_lock);
    bool valid = index < slot_count && slots[index].occupied && slots[index].active == 0;
    if (valid) slots[index] = {};
    ReleaseSRWLockExclusive(&slot_lock);
    if (!valid) fail_closed(0xE0427002u);
}
template<unsigned Index> void callback(void* job, int begin, int end) {
    Callback original;
    Context context;
    AcquireSRWLockExclusive(&slot_lock);
    original = slots[Index].original;
    bool valid = slots[Index].occupied && original;
    context = slots[Index].context;
    if (valid) ++slots[Index].active;
    ReleaseSRWLockExclusive(&slot_lock);
    if (!valid) fail_closed(0xE0427003u);
    const Context previous = tls;
    tls = context;
    __try { original(job, begin, end); }
    __finally {
        tls = previous;
        AcquireSRWLockExclusive(&slot_lock);
        --slots[Index].active;
        ReleaseSRWLockExclusive(&slot_lock);
    }
}
template<size_t... Index> constexpr auto functions(std::index_sequence<Index...>) {
    return std::array<Callback, sizeof...(Index)>{&callback<Index>...};
}
const auto callbacks = functions(std::make_index_sequence<slot_count>{});
}
State state() { return status.load(std::memory_order_acquire); }
void transition(State value) { status.store(value, std::memory_order_release); }
bool publish_ready() {
    State expected = State::Installing;
    return status.compare_exchange_strong(expected, State::Ready);
}
const char* state_name(State value) {
    switch (value) {
    case State::Disabled: return "Disabled"; case State::Resolving: return "Resolving";
    case State::Validating: return "Validating"; case State::Installing: return "Installing";
    case State::Ready: return "Ready"; case State::Unavailable: return "Unavailable";
    default: return "Faulted";
    }
}
Context current_context() { return state() == State::Ready ? tls : Context::Unknown; }
Context context_for_usage(uint32_t usage) {
    // Semantic of the certified v1 predicate: cmp usage,9; bt 0x224,usage.
    return usage <= 9 && ((0x224u >> usage) & 1) ? Context::RGBM : Context::Ordinary;
}
ContextScope::ContextScope(Context value) : previous_(tls) { tls = value; }
ContextScope::~ContextScope() { tls = previous_; }
[[noreturn]] void fail_closed(unsigned code) {
    transition(State::Faulted);
    // No log/allocator/loader locks, unwinding or cleanup: Unity's copied Job
    // may still be referenced by a queued callback. Quarantine cannot extend it.
    TerminateProcess(GetCurrentProcess(), code);
    __fastfail(7);
}
void invoke_compress(Compress native, void* image, uint32_t format, uint32_t usage,
                     int quality, bool f0, bool f1, bool f2, uint8_t* out, const void* texture) {
    if (!native) fail_closed(0xE0427004u);
    const Context previous = tls;
    tls = context_for_usage(usage);
    __try { native(image, format, usage, quality, f0, f1, f2, out, texture); }
    __finally { tls = previous; }
}
void invoke_process(Process native, int rows, const void* job, Callback original) {
    if (!native) fail_closed(0xE0427004u);
    // A null callback is harmless only on the certified no-scheduling path.
    if (rows > 0 && !original) fail_closed(0xE0427005u);
    unsigned index = rows > 0 ? acquire(original, current_context()) : slot_count;
    if (rows > 0 && index == slot_count) {
        // Sticky degradation covers even workers carrying an unrelated RGBM TLS.
        transition(State::Unavailable);
    }
    bool joined = false;
    __try {
        native(rows, job, index == slot_count ? original : callbacks[index]);
        joined = true;
    } __finally {
        if (!joined) fail_closed(0xE0427001u);
        if (index != slot_count) release(index);
    }
}
EncodingDecision encoding_decision(bool enabled, int ordinary_lambda, int rgbm_lambda) {
    const Context context = enabled ? current_context() : Context::Ordinary;
    bool is_rgbm = context == Context::RGBM;
    return {enabled && context == Context::Unknown, is_rgbm,
            is_rgbm && rgbm_lambda >= 0 ? rgbm_lambda : ordinary_lambda};
}
#ifdef RGBM_RUNTIME_TESTING
void test_ready() { transition(State::Ready); }
unsigned occupied_slots() {
    AcquireSRWLockShared(&slot_lock);
    unsigned count = 0; for (const auto& slot : slots) count += slot.occupied;
    ReleaseSRWLockShared(&slot_lock); return count;
}
Callback test_acquire(Callback original, Context context, unsigned& index) {
    index = acquire(original, context); return index == slot_count ? nullptr : callbacks[index];
}
void test_release_after_join(unsigned index) { release(index); }
#endif
} // namespace rgbm::runtime
