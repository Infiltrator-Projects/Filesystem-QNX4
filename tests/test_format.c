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
        assert(c->mount(&h,&vol,&info)==0&&info.free_bytes==d.bytes-(uint64_t)(inode_block+16u+512u)*512);
        char name[64];IfsFsEntryV2 e={.structure_bytes=sizeof e,.name=name,.name_capacity=sizeof name};
        assert(c->stat(vol,"/.inodes",&e)==0&&e.size_bytes==8192);
        assert(c->stat(vol,"/.boot",&e)==0&&!e.size_bytes);
        assert(c->stat(vol,"/.altboot",&e)==0&&!e.size_bytes);
        assert(c->directory_entry(vol,"/",4,&e)==IFS_FS_END);
        uint64_t errors=99;assert(v->check(vol,0,&errors)==0&&!errors);
        assert(c->unmount(vol)==0);
        h.capabilities=IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE;h.write_at=wr;h.flush=fl;
        assert(c->mount(&h,&vol,&info)==0);
        assert(c->create(vol,"/folder",IFS_FS_ENTRY_DIRECTORY)==0);
        assert(c->create(vol,"/folder/native-long-file-name.bin",IFS_FS_ENTRY_FILE)==0);
        assert(c->create(vol,"/folder/native-long-file-name.bin",IFS_FS_ENTRY_FILE)==IFS_FS_EXISTS);
        assert(c->create(vol,"/.ifs-journal",IFS_FS_ENTRY_FILE)==IFS_FS_ACCESS);
        assert(v->check(vol,0,&errors)==0&&!errors);
        assert(c->unmount(vol)==0);
        h.capabilities=IFS_FS_HOST_READ_ONLY;h.write_at=NULL;h.flush=NULL;
        assert(c->mount(&h,&vol,&info)==0);
        assert(c->stat(vol,"/folder/native-long-file-name.bin",&e)==0&&!e.size_bytes);
        assert(c->stat(vol,"/.ifs-journal",&e)==IFS_FS_NOT_FOUND);
        assert(c->unmount(vol)==0);
        /* Corrupt only the directory's native parent backlink, leaving the
         * bitmap, inode and journal untouched. A read-only mount permits
         * diagnosis; integrity checking and writable mount must reject it. */
        uint8_t link[64], child[64], old_parent[5], bad_parent[5]={2,0,0,0,1};
        assert(pread(d.fd,link,sizeof link,(off_t)first*512+7*64)==(ssize_t)sizeof link);
        assert(!memcmp(link,"folder",6));
        off_t child_at=(off_t)(le32(link+48)-1u)*512+link[52]*64;
        assert(pread(d.fd,child,sizeof child,child_at)==(ssize_t)sizeof child);
        off_t parent_at=(off_t)(le32(child+20)-1u)*512+64+48;
        assert(pread(d.fd,old_parent,sizeof old_parent,parent_at)==(ssize_t)sizeof old_parent);
        assert(pwrite(d.fd,bad_parent,sizeof bad_parent,parent_at)==(ssize_t)sizeof bad_parent);
        assert(c->mount(&h,&vol,&info)==0);
        assert(v->check(vol,0,&errors)==0&&errors);
        assert(c->unmount(vol)==0);
        h.capabilities=IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE;h.write_at=wr;h.flush=fl;
        before=d.writes;
        assert(c->mount(&h,&vol,&info)==IFS_FS_CORRUPT&&!vol&&d.writes==before);
        assert(pwrite(d.fd,old_parent,sizeof old_parent,parent_at)==(ssize_t)sizeof old_parent);
        assert(c->mount(&h,&vol,&info)==0);
        assert(v->check(vol,0,&errors)==0&&!errors);
        assert(c->unmount(vol)==0);
        assert(c->mount(&h,&vol,&info)==0);
        uint64_t free_before=info.free_bytes;
        /* Exercise unaligned writes, large streaming growth, holes, physical
         * extent chains, preservation of surrounding bytes and cold reopen. */
        const char *payload="/payload";
        uint32_t len=d.sector==512?65536u:2u*1024u*1024u;
        uint8_t *data=malloc(len+2000u),*got=malloc(len+2000u);
        assert(data&&got);
        for(uint32_t i=0;i<len;i++)data[i]=(uint8_t)(i*17u+i/509u);
        assert(c->create(vol,payload,IFS_FS_ENTRY_FILE)==0);
        assert(c->write(vol,payload,0,data,len)==0);
        assert(c->write(vol,payload,len+601u,"tail",4)==0);
        memset(data+len,0,601u);memcpy(data+len+601u,"tail",4);
        memset(data+507u,0xa5,1003u);
        assert(c->write(vol,payload,507u,data+507u,1003u)==0);
        assert(c->read(vol,payload,0,got,len+605u)==0&&!memcmp(got,data,len+605u));
        assert(v->check(vol,0,&errors)==0&&!errors);
        assert(c->unmount(vol)==0&&c->mount(&h,&vol,&info)==0);
        assert(c->read(vol,payload,0,got,len+605u)==0&&!memcmp(got,data,len+605u));
        assert(c->truncate(vol,payload,513u)==0);
        assert(c->truncate(vol,payload,1700u)==0);
        memset(data+513u,0,1187u);
        assert(c->read(vol,payload,0,got,1700u)==0&&!memcmp(got,data,1700u));
        assert(v->check(vol,0,&errors)==0&&!errors);
        assert(c->remove(vol,payload)==0);
        assert(v->query(vol,&info)==0&&info.free_bytes==free_before);
        free(data);free(got);

        assert(c->create(vol,"/other",IFS_FS_ENTRY_DIRECTORY)==0);
        assert(c->rename(vol,"/folder","/folder/sub")==IFS_FS_INVALID);
        assert(c->remove(vol,"/folder")==IFS_FS_BUSY);
        assert(c->rename(vol,"/folder","/other/moved")==0);
        assert(c->stat(vol,"/folder",&e)==IFS_FS_NOT_FOUND);
        assert(c->stat(vol,"/other/moved/native-long-file-name.bin",&e)==0);
        assert(c->rename(vol,"/other/moved/native-long-file-name.bin","/other/file")==0);
        assert(c->rename(vol,"/other/file","/other/moved")==IFS_FS_EXISTS);
        assert(c->remove(vol,"/other/moved")==0);
        assert(c->remove(vol,"/other/file")==0);
        assert(c->remove(vol,"/other")==0);
        assert(c->remove(vol,"/.ifs-journal")==IFS_FS_ACCESS);
        assert(v->query(vol,&info)==0&&info.free_bytes==free_before+512u);
        assert(v->check(vol,0,&errors)==0&&!errors);
        assert(c->unmount(vol)==0);
        assert(c->mount(&h,&vol,&info)==0);
        assert(c->stat(vol,"/other",&e)==IFS_FS_NOT_FOUND);
        assert(v->check(vol,0,&errors)==0&&!errors);
        assert(c->unmount(vol)==0);
        printf("QNX4 native format, journalled namespace and parent integrity PASS sector=%u\n",d.sector);
    }
    assert(close(d.fd)==0);dlclose(so);return 0;
}
