/* SPDX-License-Identifier: GPL-3.0-or-later
 * QNX4 provider-private physical-page redo journal. The native
 * .ifs-journal file owns every journal block, so a foreign read-only
 * implementation sees ordinary allocated file space. Two alternating
 * checksummed headers preserve the durable state. Before replay, validate
 * every afterimage plus the native root and journal owner.
 * Transaction mechanics adapted from the project's GPL EFS implementation.
 */
enum { JUNIT=4096, JPAGES=64, JMAX=JPAGES-2, JVERSION=1 };
#define JMAGIC UINT32_C(0x324a3451) /* Q4J2 */
typedef struct {
    uint64_t base;
    uint32_t sequence,slot,count;
    uint32_t page[JMAX],crc[JMAX];
} Journal;
typedef struct {
    const IfsFsHostRuntimeV2 *h;
    Journal j;
    uint32_t count;
    uint32_t page[JMAX];
    uint8_t *bytes;
} Tx;
static uint32_t crc32(const uint8_t *p,size_t n)
{
    uint32_t c=UINT32_MAX;
    while(n--){
        c^=*p++;
        for(unsigned i=0;i<8;i++)c=(c>>1)^((0u-(c&1u))&UINT32_C(0xedb88320));
    }
    return ~c;
}
static uint32_t journal_root(const IfsFsHostRuntimeV2 *h)
{return 2u+(uint32_t)((h->volume_bytes/BB+4095u)/4096u);}
static uint64_t journal_base(const IfsFsHostRuntimeV2 *h)
{return ((uint64_t)(journal_root(h)+4u+16u)*BB+JUNIT-1u)&~(uint64_t)(JUNIT-1u);}
static uint32_t journal_first(const IfsFsHostRuntimeV2 *h)
{return (uint32_t)(journal_base(h)/BB);}
static uint64_t journal_owner_offset(const IfsFsHostRuntimeV2 *h)
{return (uint64_t)journal_root(h)*BB+6u*REC;}
static int journal_owner_valid(const IfsFsHostRuntimeV2 *h,const uint8_t *p)
{
    return name_is(p,16,".ifs-journal")&&(p[63]&USED)&&!(p[63]&(LINK|BUSY))&&
        le32(p+16)==JPAGES*JUNIT&&le32(p+20)==journal_first(h)+1u&&
        le32(p+24)==JPAGES*JUNIT/BB&&!le32(p+28)&&le16(p+48)==1u&&
        (le16(p+50)&0170000u)==REG&&le16(p+56)==1u;
}
static int journal_supported(const IfsFsHostRuntimeV2 *h)
{
    return h->volume_bytes>=1024u*1024u && h->volume_bytes<=UINT64_C(0xffffffff)*BB &&
        h->physical_sector_bytes<=JUNIT && h->logical_sector_bytes<=JUNIT &&
        h->volume_bytes%JUNIT==0 && journal_base(h)+JPAGES*JUNIT<h->volume_bytes;
}
static int jread(const IfsFsHostRuntimeV2 *h,uint64_t at,void *p)
{return ifs_fs_host_read_bytes_v2(h,at,p,JUNIT);}
static int jwrite(const IfsFsHostRuntimeV2 *h,uint64_t at,const void *p)
{return ifs_fs_host_write_bytes_v2(h,at,p,JUNIT);}
static int jflush(const IfsFsHostRuntimeV2 *h)
{return h->flush(h->context)?IFS_FS_IO_ERROR:IFS_FS_OK;}
static int target_ok(const IfsFsHostRuntimeV2 *h,uint64_t base,uint32_t page)
{
    uint64_t at=(uint64_t)page*JUNIT;
    return at<h->volume_bytes && JUNIT<=h->volume_bytes-at &&
        (at<base || at>=base+JPAGES*JUNIT);
}
static int journal_decode(const IfsFsHostRuntimeV2 *h,const uint8_t *p,uint32_t slot,Journal *j)
{
    uint64_t base=journal_base(h);
    if(le32(p)!=JMAGIC)return IFS_FS_NOT_FOUND;
    if(le32(p+4)!=JVERSION || le32(p+8)!=(uint32_t)(h->volume_bytes/BB) ||
       le32(p+12)!=journal_first(h) || le32(p+16)==0 ||
       le32(p+20)>JMAX || le32(p+JUNIT-4)!=crc32(p,JUNIT-4))
        return IFS_FS_CORRUPT;
    *j=(Journal){.base=base,.sequence=le32(p+16),.slot=slot,.count=le32(p+20)};
    for(uint32_t i=0;i<j->count;i++){
        j->page[i]=le32(p+24+8u*i);j->crc[i]=le32(p+28+8u*i);
        if(!target_ok(h,base,j->page[i]))return IFS_FS_CORRUPT;
        for(uint32_t k=0;k<i;k++)if(j->page[k]==j->page[i])return IFS_FS_CORRUPT;
    }
    return IFS_FS_OK;
}
static int journal_load(const IfsFsHostRuntimeV2 *h,Journal *j)
{
    if(!journal_supported(h))return IFS_FS_NOT_FOUND;
    uint8_t p[JUNIT];Journal a[2];int status[2];
    uint64_t base=journal_base(h);
    for(uint32_t i=0;i<2;i++){
        int rc=jread(h,base+(uint64_t)i*JUNIT,p);if(rc)return rc;
        status[i]=journal_decode(h,p,i,&a[i]);
    }
    if(status[0] && status[1])
        return status[0]==IFS_FS_NOT_FOUND && status[1]==IFS_FS_NOT_FOUND?
            IFS_FS_NOT_FOUND:IFS_FS_CORRUPT;
    if(!status[0] && !status[1]){
        uint32_t hi=a[0].sequence>a[1].sequence?0u:1u;
        uint32_t lo=hi^1u;
        if(a[hi].sequence-a[lo].sequence!=1u ||
           (!!a[hi].count)==(!!a[lo].count))return IFS_FS_CORRUPT;
        *j=a[hi];
    }else *j=a[status[0]?1:0];
    return IFS_FS_OK;
}
static int journal_header(const IfsFsHostRuntimeV2 *h,const Journal *j)
{
    uint8_t p[JUNIT]={0};
    put32(p,JMAGIC);put32(p+4,JVERSION);
    put32(p+8,(uint32_t)(h->volume_bytes/BB));put32(p+12,journal_first(h));
    put32(p+16,j->sequence);put32(p+20,j->count);
    for(uint32_t i=0;i<j->count;i++){
        put32(p+24+8u*i,j->page[i]);put32(p+28+8u*i,j->crc[i]);
    }
    put32(p+JUNIT-4,crc32(p,JUNIT-4));
    int rc=jwrite(h,j->base+(uint64_t)j->slot*JUNIT,p);
    return rc?rc:jflush(h);
}
static int journal_init(const IfsFsHostRuntimeV2 *h)
{
    uint8_t p[JUNIT]={0};
    Journal j={.base=journal_base(h),.sequence=1,.slot=0};
    /* Invalidate both old headers before making this newly formatted
     * volume's journal usable. The primary superblock is still invalid. */
    int rc=jwrite(h,j.base,p);if(rc)return rc;
    rc=jwrite(h,j.base+JUNIT,p);if(rc)return rc;
    rc=jflush(h);return rc?rc:journal_header(h,&j);
}
static int journal_replay(const IfsFsHostRuntimeV2 *h,Journal *j)
{
    if(!j->count)return IFS_FS_OK;
    if(h->capabilities&IFS_FS_HOST_READ_ONLY)return IFS_FS_BUSY;
    if(j->sequence==UINT32_MAX)return IFS_FS_CORRUPT;
    uint8_t *data=h->allocate(h->context,(size_t)j->count*JUNIT);
    if(!data)return IFS_FS_NO_MEMORY;
    int rc=IFS_FS_OK;
    for(uint32_t i=0;i<j->count;i++){
        uint8_t *p=data+(size_t)i*JUNIT;
        rc=jread(h,j->base+(uint64_t)(i+2u)*JUNIT,p);
        if(rc)goto done;
        if(crc32(p,JUNIT)!=j->crc[i]){rc=IFS_FS_CORRUPT;goto done;}
    }
    /* Every transaction includes root block page zero. Validate the
     * native geometry and fixed journal owner in the afterimage (or in
     * its untouched home record) before applying ANY home page. */
    int primary=0,owner_found=0;
    uint8_t owner[REC];uint64_t owner_at=journal_owner_offset(h);
    for(uint32_t i=0;i<j->count;i++){
        const uint8_t *p=data+(size_t)i*JUNIT;
        if(j->page[i]==0){
            const uint8_t *root=p+BB;
            if(!name_is(root,16,"/")||(le16(root+50)&0170000u)!=DIR||
               le32(root+20)!=journal_root(h)+1u||le32(root+24)<4u){
                rc=IFS_FS_CORRUPT;goto done;
            }
            primary=1;
        }
        if(j->page[i]==owner_at/JUNIT){
            memcpy(owner,p+owner_at%JUNIT,REC);owner_found=1;
        }
    }
    if(!primary){rc=IFS_FS_CORRUPT;goto done;}
    if(!owner_found){rc=ifs_fs_host_read_bytes_v2(h,owner_at,owner,REC);if(rc)goto done;}
    if(!journal_owner_valid(h,owner)){rc=IFS_FS_CORRUPT;goto done;}
    for(uint32_t i=0;i<j->count;i++){
        rc=jwrite(h,(uint64_t)j->page[i]*JUNIT,data+(size_t)i*JUNIT);
        if(rc)goto done;
    }
    rc=jflush(h);if(rc)goto done;
    Journal clean={.base=j->base,.sequence=j->sequence+1u,.slot=j->slot^1u};
    rc=journal_header(h,&clean);
    if(!rc)*j=clean;
done:
    h->release(h->context,data);return rc;
}
static int tx_begin(Tx *t,const IfsFsHostRuntimeV2 *h)
{
    *t=(Tx){.h=h};
    int rc=journal_load(h,&t->j);
    if(rc)return rc;
    if(t->j.count)return IFS_FS_BUSY;
    if(t->j.sequence>UINT32_MAX-2u)return IFS_FS_NO_SPACE;
    t->bytes=h->allocate(h->context,(size_t)JMAX*JUNIT);
    return t->bytes?IFS_FS_OK:IFS_FS_NO_MEMORY;
}
static void tx_end(Tx *t)
{if(t->bytes)t->h->release(t->h->context,t->bytes);t->bytes=NULL;}
static int tx_stage(Tx *t,uint64_t at,const void *data,uint32_t n)
{
    const uint8_t *src=data;
    if(at>t->h->volume_bytes || n>t->h->volume_bytes-at)return IFS_FS_INVALID;
    while(n){
        uint32_t page=(uint32_t)(at/JUNIT),off=(uint32_t)(at%JUNIT),i;
        if(!target_ok(t->h,t->j.base,page))return IFS_FS_INVALID;
        for(i=0;i<t->count;i++)if(t->page[i]==page)break;
        if(i==t->count){
            if(i==JMAX)return IFS_FS_NO_SPACE;
            int rc=jread(t->h,(uint64_t)page*JUNIT,t->bytes+(size_t)i*JUNIT);
            if(rc)return rc;
            t->page[t->count++]=page;
        }
        uint32_t take=JUNIT-off;if(take>n)take=n;
        memcpy(t->bytes+(size_t)i*JUNIT+off,src,take);
        at+=take;src+=take;n-=take;
    }
    return IFS_FS_OK;
}
static int tx_commit(Tx *t)
{
    if(!t->count)return IFS_FS_INVALID;
    Journal prepare={.base=t->j.base,.slot=t->j.slot^1u,
        .sequence=t->j.sequence+1u,.count=t->count};
    int primary=0;
    for(uint32_t i=0;i<t->count;i++){
        prepare.page[i]=t->page[i];
        prepare.crc[i]=crc32(t->bytes+(size_t)i*JUNIT,JUNIT);
        if(!t->page[i])primary=1;
    }
    if(!primary)return IFS_FS_INVALID;
    for(uint32_t i=0;i<t->count;i++){
        int rc=jwrite(t->h,prepare.base+(uint64_t)(i+2u)*JUNIT,t->bytes+(size_t)i*JUNIT);
        if(rc)return rc;
    }
    int rc=jflush(t->h);if(rc)return rc;
    rc=journal_header(t->h,&prepare);if(rc)return rc;
    return journal_replay(t->h,&prepare);
}
