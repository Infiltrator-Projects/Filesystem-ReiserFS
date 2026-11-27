/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent clean ReiserFS 3.5/3.6 read-only engine. Format references:
 * Linux v6.12 fs/reiserfs/reiserfs.h, stree.c, bitmap.c, journal.c.
 * This implementation uses only the Common V2 host, no Linux VFS.
 * Modern 64 KiB superblock, distributed bitmaps, internal journal only.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
enum { MAX_BLOCK=4096, MAX_NAME=255, SLOTS=8, MAX_DEPTH=5,
       MAX_BITMAP=16*1024*1024, MAX_VISITS=262144, HEADER=24, IH=24 };
typedef struct { uint32_t dir,id,type; uint64_t off; } Key;
typedef struct { uint32_t dir,id,mode,uid,gid,type; uint64_t bytes; } Node;
typedef struct { Node node; uint32_t length; char name[MAX_NAME+1]; } Entry;
typedef struct {
    IfsFsHostRuntimeV2 host;
    uint32_t block,blocks,free_blocks,root,height,super,journal,jlen,variant;
    uint8_t *bitmap;
    char label[17];
    uintptr_t token;
} Volume;
static Volume slots[SLOTS];static uintptr_t generation;
static atomic_flag gate=ATOMIC_FLAG_INIT;
static int enter(void){return !atomic_flag_test_and_set_explicit(&gate,memory_order_acquire);}
static int leave(int rc){atomic_flag_clear_explicit(&gate,memory_order_release);return rc;}
static void copy(void *d,const void *s,size_t n){uint8_t *a=d;const uint8_t *b=s;while(n--)*a++=*b++;}
static void zero(void *d,size_t n){uint8_t *a=d;while(n--)*a++=0;}
static int same(const void *a,const void *b,size_t n)
{const uint8_t *p=a,*q=b;while(n--)if(*p++!=*q++)return 0;return 1;}
static uint32_t u16(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8;}
static uint32_t u32(const uint8_t *p){return u16(p)|(u16(p+2)<<16);}
static uint64_t u64(const uint8_t *p){return u32(p)|(uint64_t)u32(p+4)<<32;}
static int rd(const Volume *v,uint64_t off,void *p,uint32_t n)
{return ifs_fs_host_read_bytes_v2(&v->host,off,p,n);}
static int cancelled(const Volume *v){return v->host.cancelled&&v->host.cancelled(v->host.context);}
static int allocated(const Volume *v,uint32_t b){return b<v->blocks&&(v->bitmap[b/8]&(1u<<(b%8)));}
static int usable(const Volume *v,uint32_t b)
{return b>v->super+1&&b<v->blocks&&b%(v->block*8)&&
 !(b>=v->journal&&b-v->journal<=v->jlen)&&allocated(v,b);}
static void dispose(Volume *v)
{if(v->bitmap)v->host.release(v->host.context,v->bitmap);zero(v,sizeof(*v));}
static int cmp(Key a,Key b)
{
    if(a.dir!=b.dir)return a.dir<b.dir?-1:1;
    if(a.id!=b.id)return a.id<b.id?-1:1;
    if(a.off!=b.off)return a.off<b.off?-1:1;
    if(a.type!=b.type)return a.type<b.type?-1:1;
    return 0;
}
/* Internal delimiters have no item-version field; old uniqueness values
 * are distinct from the new key's high type nibble, except zero stat. */
