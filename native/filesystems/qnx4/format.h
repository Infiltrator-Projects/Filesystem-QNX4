/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native nonbootable QNX4 format. Source reference: QNX 4.25 User Guide
 * disk structure, recovery.html, and QNX4 root record layout. No external
 * formatter or kernel filesystem calls. Publish the root block last.
 * A failed format is destructive and does not promise old-volume rollback.
 * Native owned journal enables recoverable provider-specific writes.
 */
#define FORMAT_MIN (UINT64_C(1024)*1024)
#define FORMAT_MAX (UINT64_C(0xfffffff8)*BB)
static void make_node(uint8_t *p,const char *name,uint32_t bytes,uint32_t block,uint32_t count,
                      uint32_t mode,uint32_t links)
{
    memset(p,0,REC);memcpy(p,name,strlen(name));
    put32(p+16,bytes);put32(p+20,count?block+1u:0);put32(p+24,count);
    put16(p+48,count?1:0);put16(p+50,mode);put16(p+56,links);
    p[63]=USED|32u;
}
static void make_link(uint8_t *p,const char *name,uint32_t block,uint8_t slot)
{
    memset(p,0,REC);memcpy(p,name,strlen(name));put32(p+48,block+1u);p[52]=slot;p[63]=LINK|32u;
}
static int format_profile(IfsFsFormatProfileV2 *p)
{
    if(!p||p->structure_bytes<sizeof *p)return IFS_FS_INVALID;
    *p=(IfsFsFormatProfileV2){.structure_bytes=sizeof *p,.flags=IFS_FS_FORMAT_REQUIRE_CHECK,.format_variant=4,.probe_variant=4,
        .exact_block_bytes=BB,.layout=IFS_FS_LAYOUT_MULTI_EXTENT,
        .minimum_bytes=FORMAT_MIN,.recommended_bytes=16u*1024u*1024u,.maximum_bytes=FORMAT_MAX};
    return IFS_FS_OK;
}
static int format_impl(const IfsFsHostRuntimeV2 *h,uint32_t variant,const char *label,uint32_t flags)
{
    if(!ifs_fs_host_v2_valid(h))return IFS_FS_INVALID;
    if(h->capabilities&IFS_FS_HOST_READ_ONLY)return IFS_FS_ACCESS;
    if(active.token)return IFS_FS_BUSY;
    if(variant!=4||flags||h->volume_bytes<FORMAT_MIN||h->volume_bytes>FORMAT_MAX||
       !journal_supported(h))return IFS_FS_UNSUPPORTED;
    if(label&&*label)return IFS_FS_UNSUPPORTED;
    uint32_t blocks=(uint32_t)(h->volume_bytes/BB),bm=(uint32_t)(((uint64_t)blocks+4095u)/4096u);
    uint32_t root=2u+bm,inodes=root+4u,end=inodes+16u;
    if(end>=blocks)return IFS_FS_UNSUPPORTED;
    uint8_t p[BB]={0},super[BB]={0};
    /* Invalidate the root and loader. No boot program is installed. */
    if(h->cancelled&&h->cancelled(h->context))return IFS_FS_BUSY;
    int rc=ifs_fs_host_write_bytes_v2(h,BB,p,BB);if(rc)return rc;
    if(h->flush(h->context))return IFS_FS_IO_ERROR;
    rc=ifs_fs_host_write_bytes_v2(h,0,p,BB);if(rc)return rc;
    for(uint32_t i=0;i<bm;i++){
        if(h->cancelled&&h->cancelled(h->context))return IFS_FS_BUSY;
        memset(p,0,BB);
        for(uint32_t bit=0;bit<4096u;bit++){
            uint64_t b=(uint64_t)i*4096u+bit;
            if(b<end||b>=blocks||
               (b>=journal_first(h)&&b<journal_first(h)+JPAGES*JUNIT/BB))
                p[bit/8u]|=(uint8_t)(1u<<(bit%8u));
        }
        rc=ifs_fs_host_write_bytes_v2(h,(uint64_t)(2u+i)*BB,p,BB);if(rc)return rc;
    }
    memset(p,0,BB);
    make_link(p,".",1,0);memcpy(p+16,"I\003QNX",5);
    make_link(p+REC,"..",1,0);
    make_node(p+2*REC,".bitmap",bm*BB,2,bm,0100400u,1);
    make_link(p+3*REC,".inodes",1,1);
    make_link(p+4*REC,".boot",1,2);
    make_link(p+5*REC,".altboot",1,3);
    make_node(p+6*REC,".ifs-journal",JPAGES*JUNIT,journal_first(h),JPAGES*JUNIT/BB,0100600u,1);
    rc=ifs_fs_host_write_bytes_v2(h,(uint64_t)root*BB,p,BB);if(rc)return rc;
    memset(p,0,BB);
    for(uint32_t i=1;i<4;i++){
        rc=ifs_fs_host_write_bytes_v2(h,(uint64_t)(root+i)*BB,p,BB);if(rc)return rc;
    }
    memcpy(p,"IamTHE.inodeFILE",16);
    rc=ifs_fs_host_write_bytes_v2(h,(uint64_t)inodes*BB,p,BB);if(rc)return rc;
    memset(p,0,BB);
    for(uint32_t i=1;i<16;i++){
        rc=ifs_fs_host_write_bytes_v2(h,(uint64_t)(inodes+i)*BB,p,BB);if(rc)return rc;
    }
    rc=journal_init(h);if(rc)return rc;
    make_node(super,"/",4u*BB,root,4,0040755u,2);
    make_node(super+REC,".inodes",16u*BB,inodes,16,0100600u,1);
    make_node(super+2*REC,".boot",0,0,0,0100400u,1);
    make_node(super+3*REC,".altboot",0,0,0,0100400u,1);
    if(h->flush(h->context))return IFS_FS_IO_ERROR;
    if(h->cancelled&&h->cancelled(h->context))return IFS_FS_BUSY;
    rc=ifs_fs_host_write_bytes_v2(h,BB,super,BB);
    if(!rc&&h->flush(h->context))rc=IFS_FS_IO_ERROR;
    if(rc){(void)ifs_fs_host_write_bytes_v2(h,BB,p,BB);(void)h->flush(h->context);return rc;}
    Session verify;rc=discover(h,&verify);
    if(!rc){rc=check_native(&verify);session_drop(&verify);}
    return rc;
}
