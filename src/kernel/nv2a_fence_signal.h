#ifndef NV2A_FENCE_SIGNAL_H
#define NV2A_FENCE_SIGNAL_H
/* Host-only notification; guest completion words remain authoritative. */
#if defined(_WIN32)
typedef struct { SRWLOCK lock; CONDITION_VARIABLE changed; unsigned sleepers; unsigned long waits,timeouts; } NvFenceSignal;
#define NV_FENCE_SIGNAL_INIT {SRWLOCK_INIT,CONDITION_VARIABLE_INIT,0,0,0}
static void nv_fence_signal_lock(NvFenceSignal *s){AcquireSRWLockExclusive(&s->lock);}
static void nv_fence_signal_unlock(NvFenceSignal *s){ReleaseSRWLockExclusive(&s->lock);}
static void nv_fence_signal_wake(NvFenceSignal *s){WakeAllConditionVariable(&s->changed);}
static int nv_fence_signal_sleep(NvFenceSignal *s,unsigned ms){
    ++s->sleepers;++s->waits;
    int result=SleepConditionVariableSRW(&s->changed,&s->lock,ms,0)?2:
        (GetLastError()==ERROR_TIMEOUT?1:0);
    --s->sleepers;if(result==1)++s->timeouts;return result;
}
#else
#include <pthread.h>
#include <time.h>
#include <errno.h>
typedef struct { pthread_mutex_t lock; pthread_cond_t changed; unsigned sleepers; unsigned long waits,timeouts; } NvFenceSignal;
#define NV_FENCE_SIGNAL_INIT {PTHREAD_MUTEX_INITIALIZER,PTHREAD_COND_INITIALIZER,0,0,0}
static void nv_fence_signal_lock(NvFenceSignal *s){pthread_mutex_lock(&s->lock);}
static void nv_fence_signal_unlock(NvFenceSignal *s){pthread_mutex_unlock(&s->lock);}
static void nv_fence_signal_wake(NvFenceSignal *s){pthread_cond_broadcast(&s->changed);}
static int nv_fence_signal_sleep(NvFenceSignal *s,unsigned ms){
    struct timespec end;clock_gettime(CLOCK_REALTIME,&end);
    end.tv_sec+=ms/1000;end.tv_nsec+=(ms%1000)*1000000;
    if(end.tv_nsec>=1000000000){++end.tv_sec;end.tv_nsec-=1000000000;}
    ++s->sleepers;++s->waits;
    int status=pthread_cond_timedwait(&s->changed,&s->lock,&end);
    --s->sleepers;if(status==ETIMEDOUT)++s->timeouts;
    return status==0?2:(status==ETIMEDOUT?1:0);
}
#endif
#endif
