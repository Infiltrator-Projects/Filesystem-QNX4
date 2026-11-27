/* SPDX-License-Identifier: GPL-3.0-or-later
 * Cold interruption qualification of native journaled directory creation.
 * Test host models durable barriers, failed callbacks and torn page writes.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <assert.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
enum { SIZE=1024*1024 };
typedef struct {uint8_t *media,*durable;uint32_t sector,events,cut,mode;int dead;} Disk;
static int rd(void *ctx,uint64_t at,void *p,uint32_t n){
    Disk *d=ctx;assert(at%d->sector==0 && n%d->sector==0 && n<=SIZE-at);
    if(d->dead)return -1;
    memcpy(p,d->media+at,n);return 0;
}
static int wr(void *ctx,uint64_t at,const void *p,uint32_t n){
    Disk *d=ctx;assert(at%d->sector==0 && n%d->sector==0 && n<=SIZE-at);
    if(d->dead)return -1;
    if(++d->events==d->cut){
        if(d->mode==1)memcpy(d->durable+at,p,n/2u);
        if(d->mode==2)memcpy(d->durable+at,p,n);
        d->dead=1;return -1;
    }
    memcpy(d->media+at,p,n);return 0;
}
static int fl(void *ctx){
    Disk *d=ctx;if(d->dead)return -1;
    if(++d->events==d->cut){
        if(d->mode==2)memcpy(d->durable,d->media,SIZE);
        d->dead=1;return -1;
    }
    memcpy(d->durable,d->media,SIZE);return 0;
}
static void *al(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void fr(void *ctx,void *p){(void)ctx;free(p);}
static IfsFsHostRuntimeV2 host(Disk *d,int writable){
    return (IfsFsHostRuntimeV2){.abi_version=2,.structure_bytes=sizeof(IfsFsHostRuntimeV2),
        .capabilities=writable?(IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE):IFS_FS_HOST_READ_ONLY,
        .volume_bytes=SIZE,.logical_sector_bytes=d->sector,.physical_sector_bytes=d->sector,
        .maximum_transfer_bytes=4096,.context=d,.read_at=rd,.write_at=writable?wr:NULL,
        .flush=writable?fl:NULL,.allocate=al,.release=fr};
}
static int namespace_change(const IfsFsCoreOpsV2 *c,void *vol,unsigned op){
    if(op==0)return c->create(vol,"/new-directory",IFS_FS_ENTRY_DIRECTORY);
    if(op==1)return c->rename(vol,"/source","/destination");
    if(op==2)return c->remove(vol,"/source");
    if(op==3){
        uint8_t payload[8192];
        for(unsigned i=0;i<sizeof payload;i++)payload[i]=(uint8_t)(i*13u+i/127u);
        return c->write(vol,"/guard",0,payload,sizeof payload);
    }
    return c->truncate(vol,"/payload",513u);
}
static void reset(Disk *d,const uint8_t *base){
    memcpy(d->media,base,SIZE);memcpy(d->durable,base,SIZE);
    d->events=d->cut=d->mode=0;d->dead=0;
}
/* Compare cold-recovered native extent maps against the exact old and
 * new states, not merely file contents or a successful return code. */
