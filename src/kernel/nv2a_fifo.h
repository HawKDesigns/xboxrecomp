#ifndef NV2A_FIFO_H
#define NV2A_FIFO_H
#include <stdint.h>
#include <string.h>
/* Persistent DMA decoder: GET names the next unread word, including inside
 * a partially submitted method packet. JUMPs carry the real wrap target. */
typedef struct {
    uint32_t cursor,ring_lo,ring_hi,return_va,remaining,method,subch;
    uint32_t words,jumps,calls,error_word;
    int initialized,noninc,in_call,error;
} Nv2aFifo;
typedef int (*Nv2aFifoRead)(uint32_t,uint32_t*);
typedef void (*Nv2aFifoEmit)(uint32_t,uint32_t,uint32_t);
static inline uint32_t nv2a_fifo_target(uint32_t physical)
{ return 0x80000000u|(physical&0x0FFFFFFCu); }
static inline uint32_t nv2a_fifo_run(Nv2aFifo *s,uint32_t get,uint32_t put,
    uint32_t lo,uint32_t hi,Nv2aFifoRead read,Nv2aFifoEmit emit,uint32_t budget)
{
    if(!s->initialized || get!=s->cursor || lo!=s->ring_lo || hi!=s->ring_hi) {
        memset(s,0,sizeof *s);s->initialized=1;s->cursor=get;s->ring_lo=lo;s->ring_hi=hi;
    }
    if(s->error || lo>=hi || (put&3) || put<lo || put>hi)return s->cursor;
    for(uint32_t n=0;n<budget;n++) {
        uint32_t word,pc=s->cursor,target;
        if(!s->in_call && pc==put)break;
        if((pc&3) || (!s->in_call && (pc<lo || pc>=hi)) || !read(pc,&word)) {
            s->error=1;s->error_word=0xFFFFFFFF;break;
        }
        if(s->remaining) {
            emit(s->subch,s->method,word);
            if(!s->noninc)s->method+=4;
            --s->remaining;s->cursor=pc+4;++s->words;continue;
        }
        if((word&3u)==1u || (word&0xE0000003u)==0x20000000u) {
            target=nv2a_fifo_target((word&3u)==1u?word&~3u:word&0x1FFFFFFCu);
            if(!s->in_call && (target<lo || target>=hi)) {
                s->error=1;s->error_word=word;break;
            }
            s->cursor=target;++s->jumps;++s->words;continue;
        }
        if((word&0xFFFF0003u)==0x00020000u) {
            if(!s->in_call){s->error=1;s->error_word=word;break;}
            s->cursor=s->return_va;s->in_call=0;++s->words;continue;
        }
        if((word&3u)==2u) {
            if(s->in_call){s->error=1;s->error_word=word;break;}
            s->return_va=pc+4;s->cursor=nv2a_fifo_target(word&~3u);
            s->in_call=1;++s->calls;++s->words;continue;
        }
        if((word&0xE0030003u)==0 || (word&0xE0030003u)==0x40000000u) {
            s->remaining=(word>>18)&0x7FF;s->method=word&0x1FFC;
            s->subch=(word>>13)&7;s->noninc=(word&0x40000000u)!=0;
            s->cursor=pc+4;++s->words;continue;
        }
        s->error=1;s->error_word=word;break;
    }
    return s->cursor;
}
#endif
