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
static void reset(Disk *d,const uint8_t *base){
    memcpy(d->media,base,SIZE);memcpy(d->durable,base,SIZE);
    d->events=d->cut=d->mode=0;d->dead=0;
}
int main(int argc,char **argv){
    assert(argc==2);void *so=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(so);
    void *sym=dlsym(so,IFS_FS_COMMON_EXPORT_V2);assert(sym);
    IfsFsModuleApiEntryV2 fn=NULL;memcpy(&fn,&sym,sizeof fn);
    const IfsFsModuleApiV2 *api=fn();assert(ifs_fs_api_v2_valid(api,"qnx4"));
    const IfsFsCoreOpsV2 *c=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *v=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    const IfsFsDefragOpsV2 *m=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG);
    assert(c->create && m->recover && v->sync);
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
        memcpy(base,d.durable,SIZE);reset(&d,base);
        assert(c->mount(&h,&vol,&info)==0);
        uint64_t before=info.free_bytes;
        assert(c->create(vol,"/guard",IFS_FS_ENTRY_FILE)==IFS_FS_EXISTS && !d.events);
        assert(c->create(vol,"/new-directory",IFS_FS_ENTRY_DIRECTORY)==0);
        assert(v->query(vol,&info)==0);uint64_t after=info.free_bytes;assert(before>after);
        unsigned cuts=d.events;assert(cuts>=8);
        assert(c->unmount(vol)==0);
        unsigned recovered=0;
        for(unsigned mode=0;mode<3;mode++)for(unsigned cut=1;cut<=cuts;cut++){
            reset(&d,base);
            assert(c->mount(&h,&vol,&info)==0);
            d.cut=cut;d.mode=mode;
            assert(c->create(vol,"/new-directory",IFS_FS_ENTRY_DIRECTORY)!=IFS_FS_OK && d.dead);
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
            assert(c->stat(vol,"/guard",&e)==0&&!e.size_bytes);
            int state=c->stat(vol,"/new-directory",&e);
            assert(state==IFS_FS_OK||state==IFS_FS_NOT_FOUND);
            if(!state)assert(c->directory_entry(vol,"/new-directory",0,&e)==IFS_FS_END);
            assert(v->query(vol,&info)==0);
            assert(info.free_bytes==(state==IFS_FS_OK?after:before));
            assert(v->check(vol,0,&errors)==0&&!errors);
            assert(c->unmount(vol)==0);
        }
        assert(recovered);
        printf("QNX4 create journal interruption PASS sector=%u cases=%u recovered=%u\n",d.sector,cuts*3u,recovered);
    }
    free(base);free(d.media);free(d.durable);dlclose(so);return 0;
}