static int key(const uint8_t *p,int version,Key *out)
{
    Key k={.dir=u32(p),.id=u32(p+4)};
    uint32_t unique=u32(p+12);
    if(version==-1)version=(unique==0||unique==500||unique==UINT32_MAX||unique==UINT32_MAX-1)?0:1;
    if(version==0){
        k.off=u32(p+8);
        switch(unique){case 0:k.type=0;break;case UINT32_MAX-1:k.type=1;break;
        case UINT32_MAX:k.type=2;break;case 500:k.type=3;break;default:return IFS_FS_CORRUPT;}
    }else if(version==1){uint64_t x=u64(p+8);k.off=x&UINT64_C(0x0fffffffffffffff);k.type=(uint32_t)(x>>60);}
    else return IFS_FS_CORRUPT;
    if(k.type>3||(!k.type&&k.off)||(k.type&&!k.off))return IFS_FS_CORRUPT;
    *out=k;return IFS_FS_OK;
}
typedef int (*Visit)(const Volume *,Key,const uint8_t *,uint32_t,uint32_t,uint32_t,void *);
typedef struct {
    Key lo,hi,last;unsigned have_last;uint32_t visits;
    uint8_t *frames,*seen;
    Visit fn;void *context;
} Walk;
/* Prune by complete key range, validate delimiters and leaf packing.
 * A visited-node map and bounded depth reject cycles and cross-links.
 * Child bodies occupy independent host allocations, never firmware stack.
 */
