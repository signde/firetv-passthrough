/* Run with AddressSanitizer/UBSan against a captured complete DTS-HD unit. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "native.c"

int main(int argc, char **argv) {
  assert(argc == 2);
  FILE *f = fopen(argv[1], "rb"); assert(f);
  uint8_t *input = malloc(RAW_CAP); assert(input);
  size_t count = fread(input, 1, RAW_CAP, f); fclose(f);
  assert(count > 2048);
  uint32_t core = (((uint32_t)(input[5]&3)<<12)|((uint32_t)input[6]<<4)|(input[7]>>4))+1;
  assert(be32(input+core)==0x64582025u);
  uint32_t unit = core + exsize(input+core); assert(unit < count);
  struct state *s = malloc(sizeof(*s)); assert(s);
  uint8_t *out = malloc(32768), *reference = malloc(32768); assert(out && reference);
  uint32_t n; int rc;

  dtshd_reset(s);
  assert(dtshd_process(s,input,core,out,32768,&n,0)==0 && n==0);
  assert(dtshd_process(s,input+core,unit-core,out,32768,&n,0)==0 && n==0);
  assert(dtshd_process(s,input,0,out,32768,&n,1)==0 && n==32768);
  memcpy(reference,out,32768);
  assert(dtshd_pending(s)==0 && s->frames==1);

  /* A seek can interrupt a core or its lossless extension. Resetting at an
   * observed flush must discard those bytes before accepting the new unit.
   * Split the new header to exercise normal streaming after discontinuity.
   */
  const uint32_t cuts[]={1,3,9,core-1,core,core+5,unit-1};
  for(size_t c=0;c<sizeof(cuts)/sizeof(cuts[0]);c++) {
    dtshd_reset(s);
    assert(dtshd_process(s,input,cuts[c],out,32768,&n,0)==0 && n==0);
    dtshd_discontinuity(s);
    assert(dtshd_pending(s)==0 && s->error==0);
    assert(dtshd_process(s,input,3,out,32768,&n,0)==0 && n==0);
    assert(dtshd_process(s,input+3,unit-3,out,32768,&n,1)==0 && n==32768);
    assert(memcmp(out,reference,32768)==0);
    dtshd_discontinuity(s);
    assert(s->frames==1 && s->bytes==32768);
    assert(dtshd_process(s,input,unit,out,32768,&n,1)==0 && n==32768);
    assert(memcmp(out,reference,32768)==0 && s->frames==2);
  }

  /* An error clears only on an explicit reset/discontinuity. */
  dtshd_reset(s);
  uint8_t invalid[10]={0};
  assert(dtshd_process(s,invalid,sizeof(invalid),out,32768,&n,0)==-2);
  dtshd_discontinuity(s);
  assert(dtshd_process(s,input,unit,out,32768,&n,1)==0 && n==32768);
  assert(memcmp(out,reference,32768)==0);

  dtshd_reset(s);
  assert(dtshd_process(s,input,unit-1,out,32768,&n,1)==0 && n==0);
  assert(dtshd_pending(s)==unit-1);
  assert(dtshd_process(s,input+unit-1,1,out,32768,&n,1)==0 && n==32768);
  assert(memcmp(out,reference,32768)==0);

  dtshd_reset(s);
  assert(dtshd_process(s,input,unit,out,32767,&n,1)==-5 && n==0);
  assert(dtshd_process(s,input,0,out,32768,&n,1)==-5 && n==0);
  dtshd_reset(s);
  assert(dtshd_process(s,input,unit,out,32768,&n,1)==0 && n==32768);

  dtshd_reset(s);
  uint8_t saved = input[8]; input[8]=(saved & ~0x3c)|(8<<2);
  assert(dtshd_process(s,input,unit,out,32768,&n,1)==-3 && n==0);
  input[8]=saved;
  dtshd_reset(s);
  input[0]^=1;
  assert(dtshd_process(s,input,unit,out,32768,&n,1)==-2 && n==0);
  input[0]^=1;

  dtshd_reset(s);
  s->used=RAW_CAP;
  assert(dtshd_process(s,input,1,out,32768,&n,0)==-1 && n==0);
  dtshd_reset(s);
  memcpy(input+core,input,4);
  assert(dtshd_process(s,input,core+4,out,32768,&n,0)==-6 && n==0);

  /* A complete but oversized extension must reject without a buffer write. */
  memset(input+core,0,33000);
  input[core]=0x64; input[core+1]=0x58; input[core+2]=0x20; input[core+3]=0x25;
  uint32_t ext=32999;
  input[core+6]=(ext>>11)&31; input[core+7]=(ext>>3)&255; input[core+8]=(ext&7)<<5;
  dtshd_reset(s);
  memset(out,0xa5,32768);
  rc=dtshd_process(s,input,core+33000,out,32768,&n,1);
  assert(rc==-4 && n==0);
  for(uint32_t i=0;i<32768;i++) assert(out[i]==0xa5);
  dtshd_clear(out,32768);
  for(uint32_t i=0;i<32768;i++) assert(out[i]==0);
  free(reference); free(out); free(s); free(input);
  puts("Native C boundary, truncation, discontinuity, capacity, framing, error latch/reset and mute tests passed.");
  return 0;
}
