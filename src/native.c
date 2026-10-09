/* Bounded C ABI prototype for a temporary Gazelle experiment, not a HAL library.
 * All writable state is supplied by the caller. No allocator or global state.
 * Errors latch until reset; no partial output is returned on error.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define RAW_CAP (1024u * 1024u)
struct state { uint32_t used, error, frames, bytes; uint8_t raw[RAW_CAP]; };
uint32_t dtshd_state_size(void) { return sizeof(struct state); }
void dtshd_reset(struct state *s) { s->used=s->error=s->frames=s->bytes=0; }
/* Only call at a confirmed stream discontinuity with an aligned next access
 * unit. Never append a post-seek unit to a pre-seek partial core/extension.
 * Keep cumulative counters for the same encoder's diagnostics. This does not
 * scan for sync words inside arbitrary compressed payloads.
 */
void dtshd_discontinuity(struct state *s) { s->used=s->error=0; }
void dtshd_clear(uint8_t *out, uint32_t n) { memset(out,0,n); }
static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static void le16(uint8_t *p, uint32_t n) { p[0]=n&255; p[1]=(n>>8)&255; }
static uint32_t exsize(const uint8_t *p) {
  if(p[5]&32) return (((uint32_t)(p[6]&1)<<19)|((uint32_t)p[7]<<11)|((uint32_t)p[8]<<3)|(p[9]>>5))+1;
  return (((uint32_t)(p[6]&31)<<11)|((uint32_t)p[7]<<3)|(p[8]>>5))+1;
}
static int fail(struct state *s, uint32_t code, uint32_t *outn) {
  s->error=code; *outn=0; return -(int)code;
}
/* Codes: 1=state/input, 2=framing, 3=rate/period, 4=carrier capacity,
 * 5=output capacity, 6=missing extension. Caller owns all buffer lifetimes.
 */
int dtshd_process(struct state *s, const uint8_t *input, uint32_t n,
                  uint8_t *out, uint32_t cap, uint32_t *outn, int eof) {
  uint32_t used=0, written=0, frame_count=0;
  static const uint32_t rates[16]={0,8000,16000,32000,0,0,11025,22050,44100,0,0,12000,24000,48000,96000,192000};
  *outn=0;
  if(s->error) return -(int)s->error;
  if(s->used>RAW_CAP || n>RAW_CAP-s->used) return fail(s,1,outn);
  if(n) memcpy(s->raw+s->used,input,n);
  s->used+=n;
  while(s->used-used>=4) {
    uint8_t *p=s->raw+used;
    uint32_t available=s->used-used, core, end, hd=0, complete=0;
    uint32_t rate,samples,product,period,subtype,burst,payload,pd,i;
    if(be32(p)!=0x7ffe8001u) return fail(s,2,outn);
    if(available<10) break;
    core=(((uint32_t)(p[5]&3)<<12)|((uint32_t)p[6]<<4)|(p[7]>>4))+1;
    if(core<10) return fail(s,2,outn);
    if(core>available) break;
    end=core;
    for(;;) {
      uint32_t sync,ext;
      if(available-end<4) { complete=eof && available==end && hd; break; }
      sync=be32(p+end);
      if(sync==0x7ffe8001u) { complete=1; break; }
      if(sync!=0x64582025u) return fail(s,2,outn);
      if(available-end<10) break;
      ext=exsize(p+end);
      if(ext<10 || ext>RAW_CAP-end) return fail(s,2,outn);
      if(ext>available-end) break;
      end+=ext; hd=1;
    }
    if(!complete) break;
    if(!hd) return fail(s,6,outn);
    rate=rates[(p[8]>>2)&15];
    samples=((((p[4]&1)<<6)|(p[5]>>2))+1)*32;
    /* Max 4096 core samples, so this multiplication fits uint32_t. */
    product=768000u*samples;
    if(!rate || product%rate) return fail(s,3,outn);
    period=product/rate;
    for(subtype=0;subtype<6 && (512u<<subtype)!=period;subtype++) {}
    if(subtype==6) return fail(s,3,outn);
    burst=period*4; payload=12+end; pd=((payload+23)&~15u)-8;
    if(end>65535 || pd>65535 || pd>burst-8) return fail(s,4,outn);
    if(burst>cap-written) return fail(s,5,outn);
    memset(out+written,0,burst);
    le16(out+written,0xf872); le16(out+written+2,0x4e1f);
    le16(out+written+4,0x11|(subtype<<8)); le16(out+written+6,pd);
    /* Write wrapper and access unit directly in 16-bit swapped order. */
    out[written+9]=1; out[written+16]=0xfe; out[written+17]=0xfe;
    out[written+18]=end&255; out[written+19]=(end>>8)&255;
    for(i=0;i<end;i++) out[written+20+(i^1u)]=p[i];
    written+=burst; used+=end; frame_count++;
  }
  if(used) { s->used-=used; memmove(s->raw,s->raw+used,s->used); }
  s->frames+=frame_count; s->bytes+=written; *outn=written; return 0;
}
uint32_t dtshd_pending(struct state *s) { return s->used; }
/* Harness entry point: loops in native code, keeping RPC overhead out of timing. */
int dtshd_repack_buffer(struct state *s, const uint8_t *input, uint32_t n,
                       uint32_t chunk, uint8_t *out, uint32_t cap, uint32_t *outn) {
  uint32_t pos=0,total=0,took=0; int rc;
  *outn=0;
  if(!chunk) return -1;
  dtshd_reset(s);
  while(pos<n) {
    uint32_t size=n-pos; if(size>chunk) size=chunk;
    rc=dtshd_process(s,input+pos,size,out+total,cap-total,&took,0);
    if(rc) return rc;
    pos+=size; total+=took;
  }
  rc=dtshd_process(s,input,0,out+total,cap-total,&took,1);
  if(rc) return rc;
  *outn=total+took; return 0;
}
