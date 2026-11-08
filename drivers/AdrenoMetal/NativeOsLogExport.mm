// SPDX-License-Identifier: MIT
// System log export: the probe daemon (root, outside every sandbox profile) reads the unified log since boot and writes it as gzip text into
// a subdirectory of the render server's pipeline-cache directory. CacheExport.h ships that directory with the UFS log whenever it settles,
// so the host gets every process's os_log lines (PosterBoard, WidgetRenderer, wifid, ...) without a second export channel.
// Each pass rewrites the file through a temporary name; one daemon instance writes (the job runs under several labels).
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <stdarg.h>
#include <unistd.h>
#include <zlib.h>
#include <string>

extern char **environ;

@interface NSObject (UniXNUAutoLaunch)
+ (id)defaultWorkspace;
- (BOOL)openApplicationWithBundleID:(NSString *)bundleID;
+ (id)serviceWithDefaultShellEndpoint;
- (void)openApplication:(NSString *)bundleID withOptions:(id)options completion:(void (^)(id process, NSError *error))completion;
@end

static void klog_printf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n >= sizeof(buf)) n = (int)(sizeof(buf) - 1);
    int fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_CLOEXEC);
    if (fd >= 0) {
        (void)write(fd, buf, (size_t)n);
        close(fd);
    }
}

