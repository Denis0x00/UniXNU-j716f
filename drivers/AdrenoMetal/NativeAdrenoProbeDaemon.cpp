// SPDX-License-Identifier: MIT
// Adreno probe daemon: started by launchd from an owner job (a union view over /System/Library/LaunchDaemons, see the boot-time addon
// mount), so it runs outside every sandbox profile. It waits for the Adreno accelerator service to appear, runs the shared userspace GPU
// test once, then writes the system log for the UFS log export (NativeOsLogExport.mm) and exits. The test result reaches the kernel through
// the driver's progress markers and SetMetadata.
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include "NativeAdrenoUserTest.h"

extern "C" void nativeOsLogExport();
extern char **environ;

using MatchingFn = void *(*)(const char *);
using GetServiceFn = unsigned (*)(unsigned, void *);

static void *wifiSupplicantWatchdog(void *) {
    static const char *const kSupplicant = "/usr/libexec/unixnu-wpa_supplicant";
    static const char *const kConfig = "/private/var/ngpu/etc/wifi/wpa_supplicant.conf";
    if (access(kSupplicant, X_OK) != 0 || access(kConfig, R_OK) != 0) {
        return nullptr;
    }
    char *const argv[] = {
        const_cast<char *>(kSupplicant),
        const_cast<char *>("-Dsheng"),
        const_cast<char *>("-ien0"),
        const_cast<char *>("-c/private/var/ngpu/etc/wifi/wpa_supplicant.conf"),
        const_cast<char *>("-q"),
        nullptr,
    };
    for (;;) {
        pid_t pid = -1;
        if (posix_spawn(&pid, kSupplicant, nullptr, nullptr, argv, environ) == 0 && pid > 0) {
            int st = 0;
            (void)waitpid(pid, &st, 0);
        }
        sleep(3);
    }
    return nullptr;
}

static void *osLogExportWorker(void *) {
    nativeOsLogExport();
    return nullptr;
}

int main(int argc, char **argv) {
    if (argc > 0 && argv && argv[0] && strstr(argv[0], "mtmergeprops") != nullptr) {
        return 0;
    }
    pthread_t wifiThread{};
    const bool haveWifiThread = (pthread_create(&wifiThread, nullptr, wifiSupplicantWatchdog, nullptr) == 0);
    pthread_t exportThread{};
    const bool haveExportThread = (pthread_create(&exportThread, nullptr, osLogExportWorker, nullptr) == 0);
    void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW | RTLD_LOCAL);
    if (!iokit) return 1;
    auto matching = (MatchingFn)dlsym(iokit, "IOServiceMatching");
    auto getService = (GetServiceFn)dlsym(iokit, "IOServiceGetMatchingService");
    if (!matching || !getService) return 2;
    // The service is published a few seconds into boot (after the GPU is powered); poll for up to two minutes.
    unsigned port = 0;
    for (unsigned i = 0; i < 240 && !port; ++i) {
        port = getService(0, matching("IOAcceleratorES"));
        if (!port) usleep(500000);
    }
    const int status = !port ? 3 : adrenoUserTest(port, "adrenoprobed") >= 0 ? 0 : 4;
    if (!haveExportThread) {
        nativeOsLogExport();
    } else {
        (void)pthread_join(exportThread, nullptr);
    }
    if (haveWifiThread) {
        (void)pthread_join(wifiThread, nullptr);
    }
    return status;
}
