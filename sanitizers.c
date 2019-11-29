#include "sanitizers.h"

#include <ctype.h>
#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "cmdline.h"
#include "libhfcommon/common.h"
#include "libhfcommon/files.h"
#include "libhfcommon/log.h"
#include "libhfcommon/util.h"

/*
 * All clang sanitizers, except ASan, can be activated for target binaries
 * with or without the matching runtime library (libcompiler_rt). If runtime
 * libraries are included in target fuzzing environment, we can benefit from the
 * various Die() callbacks and abort/exit logic manipulation. However, some
 * setups (e.g. Android production ARM/ARM64 devices) enable sanitizers, such as
 * UBSan, without the runtime libraries. As such, their default ftrap is activated
 * which is for most cases a SIGABRT. For these cases end-user needs to enable
 * SIGABRT monitoring flag, otherwise these crashes will be missed.
 *
 * Normally SIGABRT is not a wanted signal to monitor for Android, since it produces
 * lots of useless crashes due to way Android process termination hacks work. As
 * a result the sanitizer's 'abort_on_error' flag cannot be utilized since it
 * invokes abort() internally. In order to not lose crashes a custom exitcode can
 * be registered and monitored. Since exitcode is a global flag, it's assumed
 * that target is compiled with only one sanitizer type enabled at a time.
 *
 * For cases where clang runtime library linking is not an option, SIGABRT should
 * be monitored even for noisy targets, such as the Android OS, since no viable
 * alternative exists.
 *
 * There might be cases where ASan instrumented targets crash while generating
 * reports for detected errors (inside __asan_report_error() proc). Under such
 * scenarios target fails to exit or SIGABRT (AsanDie() proc) as defined in
 * ASAN_OPTIONS flags, leaving garbage logs. An attempt is made to parse such
 * logs for cases where enough data are written to identify potentially missed
 * crashes. If ASan internal error results into a SIGSEGV being raised, it
 * will get caught from ptrace API, handling the discovered ASan internal crash.
 */

/* 'log_path' output directory for sanitizer reports */
#define kSANLOGDIR "log_path="

/* Raise SIGABRT on error or continue with exitcode logic */
#define kABORT_ENABLED "abort_on_error=1"
#define kABORT_DISABLED "abort_on_error=0"

/*
 * Common sanitizer flags
 *
 * symbolize: Disable symbolication since it changes logs (which are parsed) format
 */
#define kSAN_COMMON                \
    "symbolize=1:"                 \
    "detect_odr_violation=0:"      \
    "detect_leaks=0:"              \
    "allocator_may_return_null=1:" \
    "allow_user_segv_handler=0:"   \
    "exitcode=" HF_XSTR(HF_SAN_EXIT_CODE)

/* --{ ASan }-- */
/*
 * Sanitizer specific flags (notice that if enabled 'abort_on_error' has priority
 * over exitcode')
 */
#define kASAN_OPTS kSAN_COMMON

/* --{ UBSan }-- */
#define kUBSAN_OPTS kSAN_COMMON

/* --{ MSan }-- */
#define kMSAN_OPTS kSAN_COMMON ":wrap_signals=0:print_stats=1"

/* --{ LSan }-- */
#define kLSAN_OPTS kSAN_COMMON

/* If no sanitzer support was requested, simply make it use abort() on errors */
#define kSAN_REGULAR                                                 \
    "abort_on_error=1:handle_segv=0:handle_sigbus=0:handle_abort=0:" \
    "handle_sigill=0:handle_sigfpe=0:allocator_may_return_null=1:"   \
    "symbolize=0:detect_leaks=0:disable_coredump=0:"                 \
    "detect_odr_violation=0"

static void sanitizers_AddFlag(honggfuzz_t* hfuzz, const char* env, const char* val) {
    const char* abortFlag = hfuzz->cfg.monitorSIGABRT ? kABORT_ENABLED : kABORT_DISABLED;
    if (getenv(env)) {
        LOG_W("The '%s' envar is already set. Not overriding it!", env);
        return;
    }

    char buf[4096] = {};
    if (!hfuzz->sanitizer.enable) {
        snprintf(buf, sizeof(buf), "%s=%s", env, kSAN_REGULAR);
    } else {
        snprintf(buf, sizeof(buf), "%s=%s:%s:%s%s/%s", env, val, abortFlag, kSANLOGDIR,
            hfuzz->io.workDir, kLOGPREFIX);
    }
    /*
     * It will make ASAN to start background thread to check RSS mem use, which
     * will prevent the NetDrvier from using unshare(CLONE_NEWNET), which cannot
     * be used in multi-threaded contexts
     */
    if (!hfuzz->exe.netDriver && hfuzz->exe.rssLimit) {
        util_ssnprintf(buf, sizeof(buf), ":soft_rss_limit_mb=%" PRId64, hfuzz->exe.rssLimit);
    }

    cmdlineAddEnv(hfuzz, buf);
    LOG_D("%s", buf);
}

bool sanitizers_Init(honggfuzz_t* hfuzz) {
    sanitizers_AddFlag(hfuzz, "ASAN_OPTIONS", kASAN_OPTS);
    sanitizers_AddFlag(hfuzz, "UBSAN_OPTIONS", kUBSAN_OPTS);
    sanitizers_AddFlag(hfuzz, "MSAN_OPTIONS", kMSAN_OPTS);
    sanitizers_AddFlag(hfuzz, "LSAN_OPTIONS", kLSAN_OPTS);

    return true;
}
