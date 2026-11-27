/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent QNX4 allocation and directory consistency reconciliation.
 * No storage mutation, no repair inference. Count each inode identity
 * once, including all extension blocks, and verify all native hard-link
 * references before accepting the allocator bitmap.
 */
typedef struct {
    Session *s;
    uint8_t *owned,*visited,*reserved;
    uint16_t *refs;
    uint32_t inode_slots;
} Check;
typedef struct {char name[49];} CheckName;
static int compare_names(const void *a,const void *b)
{return strcmp(((const CheckName*)a)->name,((const CheckName*)b)->name);}
static int inode_index(const Session *s,uint64_t physical,uint32_t *index)
{
    uint64_t logical=0;
    for(uint32_t i=0;i<s->inode_map.count;i++){
        uint64_t start=(uint64_t)s->inode_map.run[i].first*BB,n=(uint64_t)s->inode_map.run[i].count*BB;
        if(physical>=start&&physical-start<n){
            uint64_t offset=logical+physical-start;
            if(offset<REC||offset%REC||offset+REC>size(&s->inodes))return IFS_FS_CORRUPT;
            *index=(uint32_t)(offset/REC);return IFS_FS_OK;
        }
        logical+=n;
    }
    return IFS_FS_CORRUPT;
}
static int check_walk(Check *c,const Node *node,uint64_t parent_identity,unsigned depth)
{
    Session *s=c->s;
    if(depth>256u)return IFS_FS_UNSUPPORTED;
    if(cancelled(s))return IFS_FS_BUSY;
    if(node->identity%REC||node->identity>=s->host.volume_bytes)return IFS_FS_CORRUPT;
    uint64_t record=node->identity/REC;
    uint8_t mask=(uint8_t)(1u<<(record%8u));
    if(c->visited[record/8u]&mask){
        /* Multiple names may reference a file inode. Direct records and
         * directories must not be multiply reached. Reserved inode-file
         * alias was already claimed during the initial check. */
        return mode(node)!=DIR&&(node->identity==BB+REC||(node->raw[63]&INODE))?
            IFS_FS_OK:IFS_FS_CORRUPT;
    }
    c->visited[record/8u]|=mask;
    Map m;int rc=node_map_claim(s,node,&m,c->owned);if(rc)return rc;
    if(mode(node)!=DIR){
        /* QNX 4 native preallocation is part of the file's extent chain
         * and marked occupied in the bitmap (QNX KB 9712). It is not
         * visible through logical EOF. Classify actual owned tail blocks,
         * never an inferred free-space gap, on our journaled volumes. */
        if(c->reserved&&mode(node)==REG){
            uint64_t remaining=((uint64_t)size(node)+BB-1u)/BB;
            for(uint32_t i=0;i<m.count;i++){
                Run r=m.run[i];uint32_t used=remaining<r.count?(uint32_t)remaining:r.count;
                remaining-=used;
                for(uint32_t j=used;j<r.count;j++){
                    uint32_t b=r.first+j;
                    if((b&4095u)==0&&cancelled(s)){map_drop(s,&m);return IFS_FS_BUSY;}
                    c->reserved[b/8u]|=(uint8_t)(1u<<(b%8u));
                }
            }
        }
        map_drop(s,&m);return IFS_FS_OK;
    }
    uint32_t count=0,capacity=0;
    CheckName *names=NULL;
    unsigned dot=0,up=0;
    for(uint64_t at=0;at<size(node);at+=REC){
        if(cancelled(s)){rc=IFS_FS_BUSY;break;}
        uint8_t p[REC];rc=map_read(s,&m,at,p,REC);if(rc)break;
        Entry e;rc=decode_entry(s,node,&m,at,p,&e);
        if(rc==IFS_FS_NOT_FOUND){rc=0;continue;}
        if(rc==IFS_FS_END){
            if(!(p[63]&LINK)||!le32(p+48)||le32(p+48)>s->blocks||p[52]>=8){
                rc=IFS_FS_CORRUPT;break;
            }
            if(name_is(p,48,".")){
                if(dot++||at!=0||((uint64_t)le32(p+48)-1u)*BB+p[52]*REC!=node->identity)
                    {rc=IFS_FS_CORRUPT;break;}
            }else if(up++||at!=REC||
                ((uint64_t)le32(p+48)-1u)*BB+p[52]*REC!=parent_identity){
                /* A syntactically valid backlink can still point at an unrelated
                 * inode. Require the actual traversal parent before allowing
                 * a writable mount; root is its own parent. */
                rc=IFS_FS_CORRUPT;break;
            }
            rc=0;continue;
        }
        if(rc)break;
        if(count==capacity){
            uint32_t next=capacity?capacity*2u:16u;
            if(next<capacity||next>UINT32_MAX/sizeof(CheckName)){rc=IFS_FS_NO_MEMORY;break;}
            CheckName *q=alloc(s,(size_t)next*sizeof(*q));if(!q){rc=IFS_FS_NO_MEMORY;break;}
            if(names)memcpy(q,names,(size_t)count*sizeof(*q));
            drop(s,names);names=q;capacity=next;
        }
        memcpy(names[count++].name,e.name,sizeof e.name);
        if((p[63]&LINK)&&e.node.identity>=2u*BB){
            uint32_t i=0;rc=inode_index(s,e.node.identity,&i);
            if(rc||i>=c->inode_slots||c->refs[i]==UINT16_MAX){rc=IFS_FS_CORRUPT;break;}
            ++c->refs[i];
        }else if(!(p[63]&LINK)&&mode(&e.node)!=DIR&&le16(e.node.raw+56)!=1u){
            rc=IFS_FS_CORRUPT;break;
        }
        rc=check_walk(c,&e.node,node->identity,depth+1);if(rc)break;
    }
    if(!rc&&(dot!=1u||up!=1u))rc=IFS_FS_CORRUPT;
    if(!rc&&count>1u){
        qsort(names,count,sizeof(*names),compare_names);
        for(uint32_t i=1;i<count;i++)if(!strcmp(names[i-1].name,names[i].name)){
            rc=IFS_FS_CORRUPT;break;
        }
    }
    drop(s,names);map_drop(s,&m);return rc;
}
static int check_native(Session *s)
{
    size_t n=(size_t)(((uint64_t)s->blocks+7u)/8u);
    Check c={.s=s,.inode_slots=size(&s->inodes)/REC};
    if(size(&s->inodes)%REC)return IFS_FS_CORRUPT;
    c.owned=alloc(s,n);c.visited=alloc(s,s->blocks);
    c.refs=alloc(s,(size_t)c.inode_slots*sizeof(*c.refs));
    if(s->journal_owned)c.reserved=alloc(s,n);
    int rc=IFS_FS_NO_MEMORY;
    if(!c.owned||!c.visited||!c.refs||(s->journal_owned&&!c.reserved))goto done;
    if(c.reserved)memset(c.reserved,0,n);
    memset(c.owned,0,n);memset(c.visited,0,s->blocks);
    memset(c.refs,0,(size_t)c.inode_slots*sizeof(*c.refs));
    c.owned[0]=3u;
    rc=check_walk(&c,&s->inodes,s->inodes.identity,0);if(rc)goto done;
    rc=check_walk(&c,&s->root,s->root.identity,0);if(rc)goto done;
    for(uint32_t i=1;i<c.inode_slots;i++){
        if(cancelled(s)){rc=IFS_FS_BUSY;goto done;}
        uint8_t raw[REC];rc=map_read(s,&s->inode_map,(uint64_t)i*REC,raw,REC);if(rc)goto done;
        if(raw[63]&(USED|INODE)){
            if(!(raw[63]&INODE)||(raw[63]&(LINK|BUSY))||!c.refs[i]||
               c.refs[i]!=le16(raw+56)){rc=IFS_FS_CORRUPT;goto done;}
        }else if(c.refs[i]){rc=IFS_FS_CORRUPT;goto done;}
    }
    for(uint32_t b=0;b<s->blocks;b++){
        if((b&4095u)==0&&cancelled(s)){rc=IFS_FS_BUSY;goto done;}
        if(bit(c.owned,b)!=bit(s->bitmap,b)){rc=IFS_FS_CORRUPT;goto done;}
    }
    drop(s,s->reserved);s->reserved=c.reserved;c.reserved=NULL;
    rc=IFS_FS_OK;
done:
    drop(s,c.owned);drop(s,c.visited);drop(s,c.refs);drop(s,c.reserved);return rc;
}
static int check_impl(void *v,uint32_t flags,uint64_t *errors)
{
    if(!errors)return IFS_FS_INVALID;
    *errors=0;if(!live(v)||flags)return IFS_FS_INVALID;
    Session fresh;int rc=discover(&active.host,&fresh);
    if(!rc){fresh.journal_owned=active.journal_owned;rc=check_native(&fresh);session_drop(&fresh);}
    if(rc==IFS_FS_CORRUPT){*errors=1;return IFS_FS_OK;}
    return rc;
}