static int walk_node(const Volume *v,Walk *w,uint32_t b,unsigned depth,
                     const Key *lower,const Key *upper,uint32_t used)
{
    if(depth>=v->height||depth>=MAX_DEPTH||!usable(v,b)||
       (w->seen[b/8]&(1u<<(b%8))))return IFS_FS_CORRUPT;
    if(++w->visits>MAX_VISITS)return IFS_FS_UNSUPPORTED;
    if(cancelled(v))return IFS_FS_BUSY;
    w->seen[b/8]|=(uint8_t)(1u<<(b%8));
    uint8_t *p=w->frames+(size_t)depth*v->block;
    int rc=rd(v,(uint64_t)b*v->block,p,v->block);if(rc)return rc;
    uint32_t level=u16(p),n=u16(p+2),free=u16(p+4);
    if(level!=v->height-depth||!n||free>v->block-HEADER||
       (used!=UINT32_MAX&&used!=v->block-HEADER-free))return IFS_FS_CORRUPT;
    if(level==1){
        if(n>(v->block-HEADER)/IH)return IFS_FS_CORRUPT;
        uint32_t end=v->block,base=HEADER+n*IH;Key prev={0};
        for(uint32_t i=0;i<n;++i){
            const uint8_t *h=p+HEADER+i*IH;
            uint32_t len=u16(h+18),loc=u16(h+20),ver=u16(h+22);
            Key k;rc=key(h,(int)ver,&k);if(rc)return rc;
            if(!len||loc<base||loc>end||len!=end-loc||
               (i&&cmp(prev,k)>=0)||(lower&&cmp(k,*lower)<0)||(upper&&cmp(k,*upper)>=0))return IFS_FS_CORRUPT;
            prev=k;end=loc;
        }
        if(end-base!=free)return IFS_FS_CORRUPT;
        for(uint32_t i=0;i<n;++i){
            const uint8_t *h=p+HEADER+i*IH;Key k;
            rc=key(h,(int)u16(h+22),&k);if(rc)return rc;
            if(w->have_last&&cmp(w->last,k)>=0)return IFS_FS_CORRUPT;
            w->last=k;w->have_last=1;
            if(cmp(k,w->lo)>=0&&cmp(k,w->hi)<=0){
                rc=w->fn(v,k,p+u16(h+20),u16(h+18),u16(h+16),u16(h+22),w->context);
                if(rc)return rc;
            }
        }
        return IFS_FS_OK;
    }
    if(n>(v->block-HEADER-8)/24||HEADER+n*16+(n+1)*8+free!=v->block)return IFS_FS_CORRUPT;
    Key prev={0};
    for(uint32_t i=0;i<n;++i){
        Key k;rc=key(p+HEADER+i*16,-1,&k);if(rc)return rc;
        if((i&&cmp(prev,k)>=0)||(lower&&cmp(k,*lower)<=0)||(upper&&cmp(k,*upper)>=0))return IFS_FS_CORRUPT;
        prev=k;
    }
    const uint8_t *children=p+HEADER+n*16;
    for(uint32_t i=0;i<=n;++i){
        uint32_t child=u32(children+i*8),size=u16(children+i*8+4);
        if(!usable(v,child)||!size||size>v->block-HEADER||u16(children+i*8+6))return IFS_FS_CORRUPT;
        for(uint32_t j=0;j<i;++j)if(child==u32(children+j*8))return IFS_FS_CORRUPT;
        Key a,bound;const Key *a0=lower,*b0=upper;
        if(i){rc=key(p+HEADER+(i-1)*16,-1,&a);if(rc)return rc;a0=&a;}
        if(i<n){rc=key(p+HEADER+i*16,-1,&bound);if(rc)return rc;b0=&bound;}
        if((a0&&cmp(*a0,w->hi)>0)||(b0&&cmp(*b0,w->lo)<=0))continue;
        rc=walk_node(v,w,child,depth+1,a0,b0,size);if(rc)return rc;
    }
    return IFS_FS_OK;
}
static int items(const Volume *v,uint32_t dir,uint32_t id,Visit fn,void *ctx)
{
    size_t bytes=(v->blocks+7u)/8u;
    Walk w={.lo={.dir=dir,.id=id},.hi={.dir=dir,.id=id,.type=3,.off=UINT64_MAX},.fn=fn,.context=ctx};
    w.frames=v->host.allocate(v->host.context,(size_t)v->height*v->block);
    w.seen=v->host.allocate(v->host.context,bytes);
    int rc=IFS_FS_NO_MEMORY;
    if(w.frames&&w.seen){zero(w.seen,bytes);rc=walk_node(v,&w,v->root,0,NULL,NULL,UINT32_MAX);}
    if(w.seen)v->host.release(v->host.context,w.seen);
    if(w.frames)v->host.release(v->host.context,w.frames);
    return rc;
}
typedef struct {Node n;unsigned found;} Stat;
static int stat_item(const Volume *v,Key k,const uint8_t *p,uint32_t len,uint32_t aux,uint32_t ver,void *ctx)
{
    (void)v;(void)aux;Stat *s=ctx;if(k.type)return IFS_FS_OK;
    if(s->found++||len!=(ver?44u:32u))return IFS_FS_CORRUPT;
    Node n={.dir=k.dir,.id=k.id,.mode=u16(p)};
    if(ver){if(!u32(p+4))return IFS_FS_CORRUPT;n.bytes=u64(p+8);n.uid=u32(p+16);n.gid=u32(p+20);}
    else {if(!u16(p+2))return IFS_FS_CORRUPT;n.bytes=u32(p+8);n.uid=u16(p+4);n.gid=u16(p+6);}
    switch(n.mode&0170000u){case 0040000:n.type=IFS_FS_ENTRY_DIRECTORY;break;
      case 0100000:n.type=IFS_FS_ENTRY_FILE;break;case 0120000:n.type=IFS_FS_ENTRY_LINK;break;
      default:return IFS_FS_UNSUPPORTED;}
    if(n.bytes>UINT64_C(0x0ffffffffffffffe))return IFS_FS_CORRUPT;
    s->n=n;return IFS_FS_OK;
}
static int inode(const Volume *v,uint32_t dir,uint32_t id,Node *out)
{
    if(!dir||!id)return IFS_FS_CORRUPT;
    Stat s={0};int rc=items(v,dir,id,stat_item,&s);if(rc)return rc;
    if(!s.found)return IFS_FS_NOT_FOUND;
    *out=s.n;return IFS_FS_OK;
}
typedef struct {const Node *dir;const char *name;uint32_t length,index,count,last,found,dots;
                uint32_t child_dir,child_id;Entry entry;} Dir;
