#pragma once

#include <IOKit/IOLocks.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline IOLock *sm8250_apps_smmu_shared_lock(void) {
    static IOLock *s_lock = 0;
    if (!__atomic_load_n(&s_lock, __ATOMIC_ACQUIRE)) {
        IOLock *created = IOLockAlloc();
        IOLock *expected = 0;
        if (!__atomic_compare_exchange_n(&s_lock, &expected, created, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            if (created)
                IOLockFree(created);
        }
    }
    return __atomic_load_n(&s_lock, __ATOMIC_ACQUIRE);
}

static inline void sm8250_apps_smmu_lock(void) {
    IOLock *lock = sm8250_apps_smmu_shared_lock();
    if (lock)
        IOLockLock(lock);
}

static inline void sm8250_apps_smmu_unlock(void) {
    IOLock *lock = sm8250_apps_smmu_shared_lock();
    if (lock)
        IOLockUnlock(lock);
}

#ifdef __cplusplus
}

struct AppsSmmuLock {
    IOLock *lock;
    AppsSmmuLock() : lock(sm8250_apps_smmu_shared_lock()) {
        if (lock)
            IOLockLock(lock);
    }
    ~AppsSmmuLock() {
        if (lock)
            IOLockUnlock(lock);
    }
    bool ready() const { return lock != nullptr; }
    AppsSmmuLock(const AppsSmmuLock &) = delete;
    AppsSmmuLock &operator=(const AppsSmmuLock &) = delete;
};
#endif