static uint32_t layout(const IfsFsDefragOpsV2 *m,void *vol,IfsFsExtentV2 out[8])
{
    memset(out,0,8*sizeof *out);
    uint32_t n=0,tail=99;IfsFsExtentV2 extra;
    assert(m->map(vol,"/target",0,out,8,&n)==0&&n&&n<8);
    assert(m->map(vol,"/target",n,&extra,1,&tail)==IFS_FS_END&&!tail);
    return n;
}
static void relocation_cuts(const IfsFsCoreOpsV2 *c,const IfsFsVolumeOpsV2 *v,
                            const IfsFsDefragOpsV2 *m,Disk *d,uint8_t *base,uint32_t policy)
{
    memset(d->media,0,SIZE);memset(d->durable,0,SIZE);
    d->events=d->cut=d->mode=0;d->dead=0;
    IfsFsHostRuntimeV2 h=host(d,1);
    assert(v->format(&h,4,NULL,0)==0);
    void *vol=NULL;IfsFsVolumeInfoV2 info={.structure_bytes=sizeof info};
    assert(c->mount(&h,&vol,&info)==0);
    uint8_t payload[16384],guard[8192],got[16384];
    for(unsigned i=0;i<sizeof payload;i++)payload[i]=(uint8_t)(i*19u+i/71u);
    memset(guard,0x73,sizeof guard);
    assert(c->create(vol,"/target",IFS_FS_ENTRY_FILE)==0);
    assert(c->create(vol,"/guard",IFS_FS_ENTRY_FILE)==0);
    assert(c->write(vol,"/target",0,payload,8192)==0);
    assert(c->write(vol,"/guard",0,guard,sizeof guard)==0);
    assert(c->write(vol,"/target",8192,payload+8192,8192)==0);
    IfsFsExtentV2 old[8],next[8],actual[8];
    uint32_t old_n=layout(m,vol,old);assert(old_n>1);
    assert(v->query(vol,&info)==0);uint64_t before=info.free_bytes;
    assert(c->unmount(vol)==0);
    memcpy(base,d->durable,SIZE);reset(d,base);
    assert(c->mount(&h,&vol,&info)==0);
    void *plan=NULL;uint64_t moved=0;
    assert(m->plan(vol,"/target",policy,&plan)==0&&!d->events);
    assert(m->execute(vol,plan,&moved)==0&&moved==sizeof payload);
    assert(m->verify(vol,plan)==0);
    uint32_t new_n=layout(m,vol,next);assert(new_n==1);
    assert(v->query(vol,&info)==0);uint64_t after=info.free_bytes;
    unsigned cuts=d->events;assert(cuts>=8);
    m->release_plan(vol,plan);assert(c->unmount(vol)==0);
    unsigned recovered=0;
    for(unsigned mode=0;mode<3;mode++)for(unsigned cut=1;cut<=cuts;cut++){
        reset(d,base);assert(c->mount(&h,&vol,&info)==0);
        assert(m->plan(vol,"/target",policy,&plan)==0&&!d->events);
        d->cut=cut;d->mode=mode;
        assert(m->execute(vol,plan,&moved)!=0&&d->dead);
        /* Simulate power loss with no provider sync or error repair. */
        assert(c->unmount(vol)==0);
        memcpy(d->media,d->durable,SIZE);d->cut=0;d->dead=0;d->events=0;
        IfsFsHostRuntimeV2 ro=host(d,0);
        int rr=c->mount(&ro,&vol,&info);
        assert(rr==IFS_FS_OK||rr==IFS_FS_BUSY||rr==IFS_FS_CORRUPT);
        if(!rr)assert(c->unmount(vol)==0);
        assert(!d->events);
        assert(c->mount(&h,&vol,&info)==0);
        uint64_t repaired=99,errors=99;
        assert(m->recover(vol,0,&repaired)==0&&repaired<=1);
        recovered+=(unsigned)repaired;
        assert(m->recover(vol,0,&repaired)==0&&!repaired);
        uint32_t n=layout(m,vol,actual);
        int is_old=n==old_n&&!memcmp(actual,old,sizeof old);
        int is_new=n==new_n&&!memcmp(actual,next,sizeof next);
        assert(is_old!=is_new);
        assert(c->read(vol,"/target",0,got,sizeof payload)==0&&!memcmp(got,payload,sizeof payload));
        assert(c->read(vol,"/guard",0,got,sizeof guard)==0&&!memcmp(got,guard,sizeof guard));
        assert(v->query(vol,&info)==0&&info.free_bytes==(is_new?after:before));
        assert(v->check(vol,0,&errors)==0&&!errors);
        assert(c->unmount(vol)==0);
    }
    assert(recovered);
    printf("QNX4 relocation journal interruption PASS policy=%u sector=%u cases=%u recovered=%u\n",policy,d->sector,cuts*3u,recovered);
}
int main(int argc,char **argv){
    assert(argc==2);void *so=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(so);
    void *sym=dlsym(so,IFS_FS_COMMON_EXPORT_V2);assert(sym);
    IfsFsModuleApiEntryV2 fn=NULL;memcpy(&fn,&sym,sizeof fn);
    const IfsFsModuleApiV2 *api=fn();assert(ifs_fs_api_v2_valid(api,"qnx4"));
    const IfsFsCoreOpsV2 *c=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *v=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    const IfsFsDefragOpsV2 *m=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG);
    assert(c->create && c->rename && c->remove && m->recover && v->sync);
    Disk d={.media=calloc(1,SIZE),.durable=calloc(1,SIZE)};
    uint8_t *base=malloc(SIZE);assert(d.media&&d.durable&&base);
    for(d.sector=512;d.sector<=4096;d.sector*=8){
        memset(d.media,0,SIZE);memset(d.durable,0,SIZE);d.events=d.cut=d.mode=0;d.dead=0;
        IfsFsHostRuntimeV2 h=host(&d,1);
        assert(v->format(&h,4,NULL,0)==0);
        void *vol=NULL;IfsFsVolumeInfoV2 info={.structure_bytes=sizeof info};
        assert(c->mount(&h,&vol,&info)==0);
        assert(c->create(vol,"/guard",IFS_FS_ENTRY_FILE)==0);
        assert(c->unmount(vol)==0);
        assert(c->mount(&h,&vol,&info)==0);
        assert(c->create(vol,"/source",IFS_FS_ENTRY_DIRECTORY)==0);
        assert(c->create(vol,"/payload",IFS_FS_ENTRY_FILE)==0);
        uint8_t payload[8192];
        for(unsigned i=0;i<sizeof payload;i++)payload[i]=(uint8_t)(i*13u+i/127u);
        assert(c->write(vol,"/payload",0,payload,sizeof payload)==0);
        assert(c->unmount(vol)==0);
        memcpy(base,d.durable,SIZE);
        for(unsigned op=0;op<5;op++){
        reset(&d,base);
        assert(c->mount(&h,&vol,&info)==0);
        uint64_t before=info.free_bytes;
        assert(c->create(vol,"/guard",IFS_FS_ENTRY_FILE)==IFS_FS_EXISTS && !d.events);
        assert(namespace_change(c,vol,op)==0);
        assert(v->query(vol,&info)==0);uint64_t after=info.free_bytes;assert((op==0||op==3)?before>after:op==1?before==after:before<after);
        unsigned cuts=d.events;assert(cuts>=8);
        assert(c->unmount(vol)==0);
        unsigned recovered=0;
        for(unsigned mode=0;mode<3;mode++)for(unsigned cut=1;cut<=cuts;cut++){
            reset(&d,base);
            assert(c->mount(&h,&vol,&info)==0);
            d.cut=cut;d.mode=mode;
            assert(namespace_change(c,vol,op)!=IFS_FS_OK && d.dead);
            assert(c->unmount(vol)==0);
            /* Discard volatile writes. Reopen through the public ABI. */
            memcpy(d.media,d.durable,SIZE);d.cut=0;d.dead=0;d.events=0;
            IfsFsHostRuntimeV2 ro=host(&d,0);
            int rr=c->mount(&ro,&vol,&info);
            assert(rr==IFS_FS_OK||rr==IFS_FS_BUSY||rr==IFS_FS_CORRUPT);
            if(!rr)assert(c->unmount(vol)==0);
            assert(!d.events);
            assert(c->mount(&h,&vol,&info)==0);
            uint64_t repairs=99,errors=99;
            assert(m->recover(vol,0,&repairs)==0 && repairs<=1);
            recovered+=(unsigned)repairs;
            assert(m->recover(vol,0,&repairs)==0 && !repairs);
            char name[64];IfsFsEntryV2 e={.structure_bytes=sizeof e,.name=name,.name_capacity=sizeof name};
            assert(c->stat(vol,"/guard",&e)==0);
            uint8_t got[8192];int committed=0;
            if(op==3){
                assert(e.size_bytes==0||e.size_bytes==sizeof payload);
                committed=e.size_bytes!=0;
                if(committed)assert(c->read(vol,"/guard",0,got,sizeof got)==0&&!memcmp(got,payload,sizeof got));
            }else assert(!e.size_bytes);
            assert(c->stat(vol,"/payload",&e)==0);
            if(op==4){
                assert(e.size_bytes==513u||e.size_bytes==sizeof payload);
                committed=e.size_bytes==513u;
            }else assert(e.size_bytes==sizeof payload);
            assert(c->read(vol,"/payload",0,got,(uint32_t)e.size_bytes)==0&&
                !memcmp(got,payload,(size_t)e.size_bytes));
            if(op<3){
                const char *target=op==0?"/new-directory":op==1?"/destination":"/source";
                int state=c->stat(vol,target,&e);
                assert(state==IFS_FS_OK||state==IFS_FS_NOT_FOUND);
                if(!state)assert(c->directory_entry(vol,target,0,&e)==IFS_FS_END);
                if(op==1)assert(c->stat(vol,"/source",&e)==(state==IFS_FS_OK?IFS_FS_NOT_FOUND:IFS_FS_OK));
                committed=op==2?state==IFS_FS_NOT_FOUND:state==IFS_FS_OK;
            }
            assert(v->query(vol,&info)==0);
            assert(info.free_bytes==(committed?after:before));
            assert(v->check(vol,0,&errors)==0&&!errors);
            assert(c->unmount(vol)==0);
        }
        assert(recovered);
        printf("QNX4 namespace journal interruption PASS op=%u sector=%u cases=%u recovered=%u\n",op,d.sector,cuts*3u,recovered);
        }
        relocation_cuts(c,v,m,&d,base,0);
        relocation_cuts(c,v,m,&d,base,10);
    }
    free(base);free(d.media);free(d.durable);dlclose(so);return 0;
}
