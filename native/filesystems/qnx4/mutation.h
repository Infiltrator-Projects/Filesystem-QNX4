/* SPDX-License-Identifier: GPL-3.0-or-later
 * QNX4 native namespace transaction foundation. Stable user inode
 * identities are allocated from the reserved inode file; both short
 * and long names use native link records. No shadow namespace.
 * This revision supports create of regular files and directories in
 * available native directory/inode slots. Directory and inode-file
 * extension, payload writes and removal are separate follow-on work.
 */
typedef struct {Tx tx;uint8_t *bitmap;uint64_t free;} Mutation;
static size_t bitmap_bytes(void){return (size_t)(((uint64_t)active.blocks+7u)/8u);}
static int mutation_begin(Mutation *m,void *v)
{
    *m=(Mutation){0};
    if(!live(v))return IFS_FS_INVALID;
    if(!active.writable)return IFS_FS_ACCESS;
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
