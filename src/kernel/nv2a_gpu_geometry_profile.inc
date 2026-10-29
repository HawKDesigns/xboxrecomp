/* CPU elapsed samples, not GPU timestamps. Cache/residency operations can
 * include their existing queue submissions and waits. Never add them to GPU
 * timing or enclosing batch time. Approximately 1/64 calls are timed; outcome
 * populations are exact, sampled means are estimates and can miss rare stalls.
 * Independent deterministic PRNG streams avoid fixed every-64th-draw aliasing.
 * Reporting resets populations, but keeps PRNG state across report windows. */
static int gpu_raw_profile_begin(unsigned phase,double *begin)
{
    NvRawProfile *p=&raw.profile[phase];
    if(!p->rng)p->rng=0x1036a5b7u+phase*0x9e3779b9u;
    uint32_t x=p->rng;x^=x<<13;x^=x>>17;x^=x<<5;p->rng=x;
    int sample=(x&63u)==0;
    if(sample)*begin=gpu_ms();
    return sample;
}
static void gpu_raw_profile_end(unsigned phase,unsigned outcome,int sample,double begin)
{
    NvRawProfile *p=&raw.profile[phase];++p->calls[outcome];
    if(sample){double elapsed=gpu_ms()-begin;++p->samples[outcome];p->ms[outcome]+=elapsed;
        if(elapsed>p->peak_ms[outcome])p->peak_ms[outcome]=elapsed;}
}
static NvRawMesh *gpu_raw_profile_mesh(const NvRawInput *in)
{
    if(g.selftest || !raw.profile_enabled)return gpu_raw_mesh(in);
    double begin=0;int sample=gpu_raw_profile_begin(0,&begin);uint64_t hits=raw.hits;
    NvRawMesh *m=gpu_raw_mesh(in);
    gpu_raw_profile_end(0,!m?2:(raw.hits!=hits?0:1),sample,begin);return m;
}
static int gpu_raw_profile_bounds(const NvRawMesh *m,const NvRawInput *in)
{
    if(g.selftest || !raw.profile_enabled)return gpu_raw_bounds(m,in);
    double begin=0;int sample=gpu_raw_profile_begin(1,&begin);int result=gpu_raw_bounds(m,in);
    gpu_raw_profile_end(1,result>0?0:(result<0?1:2),sample,begin);return result;
}
static int gpu_raw_profile_resident(NvRawMesh *m)
{
    if(g.selftest || !raw.profile_enabled)return gpu_raw_resident(m);
    double begin=0;int sample=gpu_raw_profile_begin(2,&begin);unsigned resident=m->resident;
    int result=gpu_raw_resident(m);
    gpu_raw_profile_end(2,!result?2:(resident?0:1),sample,begin);return result;
}
static void gpu_raw_profile_report(void)
{
    static const char *phase[3]={"mesh","bounds","resident"};
    static const char *outcome[3][3]={{"hit","built","failed"},{"inside","outside","cpu_fallback"},{"reused","uploaded","failed"}};
    for(unsigned i=0;i<3;i++){
        NvRawProfile *p=&raw.profile[i];
        for(unsigned j=0;j<3;j++)fprintf(stderr,"[GPU-GEOMETRY-CPU] enabled=%u phase=%s outcome=%s calls=%llu samples=%llu sample_ms=%.6f sample_peak_ms=%.6f sample_rate=64\n",
            raw.profile_enabled,phase[i],outcome[i][j],(unsigned long long)p->calls[j],(unsigned long long)p->samples[j],p->ms[j],p->peak_ms[j]);
        uint32_t rng=p->rng;memset(p,0,sizeof *p);p->rng=rng;
    }
}
