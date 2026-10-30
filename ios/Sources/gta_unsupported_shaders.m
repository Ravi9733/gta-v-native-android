#import "gta_unsupported_shaders.h"
#import <CommonCrypto/CommonDigest.h>
#include <dispatch/dispatch.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <math.h>
#include <stdint.h>

/* Persistent discovery only: these programs still require renderer support. The
   helper never compiles, changes a shader cache, or acknowledges a report itself.
   New identities are capped; repeated draw/program serials do not schedule I/O.
   Disk I/O runs on one utility queue, without any renderer lock held. */
enum { kProgramCap = 128, kFileCap = 256 * 1024 };
static NSObject *g_guard;
static dispatch_queue_t g_queue;
static NSMutableDictionary<NSString *, NSDictionary *> *g_programs;
static NSString *g_ack;
static NSDictionary *g_startup;
static int g_dirfd = -1;
static BOOL g_initialized, g_scheduled, g_loggedFailure, g_loggedCap;
static uint64_t g_revision;
static const char *kFile = "unsupported-programs-v1.json";

static void setup(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        g_guard = [NSObject new];
        g_queue = dispatch_queue_create("gta.unsupported-shader-report", DISPATCH_QUEUE_SERIAL);
        dispatch_set_target_queue(g_queue, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
        g_programs = [NSMutableDictionary dictionary];
        g_ack = @"";
    });
}
static void failed(const char *reason) {
    @synchronized (g_guard) {
        if (!g_loggedFailure) {
            g_loggedFailure = YES;
            fprintf(stderr, "[shader-unsupported] persistence unavailable: %s; no per-draw retry\n", reason);
        }
    }
}
static BOOL hex64(id value) {
    if (![value isKindOfClass:NSString.class] || [value length] != 64) return NO;
    for (NSUInteger i = 0; i < 64; ++i) {
        unichar c = [value characterAtIndex:i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return NO;
    }
    return YES;
}
static BOOL uintNumber(id value, uint64_t max, uint64_t *out) {
    if (![value isKindOfClass:NSNumber.class] || CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID()) return NO;
    double n = [value doubleValue];
    if (!isfinite(n) || n < 0 || n > (double)max || floor(n) != n) return NO;
    uint64_t u = [value unsignedLongLongValue];
    if (u > max) return NO;
    if (out) *out = u;
    return YES;
}
static NSString *digest(NSString *text) {
    NSData *data = [text dataUsingEncoding:NSUTF8StringEncoding];
    unsigned char hash[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.bytes, (CC_LONG)data.length, hash);
    char result[65];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i) snprintf(result + i * 2, 3, "%02x", hash[i]);
    return @(result);
}
static BOOL serialText(id value) {
    if (![value isKindOfClass:NSString.class] || ![value length] || [value length] > 20) return NO;
    for (NSUInteger i = 0; i < [value length]; ++i) {
        unichar c = [value characterAtIndex:i]; if (c < '0' || c > '9') return NO;
    }
    return [value length] < 20 || [value compare:@"18446744073709551615" options:NSLiteralSearch] != NSOrderedDescending;
}
static NSDictionary *normalize(id snapshot) {
    if (![snapshot isKindOfClass:NSDictionary.class]) return nil;
    id input = snapshot[@"stages"];
    if (![input isKindOfClass:NSArray.class] || ![input count] || [input count] > 6) return nil;
    NSMutableArray *stages = [NSMutableArray array];
    unsigned seen = 0; BOOL unsupported = NO;
    for (id entry in input) {
        uint64_t stage;
        if (![entry isKindOfClass:NSDictionary.class] || !uintNumber(entry[@"stage"], 5, &stage) || !hex64(entry[@"base"])) return nil;
        if (seen & (1u << stage)) return nil;
        seen |= 1u << stage; unsupported |= stage >= 2 && stage <= 4;
        [stages addObject:@{@"stage": @(stage), @"base": [entry[@"base"] copy]}];
    }
    if (!unsupported) return nil;
    [stages sortUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) { return [a[@"stage"] compare:b[@"stage"]]; }];
    NSMutableString *identity = [NSMutableString string];
    for (NSDictionary *entry in stages) [identity appendFormat:@"%u:%@;", [entry[@"stage"] unsignedIntValue], entry[@"base"]];
    NSMutableDictionary *result = [@{@"id": digest(identity), @"stages": [stages copy]} mutableCopy];
    /* Serial is diagnostic context only and never part of deduplication. */
    uint64_t serial;
    if (uintNumber(snapshot[@"serial"], UINT64_MAX, &serial)) result[@"serial"] = [NSString stringWithFormat:@"%llu", (unsigned long long)serial];
    else if (serialText(snapshot[@"serial"])) result[@"serial"] = [snapshot[@"serial"] copy];
    return result;
}
static NSString *batchFor(NSDictionary *programs) {
    return digest([[[programs allKeys] sortedArrayUsingSelector:@selector(compare:)] componentsJoinedByString:@"\n"]);
}
static NSDictionary *stateLocked(void) {
    NSMutableArray *records = [NSMutableArray arrayWithCapacity:g_programs.count];
    for (NSString *key in [[g_programs allKeys] sortedArrayUsingSelector:@selector(compare:)]) [records addObject:g_programs[key]];
    return @{@"schema": @1, @"programs": records, @"acknowledged_batch": g_ack};
}
static BOOL atomicWrite(NSData *data) {
    if (g_dirfd < 0 || !data || data.length > kFileCap) return NO;
    struct stat previous;
    if (fstatat(g_dirfd, kFile, &previous, AT_SYMLINK_NOFOLLOW) == 0 && !S_ISREG(previous.st_mode)) return NO;
    NSString *name = [NSString stringWithFormat:@".unsupported-%@.tmp", NSUUID.UUID.UUIDString];
    int fd = openat(g_dirfd, name.UTF8String, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) return NO;
    size_t done = 0;
    while (done < data.length) {
        ssize_t n = write(fd, (const char *)data.bytes + done, data.length - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += (size_t)n;
    }
    BOOL ok = done == data.length && fsync(fd) == 0;
    if (close(fd) != 0) ok = NO;
    if (ok) ok = renameat(g_dirfd, name.UTF8String, g_dirfd, kFile) == 0;
    if (!ok) unlinkat(g_dirfd, name.UTF8String, 0); // only our unpublished unique temp
    if (ok) fsync(g_dirfd);
    return ok;
}
static void scheduleLocked(void) {
    if (g_scheduled || g_dirfd < 0) return;
    g_scheduled = YES;
    dispatch_async(g_queue, ^{ @autoreleasepool {
        @try {
            for (;;) {
                NSDictionary *state; uint64_t revision;
                @synchronized (g_guard) { state = stateLocked(); revision = g_revision; }
                NSError *error = nil;
                NSData *data = [NSJSONSerialization dataWithJSONObject:state options:NSJSONWritingSortedKeys error:&error];
                BOOL ok = !error && atomicWrite(data);
                @synchronized (g_guard) {
                    if (!ok) { g_scheduled = NO; failed("atomic report write failed"); return; }
                    fprintf(stderr, "[shader-unsupported] persisted programs=%lu\n", (unsigned long)[state[@"programs"] count]);
                    if (revision == g_revision) { g_scheduled = NO; return; }
                }
            }
        } @catch (__unused NSException *exception) {
            @synchronized (g_guard) { g_scheduled = NO; }
            failed("report exception contained");
        }
    }});
}
static BOOL loadExisting(void) {
    int fd = openat(g_dirfd, kFile, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT;
    struct stat info; BOOL valid = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0 && info.st_size <= kFileCap;
    NSMutableData *data = valid ? [NSMutableData dataWithLength:(NSUInteger)info.st_size] : nil;
    size_t done = 0;
    while (valid && done < data.length) {
        ssize_t n = read(fd, (char *)data.mutableBytes + done, data.length - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { valid = NO; break; }
        done += (size_t)n;
    }
    char extra; if (valid && read(fd, &extra, 1) != 0) valid = NO;
    close(fd);
    id state = valid ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    uint64_t schema = 0;
    valid = [state isKindOfClass:NSDictionary.class] && uintNumber(state[@"schema"], 1, &schema) && schema == 1 && [state[@"programs"] isKindOfClass:NSArray.class] && [state[@"programs"] count] <= kProgramCap;
    NSString *ack = valid ? state[@"acknowledged_batch"] : nil;
    valid = valid && [ack isKindOfClass:NSString.class] && (!ack.length || hex64(ack));
    NSMutableDictionary *loaded = [NSMutableDictionary dictionary];
    if (valid) for (id record in state[@"programs"]) {
        NSDictionary *normalized = normalize(record);
        if (!normalized || ![normalized[@"id"] isEqual:record[@"id"]] || loaded[normalized[@"id"]]) { valid = NO; break; }
        /* Preserve only validated context; no arbitrary fields from an old file. */
        loaded[normalized[@"id"]] = normalized;
    }
    if (valid) { g_programs = loaded; g_ack = ack; return YES; }
    struct stat saved;
    if (fstatat(g_dirfd, kFile, &saved, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(saved.st_mode)) return NO;
    NSString *archive = [NSString stringWithFormat:@"unsupported-programs-invalid-%@.json", NSUUID.UUID.UUIDString];
    if (renameat(g_dirfd, kFile, g_dirfd, archive.UTF8String) != 0) return NO;
    fprintf(stderr, "[shader-unsupported] invalid previous report preserved as %s\n", archive.UTF8String);
    return YES;
}

void gta_unsupported_shaders_initialize(NSString *documentsDirectory) {
    setup();
    @try { @synchronized (g_guard) {
        if (g_initialized) return;
        g_initialized = YES;
        if (![documentsDirectory isKindOfClass:NSString.class] || !documentsDirectory.isAbsolutePath) { failed("invalid Documents path"); return; }
        int docs = open(documentsDirectory.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        if (docs < 0) { failed("cannot open Documents directory"); return; }
        if (mkdirat(docs, "gta-shader-diagnostics", 0700) != 0 && errno != EEXIST) { close(docs); failed("cannot create diagnostics directory"); return; }
        g_dirfd = openat(docs, "gta-shader-diagnostics", O_RDONLY | O_DIRECTORY | O_NOFOLLOW); close(docs);
        if (g_dirfd < 0 || !loadExisting()) {
            if (g_dirfd >= 0) close(g_dirfd); g_dirfd = -1;
            failed("previous diagnostics cannot be safely read"); return;
        }
        NSString *batch = batchFor(g_programs);
        if (g_programs.count && ![batch isEqualToString:g_ack]) {
            NSMutableSet *stages = [NSMutableSet set];
            for (NSDictionary *program in g_programs.allValues) for (NSDictionary *entry in program[@"stages"])
                if ([entry[@"stage"] intValue] >= 2 && [entry[@"stage"] intValue] <= 4) [stages addObject:entry[@"stage"]];
            g_startup = @{@"batch": batch, @"program_count": @(g_programs.count), @"stages": [[stages allObjects] sortedArrayUsingSelector:@selector(compare:)]};
        }
    }} @catch (__unused NSException *exception) { failed("initialization exception contained"); }
}
void runtime_shader_unsupported_program(NSDictionary *snapshot) {
    setup();
    @try {
        NSDictionary *program = normalize(snapshot);
        if (!program) return;
        @synchronized (g_guard) {
            if (!g_initialized || g_dirfd < 0 || g_programs[program[@"id"]]) return;
            if (g_programs.count >= kProgramCap) {
                if (!g_loggedCap) { g_loggedCap = YES; fprintf(stderr, "[shader-unsupported] report cap reached: %u programs\n", kProgramCap); }
                return;
            }
            g_programs[program[@"id"]] = program; ++g_revision; scheduleLocked();
        }
    } @catch (__unused NSException *exception) { failed("program snapshot exception contained"); }
}
NSDictionary *gta_unsupported_shaders_startup_notice(void) {
    setup(); @synchronized (g_guard) { return g_startup; }
}
void gta_unsupported_shaders_acknowledge(NSString *batch) {
    setup();
    @try { @synchronized (g_guard) {
        if (!hex64(batch) || ![g_startup[@"batch"] isEqual:batch] || [g_ack isEqual:batch]) return;
        g_ack = [batch copy]; g_startup = nil; ++g_revision; scheduleLocked();
    }} @catch (__unused NSException *exception) { failed("acknowledgement exception contained"); }
}

#if defined(GTA_UNSUPPORTED_TESTING)
/* Test-only barrier. No environment/test paths or reset API exist in shipping code. */
void gta_unsupported_test_flush(void) { setup(); dispatch_sync(g_queue, ^{}); }
#endif
