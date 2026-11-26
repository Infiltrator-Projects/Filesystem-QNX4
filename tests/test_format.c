/* SPDX-License-Identifier: GPL-3.0-or-later
 * Actual file-backed format, native structure and cold mount verification.
 */
#define _POSIX_C_SOURCE 200809L
#include <infiltratr/filesystem_module_v2.h>
#include <assert.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
typedef struct{int fd;uint64_t bytes;uint32_t sector,writes;} Disk;
static int rd(void *ctx,uint64_t at,void *p,uint32_t n){
    Disk *d=ctx;assert(at%d->sector==0&&n%d->sector==0&&at<=d->bytes&&n<=d->bytes-at);
    return pread(d->fd,p,n,(off_t)at)==(ssize_t)n?0:-1;
}
static int wr(void *ctx,uint64_t at,const void *p,uint32_t n){
    Disk *d=ctx;assert(at%d->sector==0&&n%d->sector==0&&at<=d->bytes&&n<=d->bytes-at);++d->writes;
    return pwrite(d->fd,p,n,(off_t)at)==(ssize_t)n?0:-1;
}
static int fl(void *ctx){return fsync(((Disk*)ctx)->fd);}
static void *al(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void fr(void *ctx,void *p){(void)ctx;free(p);}
static uint32_t le32(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
int main(int argc,char **argv){
    assert(argc==2);void *so=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(so);
    void *sym=dlsym(so,IFS_FS_COMMON_EXPORT_V2);assert(sym);IfsFsModuleApiEntryV2 fn=NULL;memcpy(&fn,&sym,sizeof fn);
    const IfsFsModuleApiV2 *api=fn();assert(ifs_fs_api_v2_valid(api,"qnx4"));
    const IfsFsCoreOpsV2 *c=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *v=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    const IfsFsFormatProfileOpsV2 *fp=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_FORMAT_PROFILE);
    IfsFsFormatProfileV2 profile={.structure_bytes=sizeof profile};
    assert(fp&&fp->get_default(&profile)==0&&ifs_fs_format_profile_v2_valid(&profile));
    char path[]="/tmp/qnx4-format-XXXXXX";Disk d={.fd=mkstemp(path)};assert(d.fd>=0&&unlink(path)==0);
    for(d.sector=512;d.sector<=4096;d.sector*=8){
        d.bytes=d.sector==512?profile.minimum_bytes:profile.recommended_bytes;
        assert(ftruncate(d.fd,(off_t)d.bytes)==0);
        IfsFsHostRuntimeV2 h={.abi_version=2,.structure_bytes=sizeof h,.capabilities=IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE,
          .volume_bytes=d.bytes,.logical_sector_bytes=d.sector,.physical_sector_bytes=d.sector,.maximum_transfer_bytes=65536,
          .context=&d,.read_at=rd,.write_at=wr,.flush=fl,.allocate=al,.release=fr};
        unsigned before=d.writes;assert(v->format(&h,0,NULL,0)==IFS_FS_UNSUPPORTED&&d.writes==before);
        assert(v->format(&h,4,"NOT-SUPPORTED",0)==IFS_FS_UNSUPPORTED&&d.writes==before);
        assert(v->format(&h,profile.format_variant,NULL,0)==0);
        uint8_t root[512],dir[512],bitmap[512],inode[512];assert(pread(d.fd,root,512,512)==512);
        uint32_t first=le32(root+20)-1u,bm=(uint32_t)((d.bytes/512+4095u)/4096u);
        assert(first==2+bm&&le32(root+24)==4);
        assert(pread(d.fd,dir,512,(off_t)first*512)==512);
        assert(!memcmp(dir+128,".bitmap",7)&&le32(dir+128+20)==3&&le32(dir+128+24)==bm);
        assert(!memcmp(dir+192,".inodes",7)&&le32(dir+192+48)==2&&dir[192+52]==1);
        uint32_t inode_block=le32(root+64+20)-1u;
        assert(inode_block==first+4&&le32(root+64+24)==16);
        assert(pread(d.fd,inode,512,(off_t)inode_block*512)==512&&!memcmp(inode,"IamTHE.inodeFILE",16));
        assert(pread(d.fd,bitmap,512,1024)==512);
        for(uint32_t b=0;b<inode_block+16u;b++)assert(bitmap[b/8]&(1u<<(b%8)));
        h.capabilities=IFS_FS_HOST_READ_ONLY;h.write_at=NULL;h.flush=NULL;
        uint32_t variant=0;assert(c->probe(&h,&variant)==0&&variant==4);
        void *vol=NULL;IfsFsVolumeInfoV2 info={.structure_bytes=sizeof info};
        assert(c->mount(&h,&vol,&info)==0&&info.free_bytes==d.bytes-(uint64_t)(inode_block+16u)*512);
        char name[64];IfsFsEntryV2 e={.structure_bytes=sizeof e,.name=name,.name_capacity=sizeof name};
        assert(c->stat(vol,"/.inodes",&e)==0&&e.size_bytes==8192);
        assert(c->stat(vol,"/.boot",&e)==0&&!e.size_bytes);
        assert(c->stat(vol,"/.altboot",&e)==0&&!e.size_bytes);
        assert(c->directory_entry(vol,"/",4,&e)==IFS_FS_END);
        assert(c->unmount(vol)==0);
        printf("QNX4 native format PASS sector=%u\n",d.sector);
    }
    assert(close(d.fd)==0);dlclose(so);return 0;
}
