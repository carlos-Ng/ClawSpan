#pragma once

#include <cstring>
#include <type_traits>

namespace clawspan {

// Safe unaligned load/store helpers.
// - Always uses memcpy to avoid UB and alignment traps (e.g. SIGBUS on arm64).
// - T must be trivially copyable (POD-like).
template <class T>
inline T loadUnaligned(const void* p) {
    static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");
    T v{};
    if (p == nullptr) {
        return v;
    }
    std::memcpy(&v, p, sizeof(T));
    return v;
}

template <class T>
inline void storeUnaligned(void* p, T v) {
    static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");
    if (p == nullptr) {
        return;
    }
    std::memcpy(p, &v, sizeof(T));
}

} // namespace clawspan

