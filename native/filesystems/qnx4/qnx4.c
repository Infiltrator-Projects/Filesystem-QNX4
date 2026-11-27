/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent QNX4 reader. On-disk reference: QNX 4.25 User Guide,
 * recovery/disk structure, and Linux UAPI layout definitions. This is
 * new code, not a Linux VFS implementation. All I/O is Common V2.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>
enum { BB=512,REC=64,USED=1,LINK=8,INODE=16,BUSY=4,REG=0100000,DIR=0040000 };
typedef struct {uint32_t first,count;} Run;
typedef struct {uint8_t raw[REC];uint64_t identity;} Node;
typedef struct {Run *run;uint32_t count;} Map;
typedef struct {IfsFsHostRuntimeV2 host;uint32_t blocks;uint8_t *bitmap,*reserved;
    Node root,inodes;Map inode_map;uint64_t free;uintptr_t token;
    int journal_owned,writable,fault;uint64_t recovered;} Session;
static Session active;
static uintptr_t active_plan;
static int plan_executing;
static void reloc_drop(void);
static uintptr_t generation;
static atomic_flag gate=ATOMIC_FLAG_INIT;
static int enter(void){return !atomic_flag_test_and_set_explicit(&gate,memory_order_acquire);}
static int leave(int rc){atomic_flag_clear_explicit(&gate,memory_order_release);return rc;}
static int live(void *v){return v&&(uintptr_t)v==active.token&&!active.fault;}
static uint32_t le16(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8;}
static uint32_t le32(const uint8_t *p){return le16(p)|le16(p+2)<<16;}
static void put16(uint8_t *p,uint32_t n){p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static void put32(uint8_t *p,uint32_t n){put16(p,n);put16(p+2,n>>16);}
static uint32_t size(const Node *n){return le32(n->raw+16);}
static uint32_t mode(const Node *n){return le16(n->raw+50)&0170000u;}
static int cancelled(const Session *s){return s->host.cancelled&&s->host.cancelled(s->host.context);}
static void *alloc(const Session *s,size_t n){return s->host.allocate(s->host.context,n);}
static void drop(const Session *s,void *p){if(p)s->host.release(s->host.context,p);}
static int rd(const Session *s,uint64_t at,void *p,uint32_t n)
{return ifs_fs_host_read_bytes_v2(&s->host,at,p,n);}
static int bit(const uint8_t *p,uint32_t b){return (p[b/8]>>(b%8))&1;}
static int name_is(const uint8_t *p,unsigned width,const char *name)
{
    size_t n=strlen(name);
    return n<=width&&!memcmp(p,name,n)&&(n==width||!p[n]);
}
static int node_valid(const Node *n)
{
    unsigned status=n->raw[63];
    if((status&LINK)||!(status&(USED|INODE))||(status&BUSY)||!le16(n->raw+56))
        return IFS_FS_CORRUPT;
    if(mode(n)!=REG&&mode(n)!=DIR&&mode(n)!=0120000u)return IFS_FS_UNSUPPORTED;
    if(mode(n)==DIR&&size(n)%REC)return IFS_FS_CORRUPT;
    return IFS_FS_OK;
}
static void map_drop(const Session *s,Map *m){drop(s,m->run);*m=(Map){0};}
static int claim(const Session *s,uint8_t *seen,uint32_t first,uint32_t count)
{
    if(first<2||first>=s->blocks||!count||count>s->blocks-first)return IFS_FS_CORRUPT;
    for(uint32_t b=first;b<first+count;b++){
        if(bit(seen,b)||(s->bitmap&&!bit(s->bitmap,b)))return IFS_FS_CORRUPT;
        seen[b/8]|=(uint8_t)(1u<<(b%8));
    }
    return IFS_FS_OK;
}
static int add_extent(const Session *s,Map *m,uint8_t *seen,const uint8_t *p,uint64_t *total)
{
    uint32_t address=le32(p),count=le32(p+4);
    if(!address)return IFS_FS_CORRUPT;
    int rc=claim(s,seen,address-1u,count);if(rc)return rc;
    m->run[m->count++]=(Run){address-1u,count};*total+=count;
    return IFS_FS_OK;
}
/* The first extent is in the inode. Extension blocks are metadata,
 * checked independently of payload ownership, including reverse links.
 * A single seen bitmap detects both cycles and crosslinked local runs. */
static int node_map_claim(const Session *s,const Node *n,Map *m,uint8_t *global)
{
    *m=(Map){0};
    uint32_t count=le16(n->raw+48),next=le32(n->raw+28);
    uint64_t needed=((uint64_t)size(n)+511u)/BB,total=0;
    if(!count)return needed||next||le32(n->raw+24)?IFS_FS_CORRUPT:IFS_FS_OK;
    if(count>s->blocks||count>needed)return IFS_FS_CORRUPT;
    m->run=alloc(s,(size_t)count*sizeof(Run));
    size_t bytes=(size_t)(((uint64_t)s->blocks+7u)/8u);
    uint8_t *seen=global?global:alloc(s,bytes);
    int rc=IFS_FS_NO_MEMORY;
    if(!m->run||!seen)goto done;
    if(!global)memset(seen,0,bytes);
    rc=add_extent(s,m,seen,n->raw+20,&total);if(rc)goto done;
    uint32_t prev=0;
    while(m->count<count){
        if(cancelled(s)){rc=IFS_FS_BUSY;goto done;}
        if(!next){rc=IFS_FS_CORRUPT;goto done;}
        rc=claim(s,seen,next-1u,1);if(rc)goto done;
        uint8_t p[BB];rc=rd(s,(uint64_t)(next-1u)*BB,p,BB);if(rc)goto done;
        uint32_t entries=p[8];
        if(memcmp(p+496,"IamXblk",7)||le32(p+4)!=prev||
           !entries||entries>60u||entries>count-m->count||
           (!prev&&memcmp(p+504,n->raw+20,8))){rc=IFS_FS_CORRUPT;goto done;}
        uint64_t subtotal=0;
        for(uint32_t i=0;i<entries;i++){
            rc=add_extent(s,m,seen,p+16+8*i,&subtotal);if(rc)goto done;
        }
        if(subtotal!=le32(p+12)){rc=IFS_FS_CORRUPT;goto done;}
        total+=subtotal;prev=next;next=le32(p);
    }
    if(next||total<needed){rc=IFS_FS_CORRUPT;goto done;}
    rc=IFS_FS_OK;
done:
    if(!global)drop(s,seen);
    if(rc)map_drop(s,m);
    return rc;
}
static int node_map(const Session *s,const Node *n,Map *m)
{return node_map_claim(s,n,m,NULL);}
static int map_read(const Session *s,const Map *m,uint64_t offset,void *p,uint32_t length)
{
    uint8_t *out=p;
    for(uint32_t i=0;i<m->count&&length;i++){
        uint64_t bytes=(uint64_t)m->run[i].count*BB;
        if(offset>=bytes){offset-=bytes;continue;}
        uint32_t take=length;if(take>bytes-offset)take=(uint32_t)(bytes-offset);
        if(cancelled(s))return IFS_FS_BUSY;
        int rc=rd(s,(uint64_t)m->run[i].first*BB+offset,out,take);if(rc)return rc;
        length-=take;out+=take;offset=0;
    }
    return length?IFS_FS_CORRUPT:IFS_FS_OK;
}
static int map_address(const Map *m,uint64_t offset,uint64_t *address)
{
    for(uint32_t i=0;i<m->count;i++){
        uint64_t bytes=(uint64_t)m->run[i].count*BB;
        if(offset<bytes){*address=(uint64_t)m->run[i].first*BB+offset;return IFS_FS_OK;}
        offset-=bytes;
    }
    return IFS_FS_CORRUPT;
}
static int read_node(const Session *s,const Node *n,uint64_t at,void *p,uint32_t len)
{
    if(at>size(n)||len>size(n)-at||(!p&&len))return IFS_FS_INVALID;
    Map m;int rc=node_map(s,n,&m);if(rc)return rc;
    rc=map_read(s,&m,at,p,len);map_drop(s,&m);return rc;
}
static int inode_location(const Session *s,uint32_t address,uint8_t slot,uint64_t *out)
{
    if(!address||slot>=8||address>s->blocks)return IFS_FS_CORRUPT;
    uint32_t block=address-1u;uint64_t logical=0;
    for(uint32_t i=0;i<s->inode_map.count;i++){
        Run r=s->inode_map.run[i];
        if(block>=r.first&&block-r.first<r.count){
            uint64_t at=logical+(uint64_t)(block-r.first)*BB+slot*REC;
            /* Record zero is the reserved inode-file identifier. */
            if(at<REC||at+REC>size(&s->inodes))return IFS_FS_CORRUPT;
            *out=(uint64_t)block*BB+slot*REC;return IFS_FS_OK;
        }
        logical+=(uint64_t)r.count*BB;
    }
    return IFS_FS_CORRUPT;
}
typedef struct {Node node;char name[49];uint32_t length;} Entry;
static int decode_entry(const Session *s,const Node *dir,const Map *m,uint64_t at,
                        const uint8_t p[REC],Entry *out)
{
    unsigned status=p[63];
    if(!p[0]||!(status&(USED|LINK)))return IFS_FS_NOT_FOUND;
    if((status&(USED|LINK))==(USED|LINK)||status&BUSY)return IFS_FS_CORRUPT;
    unsigned width=(status&LINK)?48u:16u,n=0;
    while(n<width&&p[n]){if(p[n]=='/')return IFS_FS_CORRUPT;++n;}
    *out=(Entry){.length=n};memcpy(out->name,p,n);
    if(name_is(p,width,".")||name_is(p,width,".."))return IFS_FS_END;
    int rc;
    if(status&LINK){
        uint32_t address=le32(p+48);uint8_t slot=p[52];
        int special=dir->identity==BB&&address==2u&&
            ((slot==1u&&name_is(p,width,".inodes"))||
             (slot==2u&&name_is(p,width,".boot"))||
             (slot==3u&&name_is(p,width,".altboot")));
        if(special)out->node.identity=BB+slot*REC;
        else {
            rc=inode_location(s,address,slot,&out->node.identity);
            if(rc)return rc;
        }
        rc=rd(s,out->node.identity,out->node.raw,REC);if(rc)return rc;
        if(!special&&!(out->node.raw[63]&INODE))return IFS_FS_CORRUPT;
    }else{
        memcpy(out->node.raw,p,REC);
        rc=map_address(m,at,&out->node.identity);if(rc)return rc;
    }
    return node_valid(&out->node);
}
static int directory_entry(const Session *s,const Node *dir,const char *name,uint32_t len,uint32_t index,Entry *out)
{
    if(mode(dir)!=DIR)return IFS_FS_INVALID;
    Map m;int rc=node_map(s,dir,&m);if(rc)return rc;
    uint32_t visible=0;int found=0;
    for(uint64_t at=0;at<size(dir);at+=REC){
        if(cancelled(s)){rc=IFS_FS_BUSY;goto done;}
        uint8_t p[REC];rc=map_read(s,&m,at,p,REC);if(rc)goto done;
        Entry candidate;rc=decode_entry(s,dir,&m,at,p,&candidate);
        if(rc==IFS_FS_NOT_FOUND||rc==IFS_FS_END){rc=0;continue;}
        if(rc)goto done;
        if(s->journal_owned&&dir->identity==BB&&!strcmp(candidate.name,".ifs-journal"))continue;
        int match=name?candidate.length==len&&!memcmp(candidate.name,name,len):visible==index;
        ++visible;
        if(!match)continue;
        if(found){rc=IFS_FS_CORRUPT;goto done;}
        *out=candidate;
        found=1;
    }
    rc=found?IFS_FS_OK:name?IFS_FS_NOT_FOUND:IFS_FS_END;
done:
    map_drop(s,&m);return rc;
}
static void session_drop(Session *s)
{
    map_drop(s,&s->inode_map);drop(s,s->bitmap);drop(s,s->reserved);*s=(Session){0};
}
static int discover(const IfsFsHostRuntimeV2 *h,Session *s)
{
    *s=(Session){0};
    if(!ifs_fs_host_v2_valid(h))return IFS_FS_INVALID;
    if(h->volume_bytes<4u*BB||h->volume_bytes%BB||h->volume_bytes/BB>UINT32_MAX)
        return IFS_FS_UNSUPPORTED;
    size_t copy=h->structure_bytes<sizeof s->host?h->structure_bytes:sizeof s->host;
    memcpy(&s->host,h,copy);s->blocks=(uint32_t)(h->volume_bytes/BB);
    uint8_t super[BB];int rc=rd(s,BB,super,BB);if(rc)return rc;
    if(!name_is(super,16,"/"))return IFS_FS_NOT_FOUND;
    memcpy(s->root.raw,super,REC);s->root.identity=BB;
    if(mode(&s->root)!=DIR||!size(&s->root))return IFS_FS_CORRUPT;
    rc=node_valid(&s->root);if(rc)return rc;
    memcpy(s->inodes.raw,super+REC,REC);s->inodes.identity=BB+REC;
    if(!name_is(s->inodes.raw,16,".inodes")||mode(&s->inodes)!=REG||
       size(&s->inodes)<REC)return IFS_FS_CORRUPT;
    rc=node_valid(&s->inodes);if(rc)return rc;
    rc=node_map(s,&s->inodes,&s->inode_map);if(rc)goto fail;
    uint8_t signature[16];
    rc=map_read(s,&s->inode_map,0,signature,sizeof signature);if(rc)goto fail;
    if(memcmp(signature,"IamTHE.inodeFILE",16)){rc=IFS_FS_CORRUPT;goto fail;}
    Entry bitmap;
    rc=directory_entry(s,&s->root,".bitmap",7,0,&bitmap);if(rc)goto fail;
    uint32_t bytes=(uint32_t)(((uint64_t)s->blocks+7u)/8u);
    if(mode(&bitmap.node)!=REG||size(&bitmap.node)<bytes||
       size(&bitmap.node)>((uint64_t)bytes+511u)/BB*BB){rc=IFS_FS_CORRUPT;goto fail;}
    uint8_t *bits=alloc(s,bytes);if(!bits){rc=IFS_FS_NO_MEMORY;goto fail;}
    rc=read_node(s,&bitmap.node,0,bits,bytes);
    if(rc){drop(s,bits);goto fail;}
    s->bitmap=bits;
    if(!bit(bits,0)||!bit(bits,1)){rc=IFS_FS_CORRUPT;goto fail;}
    /* Reject bitmap padding that falsely advertises unavailable blocks. */
    if(s->blocks%8u && ((uint32_t)bits[bytes-1u]>>(s->blocks%8u))!=(255u>>(s->blocks%8u))){
        rc=IFS_FS_CORRUPT;goto fail;
    }
    Map verify={0};
    rc=node_map(s,&s->root,&verify);map_drop(s,&verify);if(rc)goto fail;
    rc=node_map(s,&s->inodes,&verify);map_drop(s,&verify);if(rc)goto fail;
    rc=node_map(s,&bitmap.node,&verify);
    if(!rc && (verify.count!=1||verify.run[0].first!=2u||
        verify.run[0].count!=(uint32_t)(((uint64_t)s->blocks+4095u)/4096u)))
        rc=IFS_FS_CORRUPT;
    map_drop(s,&verify);if(rc)goto fail;
    for(uint32_t b=0;b<s->blocks;b++){
        if((b&4095u)==0&&cancelled(s)){rc=IFS_FS_BUSY;goto fail;}
        if(!bit(bits,b))s->free+=BB;
    }
    return IFS_FS_OK;
fail:
    session_drop(s);return rc;
}
#include "journal.h"
static int check_native(Session *s);
static int probe(const IfsFsHostRuntimeV2 *h,uint32_t *variant)
{
    if(!variant)return IFS_FS_INVALID;
    *variant=0;Session s;int rc=discover(h,&s);
    if(!rc){*variant=4;session_drop(&s);}return rc;
}
static void info(const Session *s,IfsFsVolumeInfoV2 *o)
{
    *o=(IfsFsVolumeInfoV2){.structure_bytes=sizeof *o,.variant=4,
        .block_bytes=BB,.max_name_bytes=48,.total_bytes=(uint64_t)s->blocks*BB,.free_bytes=s->free};
}
static int mount_impl(const IfsFsHostRuntimeV2 *h,void **v,IfsFsVolumeInfoV2 *o)
{
    if(!v||!o||o->structure_bytes<sizeof *o)return IFS_FS_INVALID;
    *v=NULL;if(active.token||generation==UINTPTR_MAX)return IFS_FS_BUSY;
    if(!ifs_fs_host_v2_valid(h))return IFS_FS_INVALID;
    int writable=!(h->capabilities&IFS_FS_HOST_READ_ONLY),owned=0;
    uint64_t recovered=0;
    if(journal_supported(h)){
        Journal j;int jr=journal_load(h,&j);
        uint8_t owner[REC];
        int rc=ifs_fs_host_read_bytes_v2(h,journal_owner_offset(h),owner,REC);
        if(rc)return rc;
        owned=journal_owner_valid(h,owner);
        if(!jr&&j.count){
            /* Validate the staged root and journal owner before touching
             * any home block, even when the primary is unreadable. */
            if(!writable)return owned?IFS_FS_BUSY:IFS_FS_CORRUPT;
            rc=journal_replay(h,&j);if(rc)return rc;
            owned=1;recovered=1;
        }else if(owned&&jr)return jr==IFS_FS_NOT_FOUND?IFS_FS_CORRUPT:jr;
        else if(!owned&&!jr)return IFS_FS_CORRUPT;
    }
    if(writable&&!owned)return IFS_FS_ACCESS;
    int rc=discover(h,&active);if(rc)return rc;
    active.journal_owned=owned;active.writable=writable;active.recovered=recovered;
    if(writable){rc=check_native(&active);if(rc){session_drop(&active);return rc;}}
    active.token=++generation;*v=(void*)active.token;info(&active,o);return IFS_FS_OK;
}
static int unmount_impl(void *v){if(!v||(uintptr_t)v!=active.token)return IFS_FS_INVALID;reloc_drop();session_drop(&active);return IFS_FS_OK;}
static int resolve(const char *path,Entry *e)
{
    if(!path||*path!='/')return IFS_FS_INVALID;
    *e=(Entry){.node=active.root,.name="/",.length=1};++path;
    for(unsigned depth=0;*path;depth++){
        if(depth>=256)return IFS_FS_INVALID;
        uint32_t n=0;while(n<=48&&path[n]&&path[n]!='/')++n;
        if(!n||n>48||(n==1&&path[0]=='.')||(n==2&&path[0]=='.'&&path[1]=='.'))return IFS_FS_INVALID;
        Node dir=e->node;int rc=directory_entry(&active,&dir,path,n,0,e);if(rc)return rc;
        path+=n;if(*path=='/'){++path;if(!*path)return IFS_FS_INVALID;}
    }
    return IFS_FS_OK;
}
static int emit(const Entry *e,IfsFsEntryV2 *o)
{
    if(!o||o->structure_bytes<sizeof *o||!o->name)return IFS_FS_INVALID;
    if(o->name_capacity<=e->length)return IFS_FS_NO_SPACE;
    uint32_t type=mode(&e->node)==DIR?IFS_FS_ENTRY_DIRECTORY:mode(&e->node)==REG?IFS_FS_ENTRY_FILE:IFS_FS_ENTRY_LINK;
    char *name=o->name;uint32_t cap=o->name_capacity;
    *o=(IfsFsEntryV2){.structure_bytes=sizeof *o,.name=name,.name_capacity=cap,.name_bytes=e->length,.type=type,.size_bytes=size(&e->node)};
    memcpy(name,e->name,e->length+1u);return IFS_FS_OK;
}
static int stat_impl(void *v,const char *path,IfsFsEntryV2 *o)
{if(!live(v))return IFS_FS_INVALID;Entry e;int rc=resolve(path,&e);return rc?rc:emit(&e,o);}
static int list_impl(void *v,const char *path,uint32_t index,IfsFsEntryV2 *o)
{
    if(!live(v))return IFS_FS_INVALID;
    Entry e;int rc=resolve(path,&e);if(rc)return rc;
    Node dir=e.node;rc=directory_entry(&active,&dir,NULL,0,index,&e);return rc?rc:emit(&e,o);
}
static int read_impl(void *v,const char *path,uint64_t at,void *p,uint32_t n)
{
    if(!live(v))return IFS_FS_INVALID;
    Entry e;int rc=resolve(path,&e);if(rc)return rc;
    if(mode(&e.node)!=REG)return IFS_FS_UNSUPPORTED;
    return read_node(&active,&e.node,at,p,n);
}
static int map_impl(void *v,const char *path,uint64_t first,IfsFsExtentV2 *o,uint32_t cap,uint32_t *n)
{
    if(!n)return IFS_FS_INVALID;
    *n=0;if(!live(v)||!o||!cap)return IFS_FS_INVALID;
    Entry e;int rc=resolve(path,&e);if(rc)return rc;
    Map m;rc=node_map(&active,&e.node,&m);if(rc)return rc;
    uint64_t logical=0,index=0,bytes=size(&e.node);
    for(uint32_t i=0;i<m.count&&logical<bytes;){
        uint64_t physical=(uint64_t)m.run[i].first*BB,length=(uint64_t)m.run[i].count*BB;
        ++i;while(i<m.count&&physical+length==(uint64_t)m.run[i].first*BB)
            length+=(uint64_t)m.run[i++].count*BB;
        if(length>bytes-logical)length=bytes-logical;
        if(index++>=first){
            o[(*n)++]=(IfsFsExtentV2){.logical_offset=logical,.physical_offset=physical,.length=length};
            if(*n==cap)break;
        }
        logical+=length;
    }
    map_drop(&active,&m);return *n?IFS_FS_OK:IFS_FS_END;
}
static uint32_t allocation_state(uint32_t b)
{
    if(b<2u||(active.reserved&&bit(active.reserved,b)))return IFS_FS_SPACE_RESERVED;
    return bit(active.bitmap,b)?IFS_FS_SPACE_ALLOCATED:IFS_FS_SPACE_FREE;
}
static int allocation_impl(void *v,uint64_t at,IfsFsAllocationRangeV2 *o,uint32_t cap,uint32_t *n)
{
    if(!n)return IFS_FS_INVALID;
    *n=0;if(!live(v)||!o||!cap||at%BB||at>(uint64_t)active.blocks*BB)return IFS_FS_INVALID;
    if(active.journal_owned&&!active.reserved){
        int rc=check_native(&active);if(rc)return rc;
    }
    uint32_t b=(uint32_t)(at/BB);
    while(b<active.blocks&&*n<cap){
        if(cancelled(&active)){*n=0;return IFS_FS_BUSY;}
        uint32_t first=b,state=allocation_state(b);
        do {++b;}while(b<active.blocks&&allocation_state(b)==state);
        o[(*n)++]=(IfsFsAllocationRangeV2){.physical_offset=(uint64_t)first*BB,.length=(uint64_t)(b-first)*BB,.state=state};
    }
    return *n?IFS_FS_OK:IFS_FS_END;
}
#include "check.h"
#include "format.h"
#include "mutation.h"
#include "payload.h"
#include "relocation.h"
static int recover_impl(void *v,uint32_t flags,uint64_t *n)
{
    if(!n)return IFS_FS_INVALID;
    *n=0;if(!live(v)||flags)return IFS_FS_INVALID;
    if(!active.writable)return IFS_FS_ACCESS;
    *n=active.recovered;active.recovered=0;return IFS_FS_OK;
}
static int sync_impl(void *v)
{if(!live(v))return IFS_FS_INVALID;return active.writable?jflush(&active.host):IFS_FS_OK;}
static int query_impl(void *v,IfsFsVolumeInfoV2 *o)
{if(!live(v)||!o||o->structure_bytes<sizeof *o)return IFS_FS_INVALID;info(&active,o);return IFS_FS_OK;}
#define WRAP(name,args,call) static int name args {if(!enter())return IFS_FS_BUSY;return leave(call);}
WRAP(mount,(const IfsFsHostRuntimeV2 *h,void **v,IfsFsVolumeInfoV2 *o),mount_impl(h,v,o))
WRAP(unmount,(void *v),unmount_impl(v))
WRAP(stat_path,(void *v,const char *p,IfsFsEntryV2 *o),stat_impl(v,p,o))
WRAP(list_path,(void *v,const char *p,uint32_t i,IfsFsEntryV2 *o),list_impl(v,p,i,o))
WRAP(read_path,(void *v,const char *p,uint64_t at,void *b,uint32_t n),read_impl(v,p,at,b,n))
WRAP(map_path,(void *v,const char *p,uint64_t at,IfsFsExtentV2 *o,uint32_t c,uint32_t *n),map_impl(v,p,at,o,c,n))
WRAP(allocation_map,(void *v,uint64_t at,IfsFsAllocationRangeV2 *o,uint32_t c,uint32_t *n),allocation_impl(v,at,o,c,n))
WRAP(query,(void *v,IfsFsVolumeInfoV2 *o),query_impl(v,o))
WRAP(format,(const IfsFsHostRuntimeV2 *h,uint32_t variant,const char *label,uint32_t flags),format_impl(h,variant,label,flags))
WRAP(check,(void *v,uint32_t flags,uint64_t *errors),check_impl(v,flags,errors))
WRAP(create_path,(void *v,const char *p,uint32_t type),create_impl(v,p,type))
WRAP(write_path,(void *v,const char *p,uint64_t at,const void *b,uint32_t n),write_impl(v,p,at,b,n))
WRAP(truncate_path,(void *v,const char *p,uint64_t n),truncate_impl(v,p,n))
WRAP(remove_path,(void *v,const char *p),remove_impl(v,p))
WRAP(rename_path,(void *v,const char *p,const char *q),rename_impl(v,p,q))
WRAP(plan,(void *v,const char *p,uint32_t policy,void **out),reloc_plan_impl(v,p,policy,out))
WRAP(execute,(void *v,void *p,uint64_t *moved),reloc_execute_impl(v,p,moved))
WRAP(verify,(void *v,void *p),reloc_verify_impl(v,p))
static void release_plan(void *v,void *p){if(enter()){reloc_release_impl(v,p);(void)leave(0);}}
WRAP(recover,(void *v,uint32_t flags,uint64_t *n),recover_impl(v,flags,n))
WRAP(sync_volume,(void *v),sync_impl(v))
static const IfsFsCoreOpsV2 core={.header={2,sizeof core,IFS_FS_GROUP_CORE,0,IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT|IFS_FS_CORE_STAT|IFS_FS_CORE_LIST|IFS_FS_CORE_READ|IFS_FS_CORE_WRITE|IFS_FS_CORE_CREATE|IFS_FS_CORE_REMOVE|IFS_FS_CORE_RENAME|IFS_FS_CORE_TRUNCATE},
.probe=probe,.mount=mount,.unmount=unmount,.stat=stat_path,.directory_entry=list_path,.read=read_path,.create=create_path,.remove=remove_path,.rename=rename_path,.write=write_path,.truncate=truncate_path};
static const IfsFsVolumeOpsV2 volume={.header={2,sizeof volume,IFS_FS_GROUP_VOLUME,0,IFS_FS_VOLUME_QUERY|IFS_FS_VOLUME_FORMAT|IFS_FS_VOLUME_CHECK|IFS_FS_VOLUME_SYNC},.query=query,.format=format,.check=check,.sync=sync_volume};
static const IfsFsFormatProfileOpsV2 profile={.header={2,sizeof profile,IFS_FS_GROUP_FORMAT_PROFILE,0,IFS_FS_FORMAT_PROFILE_DEFAULT},.get_default=format_profile};
static const IfsFsDefragOpsV2 defrag={.header={2,sizeof defrag,IFS_FS_GROUP_DEFRAG,0,IFS_FS_DEFRAG_ALL},.map=map_path,.plan=plan,.execute=execute,.verify=verify,.recover=recover,.release_plan=release_plan};
static const IfsFsAllocationOpsV2 allocation={.header={2,sizeof allocation,IFS_FS_GROUP_ALLOCATION,0,IFS_FS_ALLOCATION_MAP},.map=allocation_map};
static const IfsFsGroupHeaderV2 *group(uint32_t id,uint32_t lo,uint32_t hi)
{
    if(lo>2||hi<2)return NULL;
    if(id==IFS_FS_GROUP_CORE)return &core.header;
    if(id==IFS_FS_GROUP_VOLUME)return &volume.header;
    if(id==IFS_FS_GROUP_DEFRAG)return &defrag.header;
    if(id==IFS_FS_GROUP_ALLOCATION)return &allocation.header;
    if(id==IFS_FS_GROUP_FORMAT_PROFILE)return &profile.header;
    return NULL;
}
__attribute__((used,section(".ifsmeta")))static const char identity[]="IFS-MODULE:qnx4:common2";
static const IfsFsModuleApiV2 api={.abi_version=2,.structure_bytes=sizeof api,.format_id="qnx4",
.runtime_profiles=IFS_FS_PROFILE_HOSTED
#ifdef IFS_BOOT_FIRMWARE
        |IFS_FS_PROFILE_FREESTANDING|IFS_FS_PROFILE_FIRMWARE
#endif
        ,.group_mask=IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE)|IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME)|IFS_FS_GROUP_BIT(IFS_FS_GROUP_DEFRAG)|IFS_FS_GROUP_BIT(IFS_FS_GROUP_ALLOCATION)|IFS_FS_GROUP_BIT(IFS_FS_GROUP_FORMAT_PROFILE),.query_group=group};
__attribute__((visibility("default")))const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void){return &api;}
