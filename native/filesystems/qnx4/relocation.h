/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native QNX4 COW compaction. Plan the entire requested subtree without
 * writes, preserving stable inode identities and deduplicating hard links.
 * Each relocation publishes one native inode and bitmap transaction. A
 * cold recovery may retain completed moves but never a half-moved file.
 * Growth uses native QNX extent preallocation (QNX KB 9712): extra
 * file-owned blocks beyond logical EOF, persistently occupied in bitmap.
 */
typedef struct {
    Node original;
    uint64_t digest;
    uint32_t target,reserve;
    int move,reserve_only;
} RelocFile;
typedef struct {
    uintptr_t token;
    RelocFile *files;
    size_t count,capacity,next;
    uint64_t moved;
    int complete;
} RelocPlan;
static RelocPlan *reloc_plan;
static uintptr_t reloc_generation;
static void reloc_drop(void)
{
    if(reloc_plan){drop(&active,reloc_plan->files);drop(&active,reloc_plan);}
    reloc_plan=NULL;active_plan=0;
}
static int reloc_digest(const Node *node,const Map *map,uint64_t *out)
{
    uint64_t digest=UINT64_C(14695981039346656037);
    uint32_t left=size(node);
    uint8_t p[16384];
    for(uint32_t i=0;i<map->count&&left;i++){
        uint64_t at=(uint64_t)map->run[i].first*BB;
        uint64_t run=(uint64_t)map->run[i].count*BB;
        if(run>left)run=left;
        while(run){
            if(cancelled(&active))return IFS_FS_BUSY;
            uint32_t n=run>sizeof p?sizeof p:(uint32_t)run;
            int rc=rd(&active,at,p,n);if(rc)return rc;
            for(uint32_t j=0;j<n;j++)digest=(digest^p[j])*UINT64_C(1099511628211);
            at+=n;run-=n;left-=n;
        }
    }
    if(left)return IFS_FS_CORRUPT;
    *out=digest;return IFS_FS_OK;
}
static int reloc_append(RelocPlan *p,const Node *node)
{
    if(p->count==p->capacity){
        size_t n=p->capacity?p->capacity*2u:32u;
        if(n<p->capacity||n>SIZE_MAX/sizeof(RelocFile))return IFS_FS_NO_MEMORY;
        RelocFile *f=alloc(&active,n*sizeof *f);if(!f)return IFS_FS_NO_MEMORY;
        if(p->count)memcpy(f,p->files,p->count*sizeof *f);
        drop(&active,p->files);p->files=f;p->capacity=n;
    }
    p->files[p->count++]=(RelocFile){.original=*node};
    return IFS_FS_OK;
}
static int reloc_collect(RelocPlan *p,uint8_t *seen,const Node *node,unsigned depth)
{
    if(depth>256u)return IFS_FS_UNSUPPORTED;
    if(cancelled(&active))return IFS_FS_BUSY;
    if(node->identity%REC||node->identity>=active.host.volume_bytes)return IFS_FS_CORRUPT;
    uint64_t record=node->identity/REC;
    uint8_t mask=(uint8_t)(1u<<(record%8u));
    if(seen[record/8u]&mask)return IFS_FS_OK;
    seen[record/8u]|=mask;
    if(mode(node)==REG)return reloc_append(p,node);
    if(mode(node)!=DIR)return IFS_FS_OK;
    Map map;int rc=node_map(&active,node,&map);if(rc)return rc;
    for(uint64_t at=0;at<size(node);at+=REC){
        if(cancelled(&active)){rc=IFS_FS_BUSY;break;}
        uint8_t raw[REC];Entry e;
        rc=map_read(&active,&map,at,raw,REC);if(rc)break;
        rc=decode_entry(&active,node,&map,at,raw,&e);
        if(rc==IFS_FS_END||rc==IFS_FS_NOT_FOUND){rc=0;continue;}
        if(rc)break;
        if(protected_name(node,e.name))continue;
        rc=reloc_collect(p,seen,&e.node,depth+1u);if(rc)break;
    }
    map_drop(&active,&map);return rc;
}
static int reloc_contiguous(const Map *map,uint32_t blocks,uint32_t *start)
{
    *start=map->count?map->run[0].first:0;
    uint64_t end=*start;
    for(uint32_t i=0;i<map->count;i++){
        if(map->run[i].first!=end)return 0;
        end+=map->run[i].count;
    }
    return end-*start==blocks;
}
static int reloc_choose(const uint8_t *bitmap,uint32_t count,uint32_t *first)
{
    uint32_t n=0;
    for(uint32_t b=2;b<active.blocks;b++){
        if((b&4095u)==0&&cancelled(&active))return IFS_FS_BUSY;
        n=bit(bitmap,b)?0:n+1u;
        if(n==count){*first=b+1u-count;return IFS_FS_OK;}
    }
    return IFS_FS_NO_SPACE;
}
static int reloc_free(const uint8_t *bitmap,uint32_t start,uint32_t count)
{
    if(start>active.blocks||count>active.blocks-start)return 0;
    for(uint32_t i=0;i<count;i++)if(bit(bitmap,start+i))return 0;
    return 1;
}
static void reloc_sim_bits(uint8_t *bitmap,uint32_t start,uint32_t count,int used)
{
    for(uint32_t i=0;i<count;i++){
        uint32_t b=start+i;uint8_t mask=(uint8_t)(1u<<(b%8u));
        if(used)bitmap[b/8u]|=mask;else bitmap[b/8u]&=(uint8_t)~mask;
    }
}
static int reloc_simulate(RelocFile *f,uint8_t *bitmap,uint32_t policy)
{
    Map map;int rc=node_map(&active,&f->original,&map);if(rc)return rc;
    rc=reloc_digest(&f->original,&map,&f->digest);if(rc)goto done;
    uint32_t blocks=(uint32_t)(((uint64_t)size(&f->original)+BB-1u)/BB),dest=0;
    uint64_t total=total_blocks(&map);
    if(total<blocks||total>UINT32_MAX){rc=IFS_FS_CORRUPT;goto done;}
    uint32_t allocated=(uint32_t)total;
    int whole=reloc_contiguous(&map,allocated,&f->target);
    if(!blocks)goto done;
    /* Ordinary compaction retains existing file-owned preallocation. */
    f->reserve=policy?(blocks+9u)/10u:allocated-blocks;
    uint32_t wanted=blocks+f->reserve;
    if(wanted<blocks){rc=IFS_FS_NO_SPACE;goto done;}
    if(policy&&whole&&map.count==1u){
        if(wanted==allocated)goto done;
        if(wanted<allocated||reloc_free(bitmap,f->target+allocated,wanted-allocated)){
            f->reserve_only=1;
            if(wanted>allocated)reloc_sim_bits(bitmap,f->target+allocated,wanted-allocated,1);
            else reloc_sim_bits(bitmap,f->target+wanted,allocated-wanted,0);
            goto done;
        }
    }
    rc=reloc_choose(bitmap,wanted,&dest);
    if(rc==IFS_FS_NO_SPACE&&whole&&wanted==allocated){rc=0;goto done;}
    if(rc)goto done;
    if(whole&&wanted==allocated&&dest>=f->target)goto done;
    f->target=dest;f->move=1;
    reloc_sim_bits(bitmap,dest,wanted,1);
    for(uint32_t i=0;i<map.count;i++)
        for(uint32_t j=0;j<map.run[i].count;j++){
            uint32_t b=map.run[i].first+j;bitmap[b/8u]&=(uint8_t)~(1u<<(b%8u));
        }
    for(uint32_t next=le32(f->original.raw+28);next;){
        uint8_t raw[BB];uint32_t b=next-1u;
        rc=rd(&active,(uint64_t)b*BB,raw,BB);if(rc)goto done;
        bitmap[b/8u]&=(uint8_t)~(1u<<(b%8u));next=le32(raw);
    }
done:
    map_drop(&active,&map);return rc;
}
static int reloc_plan_impl(void *v,const char *path,uint32_t policy,void **out)
{
    if(!out)return IFS_FS_INVALID;
    *out=NULL;
    if(!live(v)||!path)return IFS_FS_INVALID;
    if(!active.writable)return IFS_FS_ACCESS;
    if(policy!=0u&&policy!=10u)return IFS_FS_UNSUPPORTED;
    if(active_plan)return IFS_FS_BUSY;
    if(reloc_generation==UINTPTR_MAX)return IFS_FS_NO_SPACE;
    int rc=check_native(&active);if(rc)return rc;
    Entry e;rc=resolve(path,&e);if(rc)return rc;
    if(strcmp(path,"/")){
        Node parent;char name[49];rc=resolve_parent(path,&parent,name);if(rc)return rc;
    }
    if(mode(&e.node)!=REG&&mode(&e.node)!=DIR)return IFS_FS_UNSUPPORTED;
    RelocPlan *p=alloc(&active,sizeof *p);
    uint8_t *seen=alloc(&active,active.blocks),*bitmap=alloc(&active,bitmap_bytes());
    if(!p||!seen||!bitmap){drop(&active,p);drop(&active,seen);drop(&active,bitmap);return IFS_FS_NO_MEMORY;}
    *p=(RelocPlan){0};memset(seen,0,active.blocks);
    memcpy(bitmap,active.bitmap,bitmap_bytes());
    rc=reloc_collect(p,seen,&e.node,0);
    if(!rc)for(size_t i=0;i<p->count;i++){
        rc=reloc_simulate(&p->files[i],bitmap,policy);if(rc)break;
    }
    drop(&active,seen);drop(&active,bitmap);
    if(rc){drop(&active,p->files);drop(&active,p);return rc;}
    p->token=++reloc_generation;reloc_plan=p;active_plan=p->token;
    *out=(void*)active_plan;return IFS_FS_OK;
}
static int reloc_verify_file(const RelocFile *f)
{
    Node node={.identity=f->original.identity};
    int rc=rd(&active,node.identity,node.raw,REC);if(rc)return rc;
    if(mode(&node)!=REG||size(&node)!=size(&f->original))return IFS_FS_CORRUPT;
    /* Relocation may only alter the native extent fields, not other inode
     * attributes or namespace identity. */
    uint8_t expected[REC];memcpy(expected,f->original.raw,REC);
    memcpy(expected+20,node.raw+20,12);memcpy(expected+48,node.raw+48,2);
    if(memcmp(expected,node.raw,REC))return IFS_FS_CORRUPT;
    Map map;rc=node_map(&active,&node,&map);if(rc)return rc;
    uint32_t start=0;uint64_t digest=0;
    if(!reloc_contiguous(&map,(uint32_t)(((uint64_t)size(&node)+BB-1u)/BB)+f->reserve,&start)||start!=f->target)
        rc=IFS_FS_CORRUPT;
    if(!rc)rc=reloc_digest(&node,&map,&digest);
    if(!rc&&digest!=f->digest)rc=IFS_FS_CORRUPT;
    map_drop(&active,&map);return rc;
}
static int relocate_one(void *v,const RelocFile *f)
{
    Node node={.identity=f->original.identity};
    int rc=rd(&active,node.identity,node.raw,REC);if(rc)return rc;
    if(memcmp(node.raw,f->original.raw,REC))return IFS_FS_CORRUPT;
    Map old;rc=node_map(&active,&node,&old);if(rc)return rc;
    uint64_t digest=0;rc=reloc_digest(&node,&old,&digest);
    if(!rc&&digest!=f->digest)rc=IFS_FS_CORRUPT;
    if(rc){map_drop(&active,&old);return rc;}
    Mutation m;rc=mutation_begin(&m,v);
    if(rc){map_drop(&active,&old);return rc;}
    uint32_t blocks=(uint32_t)(((uint64_t)size(&node)+BB-1u)/BB);
    uint32_t wanted=blocks+f->reserve;
    Run run={f->target,wanted};Map next={.run=&run,.count=1};int direct=0;
    if(!blocks||wanted<blocks||run.first<2u||run.first>=active.blocks||wanted>active.blocks-run.first){rc=IFS_FS_CORRUPT;goto done;}
    uint32_t begin=run.first,end=run.first+wanted;
    if(f->reserve_only){
        uint32_t old_count=(uint32_t)total_blocks(&old),start=0;
        if(old.count!=1u||!reloc_contiguous(&old,old_count,&start)||start!=run.first){
            rc=IFS_FS_CORRUPT;goto done;
        }
        if(wanted<old_count){
            rc=release_run(&m,run.first+wanted,old_count-wanted);if(rc)goto done;
            begin=end;
        }else begin=run.first+old_count;
    }
    for(uint32_t b=begin;b<end;b++){
        if((b&4095u)==0&&cancelled(&active)){rc=IFS_FS_BUSY;goto done;}
        if(bit(active.bitmap,b)||bit(m.bitmap,b)){rc=IFS_FS_CORRUPT;goto done;}
        m.bitmap[b/8u]|=(uint8_t)(1u<<(b%8u));m.free-=BB;
    }
    if(f->move){
        Run payload={run.first,blocks};
        rc=write_new_run(&m,&old,size(&node),0,payload,0,NULL,0,&direct);if(rc)goto done;
        rc=release_node(&m,&node);if(rc)goto done;
    }
    rc=encode_runs(&m,&node,&next,size(&node));if(rc)goto done;
    if(direct){rc=jflush(&active.host);if(rc)goto done;}
    rc=mutation_commit(&m);
done:
    mutation_end(&m);map_drop(&active,&old);return rc;
}
static int reloc_matches(void *v,void *p)
{return live(v)&&p&&reloc_plan&&active_plan==(uintptr_t)p&&reloc_plan->token==(uintptr_t)p;}
static int reloc_execute_impl(void *v,void *p,uint64_t *moved)
{
    if(!moved)return IFS_FS_INVALID;
    *moved=0;
    if(!reloc_matches(v,p))return IFS_FS_INVALID;
    RelocPlan *q=reloc_plan;
    if(q->complete){*moved=q->moved;return IFS_FS_OK;}
    int rc=IFS_FS_OK;plan_executing=1;
    while(q->next<q->count){
        if(cancelled(&active)){rc=IFS_FS_BUSY;break;}
        RelocFile *f=&q->files[q->next];
        if(f->move||f->reserve_only){rc=relocate_one(v,f);if(rc)break;if(f->move)q->moved+=size(&f->original);}
        rc=reloc_verify_file(f);if(rc){active.fault=1;break;}
        ++q->next;
    }
    plan_executing=0;
    if(!rc)q->complete=1;
    *moved=q->moved;return rc;
}
static int reloc_verify_impl(void *v,void *p)
{
    if(!reloc_matches(v,p))return IFS_FS_INVALID;
    if(!reloc_plan->complete)return IFS_FS_BUSY;
    for(size_t i=0;i<reloc_plan->count;i++){
        int rc=reloc_verify_file(&reloc_plan->files[i]);if(rc)return rc;
    }
    uint64_t errors=0;int rc=check_impl(v,0,&errors);
    return rc?rc:errors?IFS_FS_CORRUPT:IFS_FS_OK;
}
static void reloc_release_impl(void *v,void *p)
{
    if(v&&(uintptr_t)v==active.token&&p&&(uintptr_t)p==active_plan)reloc_drop();
}
