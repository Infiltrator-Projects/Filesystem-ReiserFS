/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <infiltratr/filesystem_module_v2.h>
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
enum { B=4096,N=256,MAX=B*N };
static uint8_t disk[MAX],saved[MAX];static unsigned sector,allocs,fail;
static void p16(uint8_t*p,uint32_t n){p[0]=n;p[1]=n>>8;}
static void p32(uint8_t*p,uint32_t n){p16(p,n);p16(p+2,n>>16);}
static void p64(uint8_t*p,uint64_t n){p32(p,n);p32(p+4,n>>32);}
static uint8_t *blk(unsigned n){return disk+n*B;}
static void key(uint8_t*p,unsigned dir,unsigned id,uint64_t off,unsigned type,unsigned ver)
{p32(p,dir);p32(p+4,id);if(ver)p64(p+8,off|((uint64_t)type<<60));else{p32(p+8,(uint32_t)off);p32(p+12,type==0?0:type==1?UINT32_MAX-1:type==2?UINT32_MAX:500);}}
static void leaf(uint8_t*p,unsigned i,unsigned dir,unsigned id,uint64_t off,unsigned type,unsigned ver,unsigned aux,const void*data,unsigned len)
{
    unsigned end=i?((unsigned)p[24+(i-1)*24+20]|(unsigned)p[24+(i-1)*24+21]<<8):B;
    uint8_t*h=p+24+i*24;key(h,dir,id,off,type,ver);p16(h+16,aux);p16(h+18,len);p16(h+20,end-len);p16(h+22,ver);
    memcpy(p+end-len,data,len);p16(p,1);p16(p+2,i+1);p16(p+4,end-len-24-(i+1)*24);
}
static void statdata(uint8_t*p,unsigned ver,unsigned mode,uint64_t size)
{memset(p,0,44);p16(p,mode);if(ver){p32(p+4,1);p64(p+8,size);p32(p+16,100);p32(p+20,200);}else{p16(p+2,1);p16(p+4,100);p16(p+6,200);p32(p+8,(uint32_t)size);}}
static void dent(uint8_t*p,unsigned i,unsigned off,unsigned dir,unsigned id,unsigned loc,const char*name)
{uint8_t*h=p+16*i;p32(h,off);p32(h+4,dir);p32(h+8,id);p16(h+12,loc);p16(h+14,4);memcpy(p+loc,name,strlen(name));}
static void fixture(unsigned ver)
{
    memset(disk,0,sizeof disk);
    uint8_t*sb=blk(16);p32(sb,N);p32(sb+4,N-35);p32(sb+8,30);
    p32(sb+12,18);p32(sb+20,10);p16(sb+44,B);p16(sb+50,1);
    memcpy(sb+52,ver?"ReIsEr2Fs":"ReIsErFs",ver?9:8);p16(sb+68,3);p16(sb+70,1);p16(sb+72,ver?2:0);
    if(ver)memcpy(sb+100,"REISER",6);
    for(unsigned i=0;i<35;++i)blk(17)[i/8]|=1u<<(i%8);
    uint8_t*root=blk(30);p16(root,2);p16(root+2,1);p16(root+4,B-24-16-16);
    key(root+24,2,3,0,0,ver);p32(root+40,31);p32(root+48,32);
    uint8_t s[44],d[128]={0};unsigned sl=ver?44:32;
    statdata(s,ver,0040755,86);leaf(blk(31),0,1,2,0,0,ver,UINT16_MAX,s,sl);
    dent(d,0,1,1,2,85,".");dent(d,1,2,0,1,83,".."); /* root parent fixed below */
    p32(d+16+4,1);
    dent(d,2,128,2,3,74,"proof.bin");dent(d,3,256,2,4,70,"link");
    /* four headers + packed names with trailing zero padding */
    memmove(d+64,d+70,16); /* reconstruct exact compact name storage */
    memset(d+64,0,22);dent(d,0,1,1,2,85,".");dent(d,1,2,1,2,83,"..");
    dent(d,2,128,2,3,74,"proof.bin");dent(d,3,256,2,4,64,"link");
    leaf(blk(31),1,1,2,1,3,0,4,d,86);
    p16(root+44,B-24-((unsigned)blk(31)[4]|(unsigned)blk(31)[5]<<8));
    statdata(s,ver,0100644,2*B+7);leaf(blk(32),0,2,3,0,0,ver,UINT16_MAX,s,sl);
    uint8_t ptr[8]={0};p32(ptr,33);leaf(blk(32),1,2,3,1,1,ver,0,ptr,8);
    leaf(blk(32),2,2,3,2*B+1,2,ver,UINT16_MAX,"tail123",7);
    statdata(s,ver,0120777,9);leaf(blk(32),3,2,4,0,0,ver,UINT16_MAX,s,sl);
    leaf(blk(32),4,2,4,1,2,ver,UINT16_MAX,"proof.bin",9);
    p16(root+52,B-24-((unsigned)blk(32)[4]|(unsigned)blk(32)[5]<<8));
    for(unsigned i=0;i<B;++i)blk(33)[i]=(uint8_t)(i*13+7);
    memcpy(saved,disk,sizeof disk);
}
static int rd(void*c,uint64_t off,void*out,uint32_t n)
{(void)c;if(fail||off>sizeof disk||n>sizeof disk-off||off%sector||n%sector)return -1;memcpy(out,disk+off,n);return 0;}
static void*alloc(void*c,size_t n){(void)c;void*p=malloc(n);if(p)++allocs;return p;}
static void drop(void*c,void*p){(void)c;if(p){assert(allocs);--allocs;free(p);}}
static int real_rd(void*c,uint64_t off,void*out,uint32_t n)
{
    FILE *f=c;if(off%sector||n%sector||off>INT64_MAX||fseek(f,(long)off,SEEK_SET))return -1;
    return fread(out,1,n,f)==n?0:-1;
}
static int real_test(const IfsFsCoreOpsV2 *core,const char *path,unsigned want)
{
    FILE *f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long size=ftell(f);assert(size>0);
    for(sector=512;sector<=4096;sector*=8){
        IfsFsHostRuntimeV2 h={.abi_version=2,.structure_bytes=sizeof h,.capabilities=IFS_FS_HOST_READ_ONLY,
           .volume_bytes=(uint64_t)size,.logical_sector_bytes=sector,.physical_sector_bytes=sector,
           .maximum_transfer_bytes=4096,.context=f,.read_at=real_rd,.allocate=alloc,.release=drop};
        uint32_t variant=0;int rc=core->probe(&h,&variant);
        if(rc){fprintf(stderr,"real probe rc=%d sector=%u\n",rc,sector);return 1;}
        assert(variant==want&&!allocs);
        void *v=NULL;IfsFsVolumeInfoV2 i={.structure_bytes=sizeof i};assert(core->mount(&h,&v,&i)==0&&v);
        char name[256];IfsFsEntryV2 e={.structure_bytes=sizeof e,.name=name,.name_capacity=sizeof name};
        assert(core->stat(v,"/",&e)==0&&e.type==IFS_FS_ENTRY_DIRECTORY);
        assert(core->directory_entry(v,"/",0,&e)==IFS_FS_END);
        assert(core->unmount(v)==0&&!allocs);
    }
    assert(!fclose(f));puts("Independent mkreiserfs root and allocation geometry PASS");return 0;
}
int main(int argc,char**argv)
{
    if(argc==4&&!strcmp(argv[1],"--emit")){
        fixture(1);FILE*f=fopen(argv[2],"wb"),*p=fopen(argv[3],"wb");assert(f&&p);
        assert(fwrite(disk,1,sizeof disk,f)==sizeof disk);assert(fwrite(blk(33),1,B,p)==B);
        uint8_t z[B]={0};assert(fwrite(z,1,B,p)==B);assert(fwrite("tail123",1,7,p)==7);assert(!fclose(f)&&!fclose(p));return 0;
    }
    assert(argc==2||argc==4);void*lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!lib){puts(dlerror());return 1;}
    IfsFsModuleApiEntryV2 entry;*(void**)(&entry)=dlsym(lib,IFS_FS_COMMON_EXPORT_V2);assert(entry);
    const IfsFsModuleApiV2*api=entry();assert(ifs_fs_api_v2_valid(api,"reiserfs"));
    const IfsFsCoreOpsV2*core=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsMetadataOpsV2*meta=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_METADATA);assert(core&&meta&&!core->write);
    if(argc==4){int rc=real_test(core,argv[2],(unsigned)strtoul(argv[3],NULL,10));dlclose(lib);return rc;}
    for(unsigned ver=0;ver<2;++ver)for(sector=512;sector<=4096;sector*=8){
        fixture(ver);IfsFsHostRuntimeV2 h={.abi_version=2,.structure_bytes=sizeof(h),.capabilities=IFS_FS_HOST_READ_ONLY,
          .volume_bytes=sizeof disk,.logical_sector_bytes=sector,.physical_sector_bytes=sector,.maximum_transfer_bytes=B,
          .read_at=rd,.allocate=alloc,.release=drop};
        uint32_t variant=0;assert(core->probe(&h,&variant)==0&&variant==ver+1&&!allocs);
        void*v=NULL;IfsFsVolumeInfoV2 info={.structure_bytes=sizeof info};assert(core->mount(&h,&v,&info)==0&&v);
        char name[64];IfsFsEntryV2 e={.structure_bytes=sizeof e,.name=name,.name_capacity=sizeof name};
        assert(core->directory_entry(v,"/",0,&e)==0&&!strcmp(name,"proof.bin"));
        assert(core->directory_entry(v,"/",1,&e)==0&&!strcmp(name,"link"));
        assert(core->directory_entry(v,"/",2,&e)==IFS_FS_END);
        assert(core->stat(v,"/proof.bin",&e)==0&&e.size_bytes==2*B+7);
        uint8_t out[2*B+7];assert(core->read(v,"/proof.bin",0,out,sizeof out)==0&&!memcmp(out,blk(33),B));
        for(unsigned i=B;i<2*B;++i)assert(!out[i]);
        assert(!memcmp(out+2*B,"tail123",7));
        uint32_t n;assert(meta->readlink(v,"/link",name,sizeof name,&n)==0&&n==9&&!strcmp(name,"proof.bin"));
        p32(blk(30)+48,31);assert(core->stat(v,"/proof.bin",&e)==IFS_FS_CORRUPT);memcpy(disk,saved,sizeof disk);
        p16(blk(32)+24+20,0);assert(core->stat(v,"/proof.bin",&e)==IFS_FS_CORRUPT);memcpy(disk,saved,sizeof disk);
        fail=1;assert(core->stat(v,"/",&e)!=0);fail=0;
        assert(core->unmount(v)==0&&core->unmount(v)==IFS_FS_INVALID&&!allocs);
        p16(blk(16)+50,2);assert(core->probe(&h,&variant)==IFS_FS_UNSUPPORTED&&!allocs);memcpy(disk,saved,sizeof disk);
        p32(blk(18),2);p32(blk(18)+4,1);memcpy(blk(18)+B-12,"ReIsErLB",8);
        assert(core->probe(&h,&variant)==IFS_FS_UNSUPPORTED&&!allocs);memcpy(disk,saved,sizeof disk);
        assert(!memcmp(disk,saved,sizeof disk));
    }
    puts("ReiserFS mixed key formats, tree splits, sparse and packed tails PASS");dlclose(lib);return 0;
}
