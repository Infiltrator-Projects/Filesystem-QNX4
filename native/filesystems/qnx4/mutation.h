/* SPDX-License-Identifier: GPL-3.0-or-later
 * QNX4 native namespace transaction foundation. Stable user inode
 * identities are allocated from the reserved inode file; both short
 * and long names use native link records. No shadow namespace.
 * This revision supports create of regular files and directories in
 * available native directory/inode slots, and transactionally removes or
 * renames native namespace records. Directory and inode-file extension
 * remain separate work.
 */
typedef struct {Tx tx;uint8_t *bitmap;uint64_t free;} Mutation;
static size_t bitmap_bytes(void){return (size_t)(((uint64_t)active.blocks+7u)/8u);}
static int mutation_begin(Mutation *m,void *v)
{
    *m=(Mutation){0};
    if(!live(v))return IFS_FS_INVALID;
    if(!active.writable)return IFS_FS_ACCESS;
    if(active_plan&&!plan_executing)return IFS_FS_BUSY;
    if(cancelled(&active))return IFS_FS_BUSY;
    int rc=tx_begin(&m->tx,&active.host);if(rc)return rc;
    m->bitmap=alloc(&active,bitmap_bytes());
    if(!m->bitmap){tx_end(&m->tx);return IFS_FS_NO_MEMORY;}
    memcpy(m->bitmap,active.bitmap,bitmap_bytes());m->free=active.free;
    rc=tx_stage(&m->tx,BB,active.root.raw,REC);
    if(rc){drop(&active,m->bitmap);tx_end(&m->tx);}
    return rc;
}
static void mutation_end(Mutation *m){drop(&active,m->bitmap);tx_end(&m->tx);}
static int mutation_commit(Mutation *m)
{
    for(size_t at=0;at<bitmap_bytes();at+=BB){
        size_t n=bitmap_bytes()-at;if(n>BB)n=BB;
        if(memcmp(m->bitmap+at,active.bitmap+at,n)){
            int rc=tx_stage(&m->tx,2u*BB+at,m->bitmap+at,(uint32_t)n);if(rc)return rc;
        }
    }
    if(cancelled(&active))return IFS_FS_BUSY;
    int rc=tx_commit(&m->tx);
    if(rc){active.fault=1;return rc;}
    memcpy(active.bitmap,m->bitmap,bitmap_bytes());active.free=m->free;
    return IFS_FS_OK;
}
static int allocate_block(Mutation *m,uint32_t *block)
{
    for(uint32_t b=2;b<active.blocks;b++){
        if((b&4095u)==0&&cancelled(&active))return IFS_FS_BUSY;
        if(!bit(active.bitmap,b)&&!bit(m->bitmap,b)){
            m->bitmap[b/8u]|=(uint8_t)(1u<<(b%8u));
            m->free-=BB;*block=b;return IFS_FS_OK;
        }
    }
    return IFS_FS_NO_SPACE;
}
static int allocate_inode(Node *n)
{
    for(uint64_t at=REC;at<size(&active.inodes);at+=REC){
        uint8_t raw[REC];int rc=map_read(&active,&active.inode_map,at,raw,REC);if(rc)return rc;
        if(raw[63]&(USED|INODE|LINK|BUSY))continue;
        *n=(Node){0};return map_address(&active.inode_map,at,&n->identity);
    }
    return IFS_FS_NO_SPACE;
}
static int protected_name(const Node *parent,const char *name)
{
    if(parent->identity!=BB)return 0;
    return !strcmp(name,".bitmap")||!strcmp(name,".inodes")||
        !strcmp(name,".boot")||!strcmp(name,".altboot")||!strcmp(name,".ifs-journal");
}
static int resolve_parent(const char *path,Node *parent,char name[49])
{
    if(!path||path[0]!='/')return IFS_FS_INVALID;
    size_t n=0;while(n<=65536u&&path[n])++n;
    if(n<2u||n>65536u)return IFS_FS_INVALID;
    const char *last=strrchr(path,'/');size_t len=(size_t)(path+n-last-1);
    if(!len||len>48u||!strcmp(last+1,".")||!strcmp(last+1,".."))return IFS_FS_INVALID;
    size_t prefix=(size_t)(last-path);
    if(!prefix)prefix=1;
    char *buf=alloc(&active,prefix+1u);if(!buf)return IFS_FS_NO_MEMORY;
    memcpy(buf,path,prefix);buf[prefix]=0;
    Entry e;int rc=resolve(buf,&e);drop(&active,buf);if(rc)return rc;
    if(mode(&e.node)!=DIR)return IFS_FS_INVALID;
    *parent=e.node;memcpy(name,last+1,len+1u);
    if(protected_name(parent,name))return IFS_FS_ACCESS;
    return IFS_FS_OK;
}
static int directory_slot(const Node *parent,uint64_t *physical)
{
    Map map;int rc=node_map(&active,parent,&map);if(rc)return rc;
    rc=IFS_FS_NO_SPACE;
    for(uint64_t at=2u*REC;at<size(parent);at+=REC){
        uint8_t p[REC];rc=map_read(&active,&map,at,p,REC);if(rc)break;
        if(!(p[63]&(USED|LINK|INODE|BUSY))){
            rc=map_address(&map,at,physical);break;
        }
        rc=IFS_FS_NO_SPACE;
    }
    map_drop(&active,&map);return rc;
}
static int create_impl(void *v,const char *path,uint32_t type)
{
    if(type!=IFS_FS_ENTRY_FILE&&type!=IFS_FS_ENTRY_DIRECTORY)return IFS_FS_UNSUPPORTED;
    Mutation m;int rc=mutation_begin(&m,v);if(rc)return rc;
    Node parent,child;char name[49];uint64_t slot=0;
    rc=resolve_parent(path,&parent,name);if(rc)goto done;
    Entry e;rc=directory_entry(&active,&parent,name,(uint32_t)strlen(name),0,&e);
    if(!rc){rc=IFS_FS_EXISTS;goto done;}
    if(rc!=IFS_FS_NOT_FOUND)goto done;
    rc=directory_slot(&parent,&slot);if(rc)goto done;
    rc=allocate_inode(&child);if(rc)goto done;
    uint32_t block=0;
    if(type==IFS_FS_ENTRY_DIRECTORY){
        rc=allocate_block(&m,&block);if(rc)goto done;
        uint8_t dir[BB]={0};
        make_link(dir,".",(uint32_t)(child.identity/BB),(uint8_t)(child.identity%BB/REC));
        memcpy(dir+16,"I\003QNX",5);
        make_link(dir+REC,"..",(uint32_t)(parent.identity/BB),(uint8_t)(parent.identity%BB/REC));
        rc=tx_stage(&m.tx,(uint64_t)block*BB,dir,BB);if(rc)goto done;
        make_node(child.raw,"inode",BB,block,1,0040755u,1);
    }else make_node(child.raw,"inode",0,0,0,0100644u,1);
    child.raw[63]|=INODE;
    rc=tx_stage(&m.tx,child.identity,child.raw,REC);if(rc)goto done;
    uint8_t link[REC];
    make_link(link,name,(uint32_t)(child.identity/BB),(uint8_t)(child.identity%BB/REC));
    rc=tx_stage(&m.tx,slot,link,REC);if(!rc)rc=mutation_commit(&m);
done:
    mutation_end(&m);return rc;
}