namespace {

constexpr const char *kDirectory = "/private/var/mobile/Library/Caches/com.apple.backboardd/m2v/zz-oslog";
constexpr const char *kLock = "/private/var/tmp/sheng-oslog.lock";
constexpr unsigned kPasses = 40, kPeriodSeconds = 45;
constexpr uint64_t kLineLimit = 600000;

const char *levelName(NSInteger level) {
    switch (level) {
    case 1: return "debug";
    case 2: return "info";
    case 3: return "notice";
    case 4: return "error";
    case 5: return "fault";
    default: return "-";
    }
}

// One pass: every log entry since the latest boot, "seconds pid process subsystem:category level message".
void writePass(unsigned pass) {
    @autoreleasepool {
        const std::string directory = kDirectory, temporary = directory + "/.oslog.txt.gz", final = directory + "/oslog.txt.gz";
        gzFile out = gzopen(temporary.c_str(), "wb6");
        if (!out) return;
        gzprintf(out, "# sheng oslog pass %u\n", pass);
        Class storeClass = NSClassFromString(@"OSLogStore");
        NSError *error = nil;
        // OSLogStoreSystem (0) is unavailable in the iOS SDK headers; the class answers it at run time with the logging entitlements.
        id store = storeClass ? ((id (*)(Class, SEL, NSInteger, NSError **))objc_msgSend)(storeClass, @selector(storeWithScope:error:), 0, &error)
                              : nil;
        if (!store) {
            gzprintf(out, "# no store: %s\n", (error ? error.description : @"OSLogStore missing").UTF8String);
        } else {
            id position = ((id (*)(id, SEL, NSTimeInterval))objc_msgSend)(store, @selector(positionWithTimeIntervalSinceLatestBoot:), 0);
            id entries = ((id (*)(id, SEL, NSUInteger, id, id, NSError **))objc_msgSend)(
                store, @selector(entriesEnumeratorWithOptions:position:predicate:error:), 0, position, nil, &error);
            if (!entries) gzprintf(out, "# no entries: %s\n", (error ? error.description : @"nil").UTF8String);
            NSDate *start = nil;
            uint64_t lines = 0;
            for (id entry in entries) {
                @autoreleasepool {
                    if (++lines > kLineLimit) {
                        gzprintf(out, "# line limit\n");
                        break;
                    }
                    NSDate *date = [entry valueForKey:@"date"];
                    if (!start) start = date;
                    const double at = [date timeIntervalSinceDate:start];
                    NSString *message = [entry valueForKey:@"composedMessage"];
                    const bool isLog = [entry respondsToSelector:@selector(level)];
                    NSString *process = [entry respondsToSelector:@selector(process)] ? [entry valueForKey:@"process"] : @"?";
                    NSString *subsystem = [entry respondsToSelector:@selector(subsystem)] ? [entry valueForKey:@"subsystem"] : @"";
                    NSString *category = [entry respondsToSelector:@selector(category)] ? [entry valueForKey:@"category"] : @"";
                    const int pid = [entry respondsToSelector:@selector(processIdentifier)] ? [[entry valueForKey:@"processIdentifier"] intValue] : 0;
                    const NSInteger level = isLog ? [[entry valueForKey:@"level"] integerValue] : 0;
                    gzprintf(out, "%9.3f %5d %s %s:%s %s %s\n", at, pid, process.UTF8String ?: "?", subsystem.UTF8String ?: "",
                             category.UTF8String ?: "", levelName(level), message.UTF8String ?: "");
                }
            }
            gzprintf(out, "# %llu entries\n", (unsigned long long)lines);
        }
        if (gzclose(out) == Z_OK) {
            chmod(temporary.c_str(), 0644);
            rename(temporary.c_str(), final.c_str());
        }
    }
}

static void autoInstallAndLaunchTestApp() {
    @autoreleasepool {
        static const char *const kAppInst = "/usr/libexec/unixnu-appinst";
        static const char *const kTestIpa = "/private/var/ngpu/appsync/autoinstall/UniXNUTest.ipa";
        int a1 = access(kAppInst, X_OK);
        int a2 = access(kTestIpa, R_OK);
        klog_printf("[adrenoprobed] autoinstall check: appinst=%d ipa=%d uid=%d\n", a1, a2, (int)getuid());
        if (a1 != 0 || a2 != 0) {
            return;
        }
        // Wait for SpringBoard and installd to settle at the desktop
        sleep(8);
        char *const argv[] = {
            const_cast<char *>(kAppInst),
            const_cast<char *>("org.unixnu.testapp"),
            const_cast<char *>(kTestIpa),
            nullptr,
        };
        int status = -1;
        for (unsigned instTry = 0; instTry < 3; ++instTry) {
            pid_t pid = -1;
            status = -1;
            int sp = posix_spawn(&pid, kAppInst, nullptr, nullptr, argv, environ);
            if (sp == 0 && pid > 0) {
                (void)waitpid(pid, &status, 0);
            }
            int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            int sigCode = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
            klog_printf("[adrenoprobed] autoinstall try=%u spawn=%d pid=%d raw=%d exit=%d sig=%d\n",
                        instTry + 1, sp, (int)pid, status, exitCode, sigCode);
            NSLog(@"[UniXNU AppSync] auto-install org.unixnu.testapp try=%u exit=%d", instTry + 1, exitCode);
            if (sp == 0 && WIFEXITED(status) && exitCode == 0) break;
            sleep(4);
        }
        // Verify installed bundle under /private/var/containers/Bundle/Application
        NSString *bundlesRoot = @"/private/var/containers/Bundle/Application";
        NSArray<NSString *> *uuids = [NSFileManager.defaultManager contentsOfDirectoryAtPath:bundlesRoot error:nil];
        for (NSString *uuid in uuids) {
            NSString *cand = [[bundlesRoot stringByAppendingPathComponent:uuid] stringByAppendingPathComponent:@"UniXNUTest.app"];
            if ([NSFileManager.defaultManager fileExistsAtPath:cand]) {
                klog_printf("[adrenoprobed] autoinstall verified bundle on disk: %s\n", cand.UTF8String);
            }
        }
        dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW);
        dlopen("/System/Library/PrivateFrameworks/FrontBoardServices.framework/FrontBoardServices", RTLD_NOW);
        Class cls = objc_getClass("LSApplicationWorkspace");
        Class fbsCls = objc_getClass("FBSOpenApplicationService");
        for (unsigned attempt = 0; attempt < 5; ++attempt) {
            BOOL launched = NO;
            if ([cls respondsToSelector:@selector(defaultWorkspace)]) {
                id ws = [cls defaultWorkspace];
                if ([ws respondsToSelector:@selector(openApplicationWithBundleID:)]) {
                    launched = [ws openApplicationWithBundleID:@"org.unixnu.testapp"];
                }
            }
            klog_printf("[adrenoprobed] autoinstall launch attempt=%u ls_launched=%d\n", attempt + 1, launched ? 1 : 0);
            if ([fbsCls respondsToSelector:@selector(serviceWithDefaultShellEndpoint)]) {
                id svc = [fbsCls serviceWithDefaultShellEndpoint];
                if ([svc respondsToSelector:@selector(openApplication:withOptions:completion:)]) {
                    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
                    __block int fbsOk = 0;
                    [svc openApplication:@"org.unixnu.testapp" withOptions:nil completion:^(id proc, NSError *err) {
                        fbsOk = (proc != nil && err == nil) ? 1 : 0;
                        klog_printf("[adrenoprobed] autoinstall FBSOpenApplicationService proc=%s err=%s\n",
                                    proc ? [proc description].UTF8String : "nil",
                                    err ? err.description.UTF8String : "nil");
                        dispatch_semaphore_signal(sem);
                    }];
                    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(5 * NSEC_PER_SEC)));
                    if (fbsOk) launched = YES;
                }
            }
            if (launched) break;
            sleep(3);
        }
    }
}

}  // namespace

// Runs the passes (blocks for kPasses * kPeriodSeconds); returns at once if another instance holds the lock or the cache directory is absent.
extern "C" void nativeOsLogExport() {
    const int lock = open(kLock, O_CREAT | O_RDWR, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) return;
    autoInstallAndLaunchTestApp();
    for (unsigned pass = 1; pass <= kPasses; ++pass) {
        mkdir(kDirectory, 0755);
        struct stat st = {};
        if (stat(kDirectory, &st) == 0 && S_ISDIR(st.st_mode)) writePass(pass);
        sleep(kPeriodSeconds);
    }
}
