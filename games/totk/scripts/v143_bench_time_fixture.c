/* Production launch patch fixture: dataless stand-ins or player-supplied seeds.
 * No game code is stored here; every failure must precede every module write. */
#include <assert.h>
#include <setjmp.h>
#include "../runtime/game_patches_v143.c"
#include "fixture_seeds.h"
static jmp_buf failure;
_Noreturn void runtime_fail(const char *s){(void)s;longjmp(failure,1);}
int mfxup_output_enabled(void){return 0;}
void runtime_oe_push_message(uint32_t m){(void)m;}
void dm_gfx_set_flag(const char *s,int v){(void)s;(void)v;}
void runtime_survey_init(Module *m){(void)m;}
static Module m;
static unsigned char *before;
static void reset(void){
 memset(m.base,0,m.size);
 for(unsigned i=0;i<sizeof bench_time_sites/sizeof *bench_time_sites;i++)fixture_seed_at(m.base,bench_time_sites[i]);
 unsetenv("TOTK_BENCH_TIME");
}
static void reject(void){
 memcpy(before,m.base,m.size);
 if(!setjmp(failure)){patch_bench_time(&m);assert(!"accepted invalid image");}
 assert(!memcmp(before,m.base,m.size));
}
static int real_image(const char *path){
 FILE *f=fopen(path,"rb");assert(f&&!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>0);rewind(f);
 size_t size=(((size_t)n+0x3fff)&~(size_t)0x3fff)+0x4000;
 unsigned char *base=calloc(1,size),*snapshot=malloc(size);assert(base&&snapshot);
 assert(fread(base,1,(size_t)n,f)==(size_t)n&&!fclose(f));
 char digest[65];host_sha256_hex(base,(size_t)n,digest);
 assert(!strcmp(digest,"bad348845b8f786edfe27c3c44b97528727602e33fea978dd5e55987a981c5ee"));
 Module real={.name="main",.digest=digest,.base=base,.size=size,.file_size=(size_t)n,
  .text_end=0x2bb9c00,.key_off=61734912,.patch_off=61800448};
 memcpy(snapshot,base,size);unsetenv("TOTK_BENCH_TIME");patch_bench_time(&real);assert(!memcmp(snapshot,base,size));
 setenv("TOTK_BENCH_TIME","off",1);patch_bench_time(&real);assert(!memcmp(snapshot,base,size));
 setenv("TOTK_BENCH_TIME","23:59",1);patch_bench_time(&real);
 size_t stub=(((size_t)n+15)&~(size_t)15)+128,changed=0;
 for(size_t i=0;i<size;i++)if(base[i]!=snapshot[i]){
  assert((i>=stub&&i<stub+12)||(i>=0x68c124&&i<0x68c128)||(i>=0x68c76c&&i<0x68c770));changed++;
 }
 float expected=86340;assert(!memcmp(base+stub+8,&expected,4));
 printf("{\"off_byte_identity\":true,\"file_bytes\":%ld,\"stub_offset\":%zu,\"changed_bytes\":%zu,\"effective_seconds\":86340}\n",n,stub,changed);
 free(snapshot);free(base);return 0;
}
int main(int argc,char **argv){
 if(argc==3&&!strcmp(argv[1],"--image"))return real_image(argv[2]);
 m=(Module){.name="main",.digest="bad348845b8f786edfe27c3c44b97528727602e33fea978dd5e55987a981c5ee",
  .size=0x700000,.file_size=0x6f0123,.text_end=0x690100,.key_off=0x6a0000,.patch_off=0x6b0000};
 m.base=calloc(1,m.size);before=malloc(m.size);assert(m.base&&before);reset();
 if(argc==3&&!strcmp(argv[1],"--value")){
  if(strcmp(argv[2],"absent"))setenv("TOTK_BENCH_TIME",argv[2],1);
  memcpy(before,m.base,m.size);patch_bench_time(&m);
  if(bench_time_parse(getenv("TOTK_BENCH_TIME"),&(unsigned){0})<=0)assert(!memcmp(before,m.base,m.size));
  else{
   unsigned seconds=0;assert(bench_time_parse(getenv("TOTK_BENCH_TIME"),&seconds)==1);
   float expected=(float)seconds;
   uint64_t stub=((m.file_size+15)&~15ULL)+128;
   assert(!memcmp(m.base+stub+8,&expected,4));
  }
  free(before);free(m.base);return 0;
 }
 unsigned seconds=123;
 assert(bench_time_parse(NULL,&seconds)==0&&seconds==123);
 assert(bench_time_parse("off",&seconds)==0&&seconds==123);
 for(unsigned h=0;h<24;h++)for(unsigned n=0;n<60;n++){
  char text[6];snprintf(text,sizeof text,"%02u:%02u",h,n);
  assert(bench_time_parse(text,&seconds)==1&&seconds==h*3600+n*60);
 }
 const char *bad[]={"","0","OFF","Off","on","00:0","0:00","000:00","00:000","24:00","23:60","-1:00","+1:00"," 1:00","01:00 ","01:00\n","01:00:00","01.00","aa:bb","12:3x","nan","inf","\xff\xff:00"};
 memcpy(before,m.base,m.size);patch_bench_time(&m);assert(!memcmp(before,m.base,m.size));
 setenv("TOTK_BENCH_TIME","off",1);patch_bench_time(&m);assert(!memcmp(before,m.base,m.size));
 for(unsigned i=0;i<sizeof bad/sizeof *bad;i++){
  seconds=123;assert(bench_time_parse(bad[i],&seconds)==-1&&seconds==123);
  setenv("TOTK_BENCH_TIME",bad[i],1);patch_bench_time(&m);assert(!memcmp(before,m.base,m.size));
 }
 for(unsigned i=0;i<sizeof bench_time_sites/sizeof *bench_time_sites;i++){
  reset();setenv("TOTK_BENCH_TIME","02:00",1);m.base[bench_time_sites[i]]^=1;reject();
 }
 reset();setenv("TOTK_BENCH_TIME","02:00",1);
 m.name="sdk";reject();m.name="main";
 m.digest=NULL;reject();m.digest="invalid";reject();
 m.digest="bad348845b8f786edfe27c3c44b97528727602e33fea978dd5e55987a981c5ee";
 size_t size=m.size,file_size=m.file_size;uint64_t end=m.text_end,patch=m.patch_off;
 m.text_end=0x68c770;reject();m.text_end=end;
 m.patch_off=m.file_size;reject();m.patch_off=patch;
 m.file_size=UINT64_MAX;reject();m.file_size=file_size;
 m.size=0x6f01b8;reject();m.size=size;
 m.file_size=0x6fffc0;reject();m.file_size=file_size;
 uint64_t stub=((m.file_size+15)&~15ULL)+128;
 for(unsigned i=0;i<12;i++){m.base[stub+i]=1;reject();m.base[stub+i]=0;}
 memcpy(before,m.base,m.size);patch_bench_time(&m);
 for(size_t i=0;i<m.size;i++)if(m.base[i]!=before[i])assert(
  (i>=0x68c124&&i<0x68c128)||(i>=0x68c76c&&i<0x68c770)||(i>=stub&&i<stub+12));
 uint32_t literal;memcpy(&literal,m.base+stub+8,4);float expected=7200;assert(!memcmp(&literal,&expected,4));
 reject(); /* Never stack a patch on already modified words. */
 if(argc==2){FILE *f=fopen(argv[1],"wb");assert(f);assert(fwrite(m.base,1,m.size,f)==m.size);assert(!fclose(f));}
 free(before);free(m.base);
 puts("benchmark time: PASS (1440 valid times, 23 rejected values, 45 word corruptions, off-byte identity, module/bounds/padding/retry refusal)");return 0;
}
