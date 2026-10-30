#import "gta_completion_failure.h"
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#endif
#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <mach/mach_time.h>
/* Startup prepares both event sources and the hidden UI: fault notification itself allocates no ObjC object/block.
   Report and UI have independent native queues, so a slow disk cannot delay the message. One report per process. */
static dispatch_source_t g_report_source,g_ui_source;
static atomic_flag g_noticed=ATOMIC_FLAG_INIT;
static int g_dirfd=-1;
static BOOL g_initialized,g_reported;
#if TARGET_OS_IPHONE
static __weak UIWindow *g_window;
static UIView *g_overlay;
#endif
static void show_failure(void *unused) {
    (void)unused;
#if TARGET_OS_IPHONE
    @try { if(g_overlay&&g_window) {
        if(g_overlay.superview!=g_window)[g_window addSubview:g_overlay];
        g_overlay.hidden=NO;[g_window bringSubviewToFront:g_overlay];
    } }
    @catch(__unused NSException *exception) {} /* UI failure cannot release the stopped guest */
#endif
}
static void report_failure(void *unused) {
    (void)unused;if(g_reported)return;g_reported=YES;
    if(g_dirfd<0)return;
    const char *final="last-failure-v1.json";struct stat old;
    if(fstatat(g_dirfd,final,&old,AT_SYMLINK_NOFOLLOW)==0) {
        if(!S_ISREG(old.st_mode)||old.st_nlink!=1||old.st_uid!=geteuid()||old.st_size>512)return;
    } else if(errno!=ENOENT)return;
    char data[256],temp[80];int code=runtime_gta_overflow_fault_code();
    int n=snprintf(data,sizeof data,"{\"schema\":1,\"kind\":\"completion_overflow_failure\",\"code\":%d,\"state\":\"halted\",\"relaunch_required\":true}\n",code);
    int tn=snprintf(temp,sizeof temp,".failure-%ld-%llu.tmp",(long)getpid(),(unsigned long long)mach_absolute_time());
    if(n<=0||(size_t)n>=sizeof data||tn<=0||(size_t)tn>=sizeof temp)return;
    int fd=openat(g_dirfd,temp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return;
    size_t done=0;for(unsigned attempt=0;done<(size_t)n&&attempt<8;attempt++) {
        ssize_t w=write(fd,data+done,(size_t)n-done);if(w<0&&errno==EINTR)continue;if(w<=0)break;done+=(size_t)w;
    }
    BOOL ok=done==(size_t)n&&fsync(fd)==0;if(close(fd)!=0)ok=NO;
    if(ok)ok=renameat(g_dirfd,temp,g_dirfd,final)==0;
    if(!ok)unlinkat(g_dirfd,temp,0);else fsync(g_dirfd); /* only this invocation's unpublished temp */
}
void runtime_gta_overflow_notice(int code) {
    if(!code||code!=runtime_gta_overflow_fault_code()||!g_report_source||!g_ui_source)return;
    if(atomic_flag_test_and_set_explicit(&g_noticed,memory_order_acq_rel))return;
    dispatch_source_merge_data(g_ui_source,1);dispatch_source_merge_data(g_report_source,1);
}
void gta_completion_failure_initialize(NSString *documents) {
    if(g_initialized)return;g_initialized=YES;
    @try {
        if([documents isKindOfClass:NSString.class]&&documents.isAbsolutePath) {
            int docs=open(documents.fileSystemRepresentation,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(docs>=0) {
                if(mkdirat(docs,"gta-completion-diagnostics",0700)==0||errno==EEXIST)
                    g_dirfd=openat(docs,"gta-completion-diagnostics",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
                close(docs);
            }
        }
    } @catch(__unused NSException *exception) {} /* report is best effort, UI remains available */
    dispatch_queue_t utility=dispatch_queue_create("gta.completion-failure-report",DISPATCH_QUEUE_SERIAL);
    if(!utility)return;dispatch_set_target_queue(utility,dispatch_get_global_queue(QOS_CLASS_UTILITY,0));
    g_report_source=dispatch_source_create(DISPATCH_SOURCE_TYPE_DATA_OR,0,0,utility);
    g_ui_source=dispatch_source_create(DISPATCH_SOURCE_TYPE_DATA_OR,0,0,dispatch_get_main_queue());
    if(!g_report_source||!g_ui_source)return;
    dispatch_source_set_event_handler_f(g_report_source,report_failure);dispatch_source_set_event_handler_f(g_ui_source,show_failure);
    dispatch_resume(g_report_source);dispatch_resume(g_ui_source);
    int code=runtime_gta_overflow_fault_code();if(code)runtime_gta_overflow_notice(code);
}
void gta_completion_failure_attach_window(UIWindow *window) {
#if TARGET_OS_IPHONE
    @try {
        if(!window)return;g_window=window;
        if(g_overlay){g_overlay.frame=window.bounds;[window addSubview:g_overlay];if(runtime_gta_overflow_fault_code())show_failure(NULL);return;}
        g_overlay=[[UIView alloc]initWithFrame:window.bounds];g_overlay.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
        g_overlay.backgroundColor=[UIColor colorWithWhite:0.03 alpha:1];g_overlay.accessibilityViewIsModal=YES;
        UILabel *label=[[UILabel alloc]initWithFrame:CGRectInset(g_overlay.bounds,24,24)];label.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
        label.text=@"Игра остановлена.\n\nНе удалось безопасно продолжить игру.\n\nПерезапустите GTA.";
        label.numberOfLines=0;label.textColor=UIColor.whiteColor;label.textAlignment=NSTextAlignmentCenter;label.font=[UIFont systemFontOfSize:22 weight:UIFontWeightSemibold];
        [g_overlay addSubview:label];g_overlay.hidden=YES;[window addSubview:g_overlay];
        if(runtime_gta_overflow_fault_code())show_failure(NULL);
    } @catch(__unused NSException *exception) {}
#else
    (void)window;
#endif
}
