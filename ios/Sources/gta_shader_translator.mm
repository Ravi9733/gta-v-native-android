#import "gta_shader_translator.h"

#if TARGET_OS_IPHONE || defined(GTA_SHADER_TRANSLATOR_HOST_TEST)
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>

// Matches the native bridge ABI; its allocator never crosses into NativeAOT.
extern "C" int gta_spirv_cross(const uint32_t *, size_t, int, char **, char **);
extern "C" void gta_spirv_cross_free(void *);
typedef int (*CrossFn)(const uint32_t *, size_t, int, char **, char **);
typedef void (*CrossFreeFn)(void *);
typedef int (*SetBackendFn)(CrossFn, CrossFreeFn);
typedef int (*TranslateFn)(const uint8_t *, size_t, const uint8_t *, size_t, int, uint8_t **, size_t *);
typedef void (*FreeFn)(void *);

static NSString *const Chain = @"totk-native-v4-unverified";
static const NSUInteger InputLimit = 1u << 20, OutputLimit = 16u << 20;
static TranslateFn Translate;
static FreeFn FreeOutput;
static int Available;
static void *Framework; // NativeAOT libraries must never be dlclosed.

static int Cross(const uint32_t *words, size_t count, int mode, char **out, char **error) {
    return gta_spirv_cross(words, count, mode, out, error);
}
static void CrossFree(void *memory) { gta_spirv_cross_free(memory); }
static NSDictionary *Failure(NSString *reason) {
    return @{ @"ok": @NO, @"error": reason ?: @"ios_native_translator_failed" };
}
static BOOL String(id value) { return [value isKindOfClass:NSString.class]; }
static BOOL Matches(NSString *value, NSString *pattern) {
    if (!String(value)) return NO;
    NSRegularExpression *re = [NSRegularExpression regularExpressionWithPattern:pattern options:0 error:NULL];
    return [re numberOfMatchesInString:value options:0 range:NSMakeRange(0, value.length)] == 1;
}

int gta_ios_shader_available(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSString *path;
#ifdef GTA_SHADER_TRANSLATOR_HOST_TEST
        // Only the separately compiled private host harness defines this.
        extern NSString *gta_ios_shader_test_framework_path(void);
        path = gta_ios_shader_test_framework_path();
#else
        path = [NSBundle.mainBundle.privateFrameworksPath stringByAppendingPathComponent:@"GtaShaderAot.framework/GtaShaderAot"];
#endif
        if (!path.length) { fprintf(stderr, "[gta-ios-shader] unavailable: framework path missing\n"); return; }
        Framework = dlopen(path.fileSystemRepresentation, RTLD_NOW | RTLD_LOCAL);
        if (!Framework) { fprintf(stderr, "[gta-ios-shader] unavailable: framework load failed: %s\n", dlerror() ?: "unknown"); return; }
        SetBackendFn setBackend = (SetBackendFn)dlsym(Framework, "gta_shader_set_cross_backend");
        TranslateFn translate = (TranslateFn)dlsym(Framework, "gta_shader_translate_json");
        FreeFn release = (FreeFn)dlsym(Framework, "gta_shader_free");
        if (!setBackend || !translate || !release) { fprintf(stderr, "[gta-ios-shader] unavailable: framework exports missing\n"); return; }
        if (setBackend(Cross, CrossFree) != 0) { fprintf(stderr, "[gta-ios-shader] unavailable: backend registration failed\n"); return; }
        Translate = translate; FreeOutput = release; Available = 1;
        fprintf(stderr, "[gta-ios-shader] ready: in-process NativeAOT + SPIRV-Cross, chain=totk-native-v4-unverified\n");
    });
    return Available;
}