static int dir_item(const Volume *v,Key k,const uint8_t *p,uint32_t len,uint32_t aux,uint32_t ver,void *ctx)
{
    (void)v;(void)ver;Dir *d=ctx;if(!k.type)return IFS_FS_OK;
    if(k.type!=3||!aux||aux>len/16)return IFS_FS_CORRUPT;
    uint32_t end=len;
    for(uint32_t i=0;i<aux;++i){
        const uint8_t *h=p+i*16;uint32_t off=u32(h),loc=u16(h+12),state=u16(h+14);
        if((!i&&off!=k.off)||!off||off<=d->last||loc<aux*16||loc>=end||!u32(h+4)||!u32(h+8))return IFS_FS_CORRUPT;
        d->last=off;uint32_t size=end-loc;end=loc;
        if(state&1)return IFS_FS_UNSUPPORTED; /* stat data embedded in old name */
        while(size&&p[loc+size-1]==0)--size;
        if(!size||size>MAX_NAME)return IFS_FS_CORRUPT;
        for(uint32_t j=0;j<size;++j)if(!p[loc+j]||p[loc+j]=='/'||p[loc+j]==92)return IFS_FS_CORRUPT;
        if(off==1||off==2){
            if(!(state&4)||size!=off||!same(p+loc,"..",size))return IFS_FS_CORRUPT;
            if(off==1&&(u32(h+4)!=d->dir->dir||u32(h+8)!=d->dir->id))return IFS_FS_CORRUPT;
            d->dots|=off;continue;
        }
        if(!(state&4))continue;
        if((size==1&&p[loc]=='.')||(size==2&&same(p+loc,"..",2)))return IFS_FS_CORRUPT;
        if(size==14&&same(p+loc,".reiserfs_priv",14))continue;
        if((d->name?(d->length==size&&same(d->name,p+loc,size)):d->count==d->index)){
            if(d->found++)return IFS_FS_CORRUPT;
            d->child_dir=u32(h+4);d->child_id=u32(h+8);
            d->entry.length=size;copy(d->entry.name,p+loc,size);d->entry.name[size]=0;
        }
        if(++d->count>MAX_VISITS)return IFS_FS_UNSUPPORTED;
    }
    if(end!=aux*16)return IFS_FS_CORRUPT;
    return IFS_FS_OK;
}
static int child(const Volume *v,const Node *dir,const char *name,uint32_t length,uint32_t index,Entry *out)
{
    if(dir->type!=IFS_FS_ENTRY_DIRECTORY)return IFS_FS_NOT_FOUND;
    Dir d={.dir=dir,.name=name,.length=length,.index=index};
    int rc=items(v,dir->dir,dir->id,dir_item,&d);if(rc)return rc;
    if(d.dots!=3)return IFS_FS_CORRUPT;
    if(!d.found)return name?IFS_FS_NOT_FOUND:IFS_FS_END;
    rc=inode(v,d.child_dir,d.child_id,&d.entry.node);if(rc)return rc==IFS_FS_NOT_FOUND?IFS_FS_CORRUPT:rc;
    if(d.entry.node.type==IFS_FS_ENTRY_DIRECTORY&&d.child_dir==dir->dir&&d.child_id==dir->id)return IFS_FS_CORRUPT;
    *out=d.entry;return IFS_FS_OK;
}
typedef struct {const Node *node;uint8_t *out;uint64_t at,end,covered,last;unsigned seen;} Read;
static int data_item(const Volume *v,Key k,const uint8_t *p,uint32_t len,uint32_t aux,uint32_t ver,void *ctx)
{
    Read *r=ctx;if(!k.type)return IFS_FS_OK;
    if(k.type!=1&&k.type!=2)return IFS_FS_CORRUPT;
    uint64_t start=k.off-1,span=len;
    if(k.type==1){
        if(len%4||start%v->block)return IFS_FS_CORRUPT;
        span=(uint64_t)(len/4)*v->block;
        if(!ver&&aux!=UINT16_MAX){if(aux>=v->block||aux>=span)return IFS_FS_CORRUPT;span-=aux;}
    }
    if(start>=r->node->bytes||span>UINT64_MAX-start||(r->seen&&start<r->last))return IFS_FS_CORRUPT;
    r->seen=1;r->last=start+span;
    uint64_t end=r->last;if(end>r->node->bytes)end=r->node->bytes;
    uint64_t lo=start>r->at?start:r->at,hi=end<r->end?end:r->end;
    if(lo>=hi)return IFS_FS_OK;
    if(lo!=r->covered)return IFS_FS_CORRUPT;
    if(k.type==2)copy(r->out+(size_t)(lo-r->at),p+(size_t)(lo-start),(size_t)(hi-lo));
    else while(lo<hi){
        uint64_t rel=lo-start;uint32_t b=u32(p+4*(rel/v->block));
        uint32_t skip=(uint32_t)(rel%v->block),n=v->block-skip;
        if(n>hi-lo)n=(uint32_t)(hi-lo);
        if(cancelled(v))return IFS_FS_BUSY;
        if(!b){if(r->node->type!=IFS_FS_ENTRY_FILE)return IFS_FS_CORRUPT;zero(r->out+(size_t)(lo-r->at),n);}
        else{
            if(!usable(v,b)||b==v->root)return IFS_FS_CORRUPT;
            int rc=rd(v,(uint64_t)b*v->block+skip,r->out+(size_t)(lo-r->at),n);if(rc)return rc;
        }
        lo+=n;
    }
    r->covered=hi;return IFS_FS_OK;
}
static int file_read(const Volume *v,const Node *n,uint64_t at,void *out,uint32_t bytes)
{
    if((!out&&bytes)||at>n->bytes||bytes>n->bytes-at)return IFS_FS_INVALID;
    if(!bytes)return IFS_FS_OK;
    Read r={.node=n,.out=out,.at=at,.end=at+bytes,.covered=at};
    int rc=items(v,n->dir,n->id,data_item,&r);
    return rc?rc:r.covered==r.end?IFS_FS_OK:IFS_FS_CORRUPT;
}
static int discover(const IfsFsHostRuntimeV2 *h,Volume *v)
{
    if(!ifs_fs_host_v2_valid(h))return IFS_FS_INVALID;
    if(h->volume_bytes<65536+128)return IFS_FS_NOT_FOUND;
    v->host=*h;uint8_t sb[128];
    int rc=rd(v,65536,sb,sizeof(sb));if(rc)return rc;
    if(same(sb+52,"ReIsErFs",8))v->variant=1;
    else if(same(sb+52,"ReIsEr2Fs",9))v->variant=2;
    else if(same(sb+52,"ReIsEr3Fs",9))return IFS_FS_UNSUPPORTED;
    else return IFS_FS_NOT_FOUND;
    v->block=u16(sb+44);v->blocks=u32(sb);v->free_blocks=u32(sb+4);v->root=u32(sb+8);
    v->journal=u32(sb+12);v->jlen=u32(sb+20);
    uint32_t height=u16(sb+68);
    if((v->block!=1024&&v->block!=2048&&v->block!=4096)||
       v->blocks>MAX_BITMAP*8||v->blocks>h->volume_bytes/v->block||
       v->free_blocks>=v->blocks||height<2||height>MAX_DEPTH+1)return IFS_FS_CORRUPT;
    v->height=height-1;v->super=65536/v->block;
    if(v->blocks<=v->super+2||!v->jlen||v->journal<=v->super+1||
       v->journal>=v->blocks||v->jlen>=v->blocks-v->journal||
       v->jlen>65536||u32(sb+16)||u16(sb+50)!=1||u16(sb+62)||
       (v->variant==1?u16(sb+72)!=0:u16(sb+72)!=2))return IFS_FS_UNSUPPORTED;
    if(v->variant==2)copy(v->label,sb+100,16);
    uint32_t groups=(v->blocks-1)/(v->block*8)+1;
    if(u16(sb+70)!=groups)return IFS_FS_CORRUPT;
    size_t bytes=(v->blocks+7u)/8u;
    v->bitmap=h->allocate(h->context,bytes);if(!v->bitmap)return IFS_FS_NO_MEMORY;
    for(uint32_t i=0;i<groups;++i){
        uint32_t b=i?i*v->block*8:v->super+1;
        size_t at=(size_t)i*v->block;uint32_t n=(uint32_t)(bytes-at);
        if(n>v->block)n=v->block;
        rc=rd(v,(uint64_t)b*v->block,v->bitmap+at,n);if(rc)goto fail;
    }
    uint32_t free=0;
    for(uint32_t i=0;i<v->blocks;++i){
        if(!(i&32767)&&cancelled(v)){rc=IFS_FS_BUSY;goto fail;}
        if(!allocated(v,i))++free;
    }
    if(free!=v->free_blocks||!usable(v,v->root)){rc=IFS_FS_CORRUPT;goto fail;}
    for(uint32_t b=0;b<=v->super+1;++b)if(!allocated(v,b)){rc=IFS_FS_CORRUPT;goto fail;}
    for(uint32_t i=1;i<groups;++i)if(!allocated(v,i*v->block*8)){rc=IFS_FS_CORRUPT;goto fail;}
    for(uint32_t i=0;i<=v->jlen;++i)if(!allocated(v,v->journal+i)){rc=IFS_FS_CORRUPT;goto fail;}
    /* Refuse unflushed journal descriptors; do not replay on a read-only
     * host. Old flushed transactions may remain in the circular journal. */
    uint8_t jh[12],desc[MAX_BLOCK];
    rc=rd(v,(uint64_t)(v->journal+v->jlen)*v->block,jh,12);if(rc)goto fail;
    if(u32(jh+4)>=v->jlen){rc=IFS_FS_CORRUPT;goto fail;}
    rc=rd(v,(uint64_t)(v->journal+u32(jh+4))*v->block,desc,v->block);if(rc)goto fail;
    if(same(desc+v->block-12,"ReIsErLB",8)&&u32(desc+4)&&
       (u32(desc)>u32(jh)||u32(desc+8)>u32(jh+8))){rc=IFS_FS_UNSUPPORTED;goto fail;}
    Node root;rc=inode(v,1,2,&root);if(rc)goto fail;
    if(root.type!=IFS_FS_ENTRY_DIRECTORY){rc=IFS_FS_CORRUPT;goto fail;}
    Dir d={.dir=&root,.index=UINT32_MAX};rc=items(v,1,2,dir_item,&d);
    if(!rc&&d.dots!=3)rc=IFS_FS_CORRUPT;
    if(rc)goto fail;
    return IFS_FS_OK;
fail:dispose(v);return rc;
}
static Volume *mounted(void *p)
{if(p)for(unsigned i=0;i<SLOTS;++i)if(slots[i].token==(uintptr_t)p)return &slots[i];return NULL;}
static void info(const Volume *v,IfsFsVolumeInfoV2 *o)
{*o=(IfsFsVolumeInfoV2){.structure_bytes=sizeof(*o),.variant=v->variant,.max_name_bytes=MAX_NAME,
.block_bytes=v->block,.total_bytes=(uint64_t)v->blocks*v->block,.free_bytes=(uint64_t)v->free_blocks*v->block};}
static int probe_impl(const IfsFsHostRuntimeV2 *h,uint32_t *variant)
{if(!variant)return IFS_FS_INVALID;Volume v={0};int rc=discover(h,&v);if(!rc)*variant=v.variant;dispose(&v);return rc;}
static int mount_impl(const IfsFsHostRuntimeV2 *h,void **out,IfsFsVolumeInfoV2 *o)
{
    if(!out||!o||o->structure_bytes<sizeof(*o))return IFS_FS_INVALID;
    *out=NULL;if(!ifs_fs_host_v2_valid(h))return IFS_FS_INVALID;
    if(!(h->capabilities&IFS_FS_HOST_READ_ONLY))return IFS_FS_ACCESS;
    unsigned i=0;while(i<SLOTS&&slots[i].token)++i;
    if(i==SLOTS||generation==UINTPTR_MAX)return IFS_FS_BUSY;
    int rc=discover(h,&slots[i]);if(rc)return rc;
    slots[i].token=++generation;*out=(void *)slots[i].token;info(&slots[i],o);return IFS_FS_OK;
}
static int unmount_impl(void *p)
{Volume *v=mounted(p);if(!v)return IFS_FS_INVALID;dispose(v);return IFS_FS_OK;}
static int resolve(const Volume *v,const char *path,Entry *out)
{
    if(!path||*path!='/')return IFS_FS_INVALID;
    Entry e={.length=1,.name="/"};int rc=inode(v,1,2,&e.node);if(rc)return rc;
    ++path;
    for(unsigned depth=0;*path;++depth){
        if(depth>=256)return IFS_FS_INVALID;
        uint32_t n=0;while(n<=MAX_NAME&&path[n]&&path[n]!='/')++n;
        if(!n||n>MAX_NAME||(n==1&&path[0]=='.')||(n==2&&same(path,"..",2)))return IFS_FS_INVALID;
        Node dir=e.node;rc=child(v,&dir,path,n,0,&e);if(rc)return rc;
        path+=n;if(*path=='/'){++path;if(!*path)return IFS_FS_INVALID;}
    }
    *out=e;return IFS_FS_OK;
}
static int emit(const Entry *e,IfsFsEntryV2 *o)
{
    if(!o||o->structure_bytes<sizeof(*o)||!o->name)return IFS_FS_INVALID;
    if(o->name_capacity<=e->length)return IFS_FS_NO_SPACE;
    char *name=o->name;uint32_t cap=o->name_capacity;
    *o=(IfsFsEntryV2){.structure_bytes=sizeof(*o),.type=e->node.type,.size_bytes=e->node.bytes,
      .name=name,.name_capacity=cap,.name_bytes=e->length};
    copy(name,e->name,e->length+1);return IFS_FS_OK;
}
static int stat_impl(void *p,const char *path,IfsFsEntryV2 *o)
{Volume *v=mounted(p);if(!v)return IFS_FS_INVALID;Entry e;int rc=resolve(v,path,&e);return rc?rc:emit(&e,o);}
static int list_impl(void *p,const char *path,uint32_t i,IfsFsEntryV2 *o)
{Volume *v=mounted(p);if(!v)return IFS_FS_INVALID;Entry dir,e;int rc=resolve(v,path,&dir);if(!rc)rc=child(v,&dir.node,NULL,0,i,&e);return rc?rc:emit(&e,o);}
static int read_impl(void *p,const char *path,uint64_t at,void *out,uint32_t n)
{Volume *v=mounted(p);if(!v)return IFS_FS_INVALID;Entry e;int rc=resolve(v,path,&e);if(rc)return rc;return e.node.type==IFS_FS_ENTRY_FILE?file_read(v,&e.node,at,out,n):IFS_FS_UNSUPPORTED;}
static int readlink_impl(void *p,const char *path,char *out,uint32_t cap,uint32_t *n)
{
    if(!n)return IFS_FS_INVALID;
    *n=0;Volume *v=mounted(p);if(!v||!out)return IFS_FS_INVALID;
    Entry e;int rc=resolve(v,path,&e);if(rc)return rc;
    if(e.node.type!=IFS_FS_ENTRY_LINK)return IFS_FS_INVALID;
    if(!e.node.bytes||e.node.bytes>=cap||e.node.bytes>4095)return IFS_FS_NO_SPACE;
    uint32_t len=(uint32_t)e.node.bytes;
    char target[4096];rc=file_read(v,&e.node,0,target,len);if(rc)return rc;
    for(uint32_t i=0;i<len;++i)if(!target[i])return IFS_FS_CORRUPT;
    copy(out,target,len);out[len]=0;*n=len;return IFS_FS_OK;
}
static int query_impl(void *p,IfsFsVolumeInfoV2 *o)
{Volume *v=mounted(p);if(!v||!o||o->structure_bytes<sizeof(*o))return IFS_FS_INVALID;info(v,o);return IFS_FS_OK;}
static int metadata_impl(void *p,const char *path,IfsFsMetadataV2 *o)
{
    Volume *v=mounted(p);if(!v||!o||o->structure_bytes<sizeof(*o))return IFS_FS_INVALID;
    Entry e;int rc=resolve(v,path,&e);if(rc)return rc;
    *o=(IfsFsMetadataV2){.structure_bytes=sizeof(*o),.mask=IFS_FS_META_GET_MODE,
       .mode=e.node.mode,.owner=e.node.uid,.group=e.node.gid};return IFS_FS_OK;
}
static int label_impl(void *p,char *out,uint32_t cap,uint32_t *n)
{
    if(!n)return IFS_FS_INVALID;
    *n=0;Volume *v=mounted(p);if(!v||!out)return IFS_FS_INVALID;
    uint32_t len=0;while(len<16&&v->label[len])++len;
    if(cap<=len)return IFS_FS_NO_SPACE;
    copy(out,v->label,len);out[len]=0;*n=len;return IFS_FS_OK;
}
#define WRAP(name,args,call) static int name args{if(!enter())return IFS_FS_BUSY;return leave(call);}
WRAP(probe,(const IfsFsHostRuntimeV2 *h,uint32_t *v),probe_impl(h,v))
WRAP(mount,(const IfsFsHostRuntimeV2 *h,void **p,IfsFsVolumeInfoV2 *o),mount_impl(h,p,o))
WRAP(unmount,(void *p),unmount_impl(p))
WRAP(stat_path,(void *p,const char *s,IfsFsEntryV2 *o),stat_impl(p,s,o))
WRAP(list_path,(void *p,const char *s,uint32_t i,IfsFsEntryV2 *o),list_impl(p,s,i,o))
WRAP(read_path,(void *p,const char *s,uint64_t a,void *b,uint32_t n),read_impl(p,s,a,b,n))
WRAP(readlink_path,(void *p,const char *s,char *b,uint32_t c,uint32_t *n),readlink_impl(p,s,b,c,n))
WRAP(query,(void *p,IfsFsVolumeInfoV2 *o),query_impl(p,o))
WRAP(metadata,(void *p,const char *s,IfsFsMetadataV2 *o),metadata_impl(p,s,o))
WRAP(label_path,(void *p,char *s,uint32_t c,uint32_t *n),label_impl(p,s,c,n))
static const IfsFsCoreOpsV2 core={
 .header={2,sizeof(core),IFS_FS_GROUP_CORE,0,IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT|IFS_FS_CORE_STAT|IFS_FS_CORE_LIST|IFS_FS_CORE_READ},
 .probe=probe,.mount=mount,.unmount=unmount,.stat=stat_path,.directory_entry=list_path,.read=read_path};
