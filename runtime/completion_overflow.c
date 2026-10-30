#include "completion_overflow.h"
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

struct GtaOverflowNode {
 struct GtaOverflowNode *next;
 uintptr_t payload;
 uint32_t fence;
};
_Static_assert(offsetof(GtaCompletionRing,sequence)==0x10,"guest sequence offset");
_Static_assert(offsetof(GtaCompletionRing,objects)==0x810,"guest object offset");
static void *default_alloc(void *context,size_t bytes){(void)context;return malloc(bytes);}
static void default_free(void *context,void *p){(void)context;free(p);}
static uint32_t ld32(const uint32_t *p){return __atomic_load_n(p,__ATOMIC_SEQ_CST);}

int gta_overflow_init(GtaCompletionOverflow *q,GtaCompletionRing *r,size_t cap,
                     GtaOverflowAlloc alloc,GtaOverflowFree dealloc,void *context) {
 if(!q||!r||!cap||cap>65536||((uintptr_t)r&7)||!!alloc!=!!dealloc)return GTA_OVERFLOW_INVALID;
 memset(q,0,sizeof *q);q->ring=r;q->capacity=cap;
 q->allocate=alloc?alloc:default_alloc;q->deallocate=dealloc?dealloc:default_free;
 q->allocator_context=context;
 return pthread_mutex_init(&q->mutex,NULL)?GTA_OVERFLOW_INVALID:GTA_OVERFLOW_OK;
}

int gta_overflow_append_completed(GtaCompletionOverflow *q,uintptr_t payload) {
 if(!q||!q->ring||!payload)return GTA_OVERFLOW_INVALID;
 /* Allocation is outside our mutex. No guest/HLE call occurs while it is held. */
 GtaOverflowNode *n=q->allocate(q->allocator_context,sizeof *n);
 if(!n){__atomic_fetch_add(&q->allocation_failures,1,__ATOMIC_RELAXED);return GTA_OVERFLOW_OOM;}
 n->payload=payload;n->next=NULL;
 pthread_mutex_lock(&q->mutex);
 if(q->count>=q->capacity) {
  q->capacity_failures++;pthread_mutex_unlock(&q->mutex);
  q->deallocate(q->allocator_context,n);return GTA_OVERFLOW_CAPACITY;
 }
 /* All already-reserved ring tickets precede this completed overflow item.
  * Append and unified pop share this lock; ring producers continue normally. */
 n->fence=ld32(&q->ring->producer);
 if(q->tail)q->tail->next=n;else q->head=n;
 q->tail=n;q->count++;if(q->count>q->peak)q->peak=q->count;q->appended++;
 pthread_mutex_unlock(&q->mutex);return GTA_OVERFLOW_OK;
}

int gta_overflow_try_pop(GtaCompletionOverflow *q,uint32_t owner,uintptr_t *out) {
 if(!q||!q->ring||!out)return GTA_OVERFLOW_INVALID;
 *out=0;GtaOverflowNode *retired=NULL;int result=GTA_OVERFLOW_EMPTY;
 pthread_mutex_lock(&q->mutex);
 q->last_consumer_handle=owner;
 GtaCompletionRing *r=q->ring;uint32_t pos=ld32(&r->consumer);
 if(q->head&&(int32_t)(pos-q->head->fence)>=0) {
  retired=q->head;q->head=retired->next;if(!q->head)q->tail=NULL;
  q->count--;q->overflow_popped++;*out=retired->payload;result=GTA_OVERFLOW_ITEM;
 } else {
  unsigned slot=pos&(GTA_RING_CAPACITY-1);
  uint32_t seq=__atomic_load_n(&r->sequence[slot],__ATOMIC_ACQUIRE);
  int32_t difference=(int32_t)(seq-(pos+1));
  if(!difference) {
   uintptr_t payload=(uintptr_t)__atomic_load_n(&r->objects[slot],__ATOMIC_ACQUIRE);
   if(!payload)result=GTA_OVERFLOW_GUEST_STATE;
   else {
    uint32_t expected=pos;
    /* All consumer paths are replaced; an unexpected competing mutation is
     * reported rather than silently consuming or duplicating an item. */
    if(!__atomic_compare_exchange_n(&r->consumer,&expected,pos+1,0,
                                   __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))result=GTA_OVERFLOW_GUEST_STATE;
    else {
     __atomic_store_n(&r->sequence[slot],pos+GTA_RING_CAPACITY,__ATOMIC_RELEASE);
     q->ring_popped++;*out=payload;result=GTA_OVERFLOW_ITEM;
    }
   }
  } else if(difference>0)result=GTA_OVERFLOW_GUEST_STATE;
  else q->empty++;
 }
 pthread_mutex_unlock(&q->mutex);
 /* Native node ownership ends here; the guest payload is not freed/retained. */
 if(retired)q->deallocate(q->allocator_context,retired);
 return result;
}

int gta_overflow_destroy(GtaCompletionOverflow *q) {
 if(!q||!q->ring)return GTA_OVERFLOW_INVALID;
 pthread_mutex_lock(&q->mutex);
 int busy=q->head||q->count||ld32(&q->ring->outstanding)||
          ld32(&q->ring->producer)!=ld32(&q->ring->consumer);
 pthread_mutex_unlock(&q->mutex);
 if(busy)return GTA_OVERFLOW_GUEST_STATE;
 pthread_mutex_destroy(&q->mutex);memset(q,0,sizeof *q);return GTA_OVERFLOW_OK;
}
