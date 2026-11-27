/* SPDX-License-Identifier: GPL-3.0-or-later
 * QNX4 native copy-on-write payload mutation. Old extents remain owned
 * until a single redo transaction publishes the new inode, extent chain
 * and bitmap. Fully free aligned pages may be written directly before
 * publication; mixed pages (including every partially occupied sector)
 * are journaled, so a torn write cannot damage a neighbouring live file.
 * Unchanged payload runs are retained, not copied on streaming append.
 */
typedef struct {Map map;uint32_t capacity;} RunList;
static int append_run(RunList *list,uint32_t first,uint32_t count)
{
    if(!count)return IFS_FS_OK;
    Map *map=&list->map;
    if(map->count){
        Run *last=&map->run[map->count-1u];
        if((uint64_t)last->first+last->count==first){
            if(count>UINT32_MAX-last->count)return IFS_FS_INVALID;
            last->count+=count;return IFS_FS_OK;
        }
    }
    if(map->count==UINT16_MAX)return IFS_FS_NO_SPACE;
    if(map->count==list->capacity){
        uint32_t cap=list->capacity?list->capacity*2u:16u;
        if(cap>UINT16_MAX)cap=UINT16_MAX;
        Run *p=alloc(&active,(size_t)cap*sizeof *p);
        if(!p)return IFS_FS_NO_MEMORY;
        if(map->count)memcpy(p,map->run,(size_t)map->count*sizeof *p);
        drop(&active,map->run);map->run=p;list->capacity=cap;
    }
    map->run[map->count++]=(Run){first,count};return IFS_FS_OK;
}
static uint64_t total_blocks(const Map *map)
{
    uint64_t n=0;
    for(uint32_t i=0;i<map->count;i++)n+=map->run[i].count;
    return n;
}
/* Copy or release the intersection with [first,end) in logical allocation
 * units. No physical address is ever derived from the logical index alone. */
static int slice_runs(Mutation *m,const Map *map,uint64_t first,uint64_t end,
                      RunList *keep)
{
    uint64_t logical=0;
    if(first>end||end>total_blocks(map))return IFS_FS_CORRUPT;
    for(uint32_t i=0;i<map->count&&logical<end;i++){
        Run r=map->run[i];uint64_t next=logical+r.count;
        if(next>first){
            uint64_t lo=first>logical?first:logical,hi=end<next?end:next;
            if(hi>lo){
                uint32_t at=r.first+(uint32_t)(lo-logical),n=(uint32_t)(hi-lo);
                int rc=keep?append_run(keep,at,n):release_run(m,at,n);
                if(rc)return rc;
            }
        }
        logical=next;
    }
    return IFS_FS_OK;
}
static int next_free_run(Mutation *m,uint32_t *cursor,uint32_t want,Run *out)
{
    uint32_t b=*cursor;
    while(b<active.blocks&&(bit(active.bitmap,b)||bit(m->bitmap,b))){
        if((b&4095u)==0&&cancelled(&active))return IFS_FS_BUSY;
        ++b;
    }
    if(b==active.blocks)return IFS_FS_NO_SPACE;
    uint32_t start=b;
    while(b<active.blocks&&b-start<want&&!bit(active.bitmap,b)&&!bit(m->bitmap,b)){
        if((b&4095u)==0&&cancelled(&active))return IFS_FS_BUSY;
        m->bitmap[b/8u]|=(uint8_t)(1u<<(b%8u));m->free-=BB;++b;
    }
    *cursor=b;*out=(Run){start,b-start};return IFS_FS_OK;
}
/* Materialise each new logical block from its old bytes, zeros beyond
 * old EOF, and the caller's overlay. Direct writes never target a page
 * containing any previously allocated block, even one being replaced. */
static int write_new_run(Mutation *m,const Map *old,uint32_t old_size,
                         uint64_t logical,Run run,uint64_t offset,
                         const uint8_t *source,uint32_t length,int *direct)
{
    uint32_t done=0;
    while(done<run.count){
        if(cancelled(&active))return IFS_FS_BUSY;
        uint64_t physical=(uint64_t)(run.first+done)*BB;
        uint32_t n=(JUNIT-(uint32_t)(physical%JUNIT))/BB;
        if(n>run.count-done)n=run.count-done;
        uint32_t bytes=n*BB;uint8_t data[JUNIT]={0};
        uint64_t at=(logical+done)*BB,finish=at+bytes,write_end=offset+length;
        uint32_t old_bytes=at<old_size?(uint32_t)((old_size-at)<bytes?old_size-at:bytes):0;
        if(old_bytes&&!(source&&offset<=at&&write_end>=at+old_bytes)){
            int rc=map_read(&active,old,at,data,old_bytes);if(rc)return rc;
        }
        if(source&&offset<finish&&write_end>at){
            uint64_t lo=offset>at?offset:at,hi=write_end<finish?write_end:finish;
            memcpy(data+(size_t)(lo-at),source+(size_t)(lo-offset),(size_t)(hi-lo));
        }
        int rc;
        if(bytes==JUNIT&&physical%JUNIT==0){
            /* All blocks were free in the original bitmap, by construction. */
            rc=jwrite(&active.host,physical,data);*direct=1;
        }else rc=tx_stage(&m->tx,physical,data,bytes);
        if(rc)return rc;
        done+=n;
    }
    return IFS_FS_OK;
}
static int release_extensions(Mutation *m,const Node *node)
{
    uint32_t next=le32(node->raw+28);
    while(next){
        if(cancelled(&active))return IFS_FS_BUSY;
        uint8_t p[BB];uint32_t block=next-1u;
        int rc=rd(&active,(uint64_t)block*BB,p,BB);if(rc)return rc;
        next=le32(p);rc=release_run(m,block,1);if(rc)return rc;
    }
    return IFS_FS_OK;
}
/* Publish the native first extent and linked extent blocks. Each block
 * contains at most 60 entries, a reverse link, total and native magic. */