static const IfsFsVolumeOpsV2 volume={.header={2,sizeof(volume),IFS_FS_GROUP_VOLUME,0,IFS_FS_VOLUME_QUERY|IFS_FS_VOLUME_LABEL_GET},.query=query,.label=label_path};
static const IfsFsMetadataOpsV2 meta={.header={2,sizeof(meta),IFS_FS_GROUP_METADATA,0,IFS_FS_META_GET_MODE|IFS_FS_META_READLINK},.get=metadata,.readlink=readlink_path};
static const IfsFsGroupHeaderV2 *group(uint32_t id,uint32_t lo,uint32_t hi)
{if(lo>2||hi<2)return NULL;if(id==IFS_FS_GROUP_CORE)return &core.header;if(id==IFS_FS_GROUP_VOLUME)return &volume.header;if(id==IFS_FS_GROUP_METADATA)return &meta.header;return NULL;}
static const IfsFsModuleApiV2 api={.abi_version=2,.structure_bytes=sizeof(api),.format_id="reiserfs",
#ifdef IFS_BOOT_FIRMWARE
 .runtime_profiles=IFS_FS_PROFILE_FREESTANDING|IFS_FS_PROFILE_FIRMWARE,
#else
 .runtime_profiles=IFS_FS_PROFILE_HOSTED,
#endif
 .group_mask=IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE)|IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME)|IFS_FS_GROUP_BIT(IFS_FS_GROUP_METADATA),.query_group=group};
__attribute__((visibility("default")))const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void){return &api;}