/* Resolve the actual directory record as well as its inode. A linked
 * directory entry and the inode it names are distinct native objects. */
static int source_slot(const Node *parent,const char *name,Entry *entry,
                       uint64_t *physical,uint8_t raw[REC])
{
    Map map;int rc=node_map(&active,parent,&map);if(rc)return rc;
    int found=0;uint32_t len=(uint32_t)strlen(name);
    for(uint64_t at=2u*REC;at<size(parent);at+=REC){
        uint8_t p[REC];Entry e;
        if(cancelled(&active)){rc=IFS_FS_BUSY;goto done;}
        rc=map_read(&active,&map,at,p,REC);if(rc)goto done;
        rc=decode_entry(&active,parent,&map,at,p,&e);
        if(rc==IFS_FS_NOT_FOUND||rc==IFS_FS_END)continue;
        if(rc)goto done;
        if(e.length!=len||memcmp(e.name,name,len))continue;
        if(found){rc=IFS_FS_CORRUPT;goto done;}
        rc=map_address(&map,at,physical);if(rc)goto done;
        *entry=e;memcpy(raw,p,REC);found=1;
    }
    rc=found?IFS_FS_OK:IFS_FS_NOT_FOUND;
done:
    map_drop(&active,&map);return rc;
}

/* Free all data and extension blocks only in the staged bitmap. The
 * verified live bitmap stays unchanged until the redo transaction commits. */