static BOOL SameFile(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
           a->st_mtimespec.tv_sec == b->st_mtimespec.tv_sec && a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec;
}
static NSData *ReadCode(NSString *path) {
    struct stat before, opened, after;
    if (lstat(path.fileSystemRepresentation, &before) || !S_ISREG(before.st_mode) || before.st_size < 1 || (uint64_t)before.st_size > InputLimit) return nil;
    int fd = open(path.fileSystemRepresentation, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return nil;
    BOOL ok = !fstat(fd, &opened) && SameFile(&before, &opened);
    NSMutableData *data = ok ? [NSMutableData dataWithLength:(NSUInteger)opened.st_size] : nil;
    size_t at = 0;
    while (ok && at < data.length) {
        ssize_t n = read(fd, (uint8_t *)data.mutableBytes + at, data.length - at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = NO; break; }
        at += (size_t)n;
    }
    if (ok) { uint8_t extra; ssize_t n; do { n = read(fd, &extra, 1); } while (n < 0 && errno == EINTR); ok = n == 0; }
    if (ok) ok = !fstat(fd, &after) && SameFile(&opened, &after);
    if (close(fd)) ok = NO;
    return ok ? data : nil;
}
static BOOL WritableTarget(NSString *path) {
    struct stat st;
    if (!lstat(path.fileSystemRepresentation, &st)) return S_ISREG(st.st_mode);
    return errno == ENOENT;
}

NSDictionary *gta_ios_shader_translate(NSDictionary *request, NSString *cacheDir) {
    @try {
        if (![request isKindOfClass:NSDictionary.class] || !String(cacheDir) || !cacheDir.isAbsolutePath ||
            ![cacheDir isEqual:cacheDir.stringByStandardizingPath]) return Failure(@"ios_native_invalid_request_or_cache_directory");
        NSString *variant = request[@"variant"], *stage = request[@"stage"], *codePath = request[@"code"];
        if (!Matches(variant, @"\\A[0-9a-f]{64}-[0-9a-f]{12}\\z") || ![@[@"vs", @"ps", @"cs"] containsObject:stage]) return Failure(@"ios_native_invalid_variant_or_stage");
        NSString *expected = [cacheDir stringByAppendingPathComponent:[variant stringByAppendingString:@".code"]];
        if (!String(codePath) || ![codePath isEqual:expected]) return Failure(@"ios_native_code_path_outside_exact_cache_variant");
        struct stat dirStat;
        if (lstat(cacheDir.fileSystemRepresentation, &dirStat) || !S_ISDIR(dirStat.st_mode)) return Failure(@"ios_native_cache_directory_missing_or_symlink");
        NSData *code = ReadCode(expected);
        if (!code) return Failure(@"ios_native_code_not_stable_regular_file_or_exceeds_1MiB");
        NSError *error = nil;
        NSData *input = [NSJSONSerialization dataWithJSONObject:request options:0 error:&error];
        if (!input || input.length < 1 || input.length > InputLimit) return Failure(@"ios_native_request_JSON_invalid_or_exceeds_1MiB");
        if (!gta_ios_shader_available()) return Failure(@"ios_native_translator_unavailable");
        uint8_t *owned = NULL; size_t outputLength = 0;
        @try {
            int status = Translate((const uint8_t *)input.bytes, input.length, (const uint8_t *)code.bytes, code.length, 1, &owned, &outputLength);
            if (status < 0 || !owned || !outputLength || outputLength > OutputLimit) return Failure(@"ios_native_translator_invalid_ABI_result");
            NSData *raw = [NSData dataWithBytes:owned length:outputLength];
            id reply = [NSJSONSerialization JSONObjectWithData:raw options:0 error:&error];
            if (![reply isKindOfClass:NSDictionary.class]) return Failure(@"ios_native_translator_invalid_result_JSON");
            if (status != 0 || ![reply[@"ok"] isKindOfClass:NSNumber.class] || ![reply[@"ok"] boolValue]) {
                NSString *detail = String(reply[@"error"]) ? reply[@"error"] : @"ios_native_translation_failed";
                return Failure(detail.length > 1024 ? [detail substringToIndex:1024] : detail);
            }
            NSString *msl = reply[@"msl"], *reflectionText = reply[@"reflection_json"];
            if (![reply[@"chain"] isEqual:Chain] || ![reply[@"variant"] isEqual:variant] || !String(msl) || !msl.length || !String(reflectionText) || !reflectionText.length) return Failure(@"ios_native_result_identity_or_payload_mismatch");
            NSData *reflectionData = [reflectionText dataUsingEncoding:NSUTF8StringEncoding];
            id reflection = reflectionData ? [NSJSONSerialization JSONObjectWithData:reflectionData options:0 error:&error] : nil;
            NSString *entryKind = [stage isEqual:@"vs"] ? @"vertex" : [stage isEqual:@"ps"] ? @"fragment" : @"kernel";
            if (![reflection isKindOfClass:NSDictionary.class] || ![reflection[@"chain"] isEqual:Chain] ||
                ![reflection[@"key"] isEqual:[variant stringByAppendingString:@"_b"]] || ![reflection[@"route"] isEqual:@"B"] ||
                ![reflection[@"stage"] isEqual:stage] || ![reflection[@"msl_entry_kind"] isEqual:entryKind] ||
                !Matches(reflection[@"entry"], @"\\A[A-Za-z_][A-Za-z0-9_]*\\z") ||
                ![reflection[@"bindings"] isKindOfClass:NSArray.class] || ![reflection[@"deps"] isKindOfClass:NSDictionary.class]) return Failure(@"ios_native_reflection_identity_or_stage_mismatch");
            NSData *metalData = [msl dataUsingEncoding:NSUTF8StringEncoding];
            NSString *metalPath = [cacheDir stringByAppendingPathComponent:[variant stringByAppendingString:@"_b.metal"]];
            NSString *jsonPath = [cacheDir stringByAppendingPathComponent:[variant stringByAppendingString:@"_b.json"]];
            if (!metalData || !WritableTarget(metalPath) || !WritableTarget(jsonPath)) return Failure(@"ios_native_output_target_not_regular_or_absent");
            // Runtime's existing reflection/dependency validators remain authoritative.
            // JSON is the final publication step; the hook updates its index after OK.
            if (![metalData writeToFile:metalPath options:NSDataWritingAtomic error:&error]) return Failure(@"ios_native_atomic_metal_write_failed");
            if (![reflectionData writeToFile:jsonPath options:NSDataWritingAtomic error:&error]) return Failure(@"ios_native_atomic_reflection_write_failed");
            return @{ @"ok": @YES };
        } @finally {
            if (owned) FreeOutput(owned);
        }
    } @catch (NSException *exception) {
        (void)exception;
        return Failure(@"ios_native_translator_objective_c_exception");
    }
}
#endif
