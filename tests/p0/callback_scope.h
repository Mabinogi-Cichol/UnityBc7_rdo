#pragma once
// P0 prototype, NOT linked into the production plugin. No JobData fields read.
#include <array>
#include <mutex>
#include <utility>
#include <cstdint>

namespace p0 {
enum class Context { Unknown, Ordinary, RGBM };
using Callback = void (*)(void*, int, int);
inline thread_local Context tls_context = Context::Unknown;
inline Context current_context() { return tls_context; }
class ContextScope {
    Context previous_;
public:
    explicit ContextScope(Context value) : previous_(tls_context) { tls_context = value; }
    ~ContextScope() { tls_context = previous_; }
    ContextScope(const ContextScope&) = delete;
    ContextScope& operator=(const ContextScope&) = delete;
};
struct Slot {
    Callback original = nullptr;
    Context context = Context::Unknown;
    unsigned active = 0;
    bool occupied = false, quarantined = false;
};
constexpr size_t slot_count = 64;
inline std::array<Slot, slot_count> slots{};
inline std::mutex slots_mutex;
inline uint64_t rejected_callbacks = 0;
template<size_t Index> void invoke(void* job, int begin, int end) {
    Callback original;
    Context context;
    {
        std::lock_guard<std::mutex> lock(slots_mutex);
        auto& slot = slots[Index];
        original = slot.original;
        if (!slot.occupied || !original) { ++rejected_callbacks; return; }
        context = slot.quarantined ? Context::Unknown : slot.context;
        ++slot.active;
    }
    struct ActiveGuard {
        ~ActiveGuard() { std::lock_guard<std::mutex> lock(slots_mutex); --slots[Index].active; }
    } guard;
    ContextScope scope(context);
    original(job, begin, end);
}
template<size_t... Index> constexpr auto functions(std::index_sequence<Index...>) {
    return std::array<Callback, sizeof...(Index)>{&invoke<Index>...};
}
inline const auto callbacks = functions(std::make_index_sequence<slot_count>{});
class CallbackLease {
    size_t slot_ = slot_count;
public:
    explicit CallbackLease(Callback original, Context context = current_context()) {
        if (!original) return;
        std::lock_guard<std::mutex> lock(slots_mutex);
        for (size_t i = 0; i < slots.size(); ++i) if (!slots[i].occupied) {
            slots[i] = {original, context, 0, true, false}; slot_ = i; break;
        }
    }
    Callback callback() const { return slot_ == slot_count ? nullptr : callbacks[slot_]; }
    // Caller may invoke this ONLY after the synchronous scheduler has joined all
    // callbacks. active==0 alone cannot prove that no callback is still queued.
    bool complete_after_join() {
        if (slot_ == slot_count) return false;
        std::lock_guard<std::mutex> lock(slots_mutex);
        if (slots[slot_].active) return false;
        slots[slot_] = {}; slot_ = slot_count; return true;
    }
    ~CallbackLease() {
        if (slot_ == slot_count) return;
        // Abnormal exit never recycles this function identity. A pending callback
        // can still forward its original, but must receive Unknown classification.
        std::lock_guard<std::mutex> lock(slots_mutex);
        slots[slot_].quarantined = true;
        slots[slot_].context = Context::Unknown;
    }
    CallbackLease(const CallbackLease&) = delete;
    CallbackLease& operator=(const CallbackLease&) = delete;
};
inline size_t occupied_slots() {
    std::lock_guard<std::mutex> lock(slots_mutex);
    size_t count = 0; for (const auto& slot : slots) if (slot.occupied) ++count; return count;
}
} // namespace p0