static int release_run(Mutation *m,uint32_t first,uint32_t count)
{
    if(first<2u||first>=active.blocks||count>active.blocks-first)
        return IFS_FS_CORRUPT;
    for(uint32_t b=first;b<first+count;b++){
        if((b&4095u)==0&&cancelled(&active))return IFS_FS_BUSY;
        if(!bit(m->bitmap,b))return IFS_FS_CORRUPT;
        m->bitmap[b/8u]&=(uint8_t)~(1u<<(b%8u));m->free+=BB;
    }
    return IFS_FS_OK;
}
static int release_node(Mutation *m,const Node *node)
{
    Map map;int rc=node_map(&active,node,&map);if(rc)return rc;
    for(uint32_t i=0;i<map.count;i++){
        rc=release_run(m,map.run[i].first,map.run[i].count);if(rc)goto done;
    }
    /* node_map already validated the complete chain, including cycles,
     * allocation ownership and forward/reverse links. Free its metadata
     * separately from the data extents; read the next pointer first. */
    uint32_t next=le32(node->raw+28);
    while(next){
        uint8_t p[BB];uint32_t block=next-1u;
        rc=rd(&active,(uint64_t)block*BB,p,BB);if(rc)goto done;
        next=le32(p);rc=release_run(m,block,1);if(rc)goto done;
    }
done:
    map_drop(&active,&map);return rc;
}
static int remove_impl(void *v,const char *path)
{
    Mutation m;int rc=mutation_begin(&m,v);if(rc)return rc;
    Node parent;Entry e,child;char name[49];uint64_t slot=0;
    uint8_t raw[REC],zero[REC]={0};
    rc=resolve_parent(path,&parent,name);if(rc)goto done;
    rc=source_slot(&parent,name,&e,&slot,raw);if(rc)goto done;
    if(e.node.identity<2u*BB){rc=IFS_FS_ACCESS;goto done;}
    if(mode(&e.node)==DIR){
        rc=directory_entry(&active,&e.node,NULL,0,0,&child);
        if(!rc){rc=IFS_FS_BUSY;goto done;}
        if(rc!=IFS_FS_END)goto done;
    }
    uint32_t links=le16(e.node.raw+56);
    if((raw[63]&LINK)&&links>1u){
        if(mode(&e.node)==DIR){rc=IFS_FS_CORRUPT;goto done;}
        put16(e.node.raw+56,links-1u);
        rc=tx_stage(&m.tx,e.node.identity,e.node.raw,REC);if(rc)goto done;
    }else{
        if(links!=1u){rc=IFS_FS_UNSUPPORTED;goto done;}
        rc=release_node(&m,&e.node);if(rc)goto done;
        if(raw[63]&LINK){
            rc=tx_stage(&m.tx,e.node.identity,zero,REC);if(rc)goto done;
        }
    }
    rc=tx_stage(&m.tx,slot,zero,REC);if(!rc)rc=mutation_commit(&m);
done:
    mutation_end(&m);return rc;
}
static int rename_impl(void *v,const char *old_path,const char *new_path)
{
    Mutation m;int rc=mutation_begin(&m,v);if(rc)return rc;
    Node from,to;Entry e,existing;char old_name[49],new_name[49];
    uint64_t old_slot=0,new_slot=0;uint8_t raw[REC],zero[REC]={0};
    rc=resolve_parent(old_path,&from,old_name);if(rc)goto done;
    rc=resolve_parent(new_path,&to,new_name);if(rc)goto done;
    rc=source_slot(&from,old_name,&e,&old_slot,raw);if(rc)goto done;
    if(e.node.identity<2u*BB){rc=IFS_FS_ACCESS;goto done;}
    if(!strcmp(old_path,new_path)){rc=IFS_FS_OK;goto done;}
    rc=directory_entry(&active,&to,new_name,(uint32_t)strlen(new_name),0,&existing);
    if(!rc){rc=IFS_FS_EXISTS;goto done;}
    if(rc!=IFS_FS_NOT_FOUND)goto done;
    int moving=from.identity!=to.identity;
    if(mode(&e.node)==DIR){
        size_t n=strlen(old_path);
        /* Canonical resolver rejects dot components and redundant slashes.
         * A destination below the moved directory would create a cycle. */
        if(!strncmp(old_path,new_path,n)&&new_path[n]=='/'){
            rc=IFS_FS_INVALID;goto done;
        }
        /* A direct directory inode's identity changes when moved; the
         * linked form used by this provider has a stable inode identity. */
        if(moving&&!(raw[63]&LINK)){rc=IFS_FS_UNSUPPORTED;goto done;}
    }
    new_slot=old_slot;
    if(moving){rc=directory_slot(&to,&new_slot);if(rc)goto done;}
    size_t width=(raw[63]&LINK)?48u:16u;
    if(strlen(new_name)>width){rc=IFS_FS_UNSUPPORTED;goto done;}
    memset(raw,0,width);memcpy(raw,new_name,strlen(new_name));
    if(moving&&mode(&e.node)==DIR){
        Map map;uint64_t parent_at;
        rc=node_map(&active,&e.node,&map);if(rc)goto done;
        rc=map_address(&map,REC,&parent_at);map_drop(&active,&map);if(rc)goto done;
        uint8_t up[REC];rc=rd(&active,parent_at,up,REC);if(rc)goto done;
        if(!(up[63]&LINK)||!name_is(up,48,"..")){rc=IFS_FS_CORRUPT;goto done;}
        put32(up+48,(uint32_t)(to.identity/BB)+1u);
        up[52]=(uint8_t)(to.identity%BB/REC);
        rc=tx_stage(&m.tx,parent_at,up,REC);if(rc)goto done;
    }
    rc=tx_stage(&m.tx,new_slot,raw,REC);if(rc)goto done;
    if(moving){rc=tx_stage(&m.tx,old_slot,zero,REC);if(rc)goto done;}
    rc=mutation_commit(&m);
done:
    mutation_end(&m);return rc;
}
