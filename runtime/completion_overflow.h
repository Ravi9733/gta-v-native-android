#ifndef GTA_COMPLETION_OVERFLOW_H
#define GTA_COMPLETION_OVERFLOW_H
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

enum { GTA_RING_CAPACITY=512 };
typedef struct {
 uint32_t outstanding,waiting,producer,consumer;
 uint32_t sequence[GTA_RING_CAPACITY];
 uint64_t objects[GTA_RING_CAPACITY];
} GtaCompletionRing;
typedef struct GtaOverflowNode GtaOverflowNode;
typedef void *(*GtaOverflowAlloc)(void *,size_t);
typedef void (*GtaOverflowFree)(void *,void *);
typedef struct {
 pthread_mutex_t mutex;
 GtaCompletionRing *ring;
 GtaOverflowNode *head,*tail;
 size_t count,peak,capacity;
 uint64_t appended,overflow_popped,ring_popped,empty,allocation_failures,capacity_failures;
 GtaOverflowAlloc allocate;
 GtaOverflowFree deallocate;
 void *allocator_context;
 uint32_t last_consumer_handle;
} GtaCompletionOverflow;
enum {
 GTA_OVERFLOW_EMPTY=0,GTA_OVERFLOW_ITEM=1,GTA_OVERFLOW_OK=2,
 GTA_OVERFLOW_INVALID=-1,GTA_OVERFLOW_OOM=-2,GTA_OVERFLOW_CAPACITY=-3,
 GTA_OVERFLOW_GUEST_STATE=-5
};
int gta_overflow_init(GtaCompletionOverflow *,GtaCompletionRing *,size_t,
                     GtaOverflowAlloc,GtaOverflowFree,void *);
/* Called ONLY at the exact original completed-payload FULL path.
 * Never waits for guest queue space. Brief host mutex serialization is used;
 * this implementation is not lock-free. Failure transfers no ownership. */
int gta_overflow_append_completed(GtaCompletionOverflow *,uintptr_t);
/* Every guest consumer pop attempt must use this unified selector.
 * No outstanding decrement or game dispatch is performed here.
 * Handle is diagnostic only: serialized ownership may move between threads.
 * Original plain outstanding updates retain their existing external
 * serialization contract; this helper neither tightens nor repairs it. */
int gta_overflow_try_pop(GtaCompletionOverflow *,uint32_t,uintptr_t *);
/* Only after all callbacks stopped and original outstanding/ring/overflow empty. */
int gta_overflow_destroy(GtaCompletionOverflow *);
#endif
