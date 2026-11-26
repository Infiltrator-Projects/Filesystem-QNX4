/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent hand-encoded native structures; not a foreign-media claim.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <assert.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
enum { SIZE=65536 };
typedef struct {uint8_t data[SIZE];uint32_t sector;} Disk;
static int rd(void *ctx,uint64_t at,void *p,uint32_t n){
    Disk *d=ctx;assert(at%d->sector==0&&n%d->sector==0&&at<=SIZE&&n<=SIZE-at);
    memcpy(p,d->data+at,n);return 0;
}
static void *al(void *c,size_t n){(void)c;return malloc(n);}
static void fr(void *c,void *p){(void)c;free(p);}
static void p16(uint8_t *p,uint32_t n){p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static void p32(uint8_t *p,uint32_t n){p16(p,n);p16(p+2,n>>16);}
static void node(uint8_t *p,const char *name,uint32_t size,uint32_t first,uint32_t count,uint32_t mode,uint32_t status){
    memcpy(p,name,strlen(name));p32(p+16,size);p32(p+20,first);p32(p+24,count);
    p16(p+48,count?1:0);p16(p+50,mode);p16(p+56,1);p[63]=(uint8_t)status;
}
static void linknode(uint8_t *p,const char *name,uint32_t block,uint8_t slot){
    memcpy(p,name,strlen(name));p32(p+48,block);p[52]=slot;p[63]=8;
}
static void used(Disk *d,uint32_t block){d->data[1024+block/8]|=(uint8_t)(1u<<(block%8));}
static void fixture(Disk *d){
    memset(d->data,0,SIZE);
    node(d->data+512,"/",512,4,1,0040755,1);
    node(d->data+576,".inodes",512,5,1,0100600,1);
    uint8_t *root=d->data+1536;
    linknode(root,".",2,0);linknode(root+64,"..",2,0);
    node(root+128,".bitmap",16,3,1,0100400,1);
    linknode(root+192,"a-long-native-filename.bin",5,1);
    node(root+256,"short",7,9,1,0100644,1);
    uint8_t *ino=d->data+2048+64;
    node(ino,"inode",700,6,1,0100644,16);
    p16(ino+48,2);p32(ino+28,8);
    uint8_t *x=d->data+7*512;
    x[8]=1;p32(x+12,1);p32(x+16,7);p32(x+20,1);memcpy(x+496,"IamXblk",7);
    p32(x+504,6);p32(x+508,1);
    for(unsigned i=0;i<512;i++)d->data[5*512+i]=(uint8_t)i;
    for(unsigned i=0;i<188;i++)d->data[6*512+i]=(uint8_t)(255-i);
    memcpy(d->data+8*512,"QNX4!!!",7);
    for(unsigned b=0;b<9;b++)used(d,b);
}
int main(int argc,char **argv){
    assert(argc==2);void *so=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(so);
    void *sym=dlsym(so,IFS_FS_COMMON_EXPORT_V2);assert(sym);IfsFsModuleApiEntryV2 fn=NULL;memcpy(&fn,&sym,sizeof fn);
    const IfsFsModuleApiV2 *api=fn();assert(ifs_fs_api_v2_valid(api,"qnx4"));
    const IfsFsCoreOpsV2 *c=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsDefragOpsV2 *m=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG);
    const IfsFsAllocationOpsV2 *a=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_ALLOCATION);
    assert(!c->create&&!c->write&&!m->execute);
    Disk d;for(d.sector=512;d.sector<=4096;d.sector*=8){
        fixture(&d);
        IfsFsHostRuntimeV2 h={.abi_version=2,.structure_bytes=sizeof h,.capabilities=IFS_FS_HOST_READ_ONLY,
        .volume_bytes=SIZE,.logical_sector_bytes=d.sector,.physical_sector_bytes=d.sector,
        .maximum_transfer_bytes=4096,.context=&d,.read_at=rd,.allocate=al,.release=fr};
        uint32_t variant=0;assert(c->probe(&h,&variant)==0&&variant==4);
        void *v=NULL;IfsFsVolumeInfoV2 info={.structure_bytes=sizeof info};
        assert(c->mount(&h,&v,&info)==0&&info.free_bytes==SIZE-9*512);
        char name[64];IfsFsEntryV2 e={.structure_bytes=sizeof e,.name=name,.name_capacity=sizeof name};
        assert(c->directory_entry(v,"/",0,&e)==0&&!strcmp(name,".bitmap"));
        assert(c->directory_entry(v,"/",1,&e)==0&&!strcmp(name,"a-long-native-filename.bin")&&e.size_bytes==700);
        uint8_t buf[700];assert(c->read(v,"/a-long-native-filename.bin",0,buf,sizeof buf)==0);
        for(unsigned i=0;i<512;i++)assert(buf[i]==(uint8_t)i);
        for(unsigned i=0;i<188;i++)assert(buf[512+i]==(uint8_t)(255-i));
        IfsFsExtentV2 ex;uint32_t n=0;
        assert(m->map(v,"/a-long-native-filename.bin",0,&ex,1,&n)==0&&n==1&&ex.length==700&&ex.physical_offset==5*512);
        assert(m->map(v,"/a-long-native-filename.bin",1,&ex,1,&n)==IFS_FS_END&&!n);
        uint64_t at=0,unknown=0,free_bytes=0;
        for(;;){IfsFsAllocationRangeV2 ranges[3];uint64_t next=at;int rc=a->map(v,at,ranges,3,&n);
            assert(ifs_fs_allocation_page_v2_valid(SIZE,at,rc,ranges,3,n,&next,&unknown));
            if(rc==IFS_FS_END)break;
            for(unsigned i=0;i<n;i++)if(ranges[i].state==IFS_FS_SPACE_FREE)free_bytes+=ranges[i].length;
            at=next;
        }
        assert(at==SIZE&&!unknown&&free_bytes==info.free_bytes);
        d.data[7*512+496]='X';assert(c->read(v,"/a-long-native-filename.bin",0,buf,1)==IFS_FS_CORRUPT);
        d.data[7*512+496]='I';p32(d.data+7*512,8);assert(c->read(v,"/a-long-native-filename.bin",0,buf,1)==IFS_FS_CORRUPT);
        p32(d.data+7*512,0);d.data[1536+192+52]=8;
        assert(c->stat(v,"/a-long-native-filename.bin",&e)==IFS_FS_CORRUPT);
        assert(c->unmount(v)==0&&c->stat(v,"/short",&e)==IFS_FS_INVALID);
        fixture(&d);d.data[1024]&=(uint8_t)~(1u<<3);
        assert(c->mount(&h,&v,&info)==IFS_FS_CORRUPT&&!v);
        printf("QNX4 native reader, extent chain, links and bitmap PASS sector=%u\n",d.sector);
    }
    dlclose(so);return 0;
}