static int encode_runs(Mutation *m,Node *node,const Map *map,uint32_t bytes)
{
    uint32_t ext=map->count>1u?(map->count-1u+59u)/60u:0;
    uint32_t *blocks=ext?alloc(&active,(size_t)ext*sizeof *blocks):NULL;
    if(ext&&!blocks)return IFS_FS_NO_MEMORY;
    int rc=IFS_FS_OK;
    for(uint32_t i=0;i<ext;i++){rc=allocate_block(m,&blocks[i]);if(rc)goto done;}
    put32(node->raw+16,bytes);
    put32(node->raw+20,map->count?map->run[0].first+1u:0);
    put32(node->raw+24,map->count?map->run[0].count:0);
    put32(node->raw+28,ext?blocks[0]+1u:0);
    put16(node->raw+48,map->count);
    for(uint32_t i=0,j=1;i<ext;i++){
        uint8_t p[BB]={0};uint32_t n=map->count-j,total=0;
        if(n>60u)n=60u;
        put32(p,i+1u<ext?blocks[i+1u]+1u:0);
        put32(p+4,i?blocks[i-1u]+1u:0);p[8]=(uint8_t)n;
        for(uint32_t k=0;k<n;k++,j++){
            Run r=map->run[j];put32(p+16u+8u*k,r.first+1u);
            put32(p+20u+8u*k,r.count);total+=r.count;
        }
        put32(p+12,total);memcpy(p+496,"IamXblk",7);
        memcpy(p+504,node->raw+20,8);
        rc=tx_stage(&m->tx,(uint64_t)blocks[i]*BB,p,BB);if(rc)goto done;
    }
    rc=tx_stage(&m->tx,node->identity,node->raw,REC);
done:
    drop(&active,blocks);return rc;
}
static int payload_impl(void *v,const char *path,uint64_t offset,
                        const void *source,uint32_t length,uint64_t resize,
                        int truncating)
{
    if((!truncating&&((!source&&length)||offset>UINT32_MAX||
        length>UINT32_MAX-offset))||(truncating&&resize>UINT32_MAX))
        return IFS_FS_INVALID;
    Mutation m;int rc=mutation_begin(&m,v);if(rc)return rc;
    Node parent;Entry e;char name[49];Map old={0};RunList next={0};
    uint64_t slot;uint8_t raw[REC];int direct=0;
    rc=resolve_parent(path,&parent,name);if(rc)goto done;
    rc=source_slot(&parent,name,&e,&slot,raw);if(rc)goto done;
    if(mode(&e.node)!=REG){rc=IFS_FS_UNSUPPORTED;goto done;}
    if(e.node.identity<2u*BB){rc=IFS_FS_ACCESS;goto done;}
    uint32_t old_size=size(&e.node);
    uint64_t end=offset+length;
    uint32_t new_size=truncating?(uint32_t)resize:(end>old_size?(uint32_t)end:old_size);
    if((!truncating&&!length)||(truncating&&new_size==old_size)){rc=IFS_FS_OK;goto done;}
    rc=node_map(&active,&e.node,&old);if(rc)goto done;
    uint64_t old_count=total_blocks(&old),new_count=((uint64_t)new_size+BB-1u)/BB;
    uint64_t first,last;
    if(truncating&&new_size<old_size){
        first=new_count;last=old_count;
        rc=slice_runs(&m,&old,0,first,&next);if(rc)goto done;
    }else{
        uint64_t start=truncating?old_size:(offset<old_size?offset:old_size);
        first=start/BB;last=(truncating?(uint64_t)new_size:end)+BB-1u;last/=BB;
        rc=slice_runs(&m,&old,0,first,&next);if(rc)goto done;
        uint32_t cursor=2u;uint64_t logical=first;
        while(logical<last){
            Run run;rc=next_free_run(&m,&cursor,(uint32_t)(last-logical),&run);
            if(rc)goto done;
            rc=append_run(&next,run.first,run.count);if(rc)goto done;
            rc=write_new_run(&m,&old,old_size,logical,run,offset,source,length,&direct);
            if(rc)goto done;
            logical+=run.count;
        }
        if(last<new_count){
            rc=slice_runs(&m,&old,last,new_count,&next);if(rc)goto done;
        }
    }
    if(first<old_count){
        uint64_t release_end=last<old_count?last:old_count;
        rc=slice_runs(&m,&old,first,release_end,NULL);if(rc)goto done;
    }
    /* Native preallocation beyond old EOF is not part of the new map. */
    if(old_count>new_count&&last<old_count){
        uint64_t tail=new_count>last?new_count:last;
        rc=slice_runs(&m,&old,tail,old_count,NULL);if(rc)goto done;
    }
    if(total_blocks(&next.map)!=new_count){rc=IFS_FS_CORRUPT;goto done;}
    rc=release_extensions(&m,&e.node);if(rc)goto done;
    rc=encode_runs(&m,&e.node,&next.map,new_size);if(rc)goto done;
    /* Ensure prepublished free-page data is durable before journal intent.
     * On failure the old allocation map still owns exactly the old data. */
    if(direct){rc=jflush(&active.host);if(rc)goto done;}
    rc=mutation_commit(&m);
done:
    map_drop(&active,&old);map_drop(&active,&next.map);mutation_end(&m);return rc;
}
static int write_impl(void *v,const char *path,uint64_t offset,const void *source,uint32_t length)
{return payload_impl(v,path,offset,source,length,0,0);}
static int truncate_impl(void *v,const char *path,uint64_t bytes)
{return payload_impl(v,path,0,NULL,0,bytes,1);}
