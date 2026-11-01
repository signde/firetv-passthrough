/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exact PS7702 reversible equivalents of the reviewed open-driver changes.
 * Never replace the proprietary Dolby module. Only alter calls in our session.
 */
#include <linux/utsname.h>
#include <linux/kallsyms.h>
struct code_guard { const char *name; unsigned long addr; size_t size; u64 hash; };
#include "live_guards.h"
#define VD1_ADDR 0xffffffc011b744d8UL
#define VD2_ADDR 0xffffffc011b74b78UL
#define FEL_ENABLE_ADDR 0xffffffc011bbc9ccUL
#define EL_FLAGS_ADDR 0xffffffc011ba9c70UL
#define AMDV_MASK_ADDR 0xffffffc01197ef68UL
#define DV_FLAGS_ADDR 0xffffffc01197efa0UL
static bool fix_session, saved_enable_fel;
static u32 (*read_dv_reg)(u32);
static int (*write_dv_bits)(u32,u32,u32,u32);
static int (*write_dv_bits_now)(u32,u32,u32,u32);
static u32 saved_vd2_bypass;
static bool vd2_changed;
static unsigned long vd2_route_writes;
static u32 saved_el_mux;
static bool mux_allowed, mux_changed;
static bool route_el = true;
module_param(route_el,bool,0444);
MODULE_PARM_DESC(route_el,"Experimental T7 VD2 to core1a EL input routing");
/* A cold boot leaves these registers uninitialized at session open. Capture
 * the restoration state only when the stock T7 route is actually established. */
static bool prepare_el_route(void)
{
 u32 mux;
 if(mux_allowed)return true;
 if(((read_dv_reg(0x1a70)>>4)&7)!=0)return false;
 mux=(read_dv_reg(0x1a71)>>4)&3;
 if(mux!=3)return false;
 saved_el_mux=mux;
 saved_vd2_bypass=(read_dv_reg(0x1a84)>>4)&1;
 mux_allowed=true;
 pr_info("dvfel: stock T7 route ready, saved EL mux=%u VD2 bypass=%u\n",
         saved_el_mux,saved_vd2_bypass);
 return true;
}
static unsigned long fixed_receiver, fixed_peek, fixed_canvas, fixed_register;
static bool residual_bypass;
module_param(residual_bypass,bool,0444);
MODULE_PARM_DESC(residual_bypass,"Explicitly authorized temporary bypass of stock FEL residual rejection");
static struct module *stock_dovi_owner;
static struct kprobe residual_probe;
static bool residual_probe_active;
static unsigned long residual_checks_bypassed;
static bool residual_unpaired;
module_param(residual_unpaired,bool,0644);
MODULE_PARM_DESC(residual_unpaired,"Allow the same residual check bypass during BL fallback within the FEL test session");
static unsigned long residual_unpaired_hits;
static int residual_pre(struct kprobe *p, struct pt_regs *r)
{
 /* Emulate falling through this CBZ only. Do NOT rewrite RPU metadata,
  * coefficients, the file, or any other instruction/validation result. */
 if(READ_ONCE(fix_session) && READ_ONCE(g.active) &&
    (READ_ONCE(*(u8 *)EL_FLAGS_ADDR) || residual_unpaired) && !(u32)r->regs[8]){
  if (!READ_ONCE(*(u8 *)EL_FLAGS_ADDR)) residual_unpaired_hits++;
  r->regs[8]=1;
  residual_checks_bypassed++;
 }
 return 0;
}
static int residual_init(void)
{
 static const u32 expected[]={0x340000b7,0x9b1a52c8,0xf9402508,0x39527508,
                              0x34008cc8,0x9b1a52c8,0xf9401d09,0xd37df2ca};
 static const u32 expected_error[]={0x12800038,0x17ffffce};
 u32 actual[ARRAY_SIZE(expected)],error[ARRAY_SIZE(expected_error)];
 unsigned long addr=kallsyms_lookup_name("dovi:commit_reg");
 struct module *(*owner_of)(unsigned long)=(void *)kallsyms_lookup_name("__module_text_address");
 unsigned long irq_flags;
 struct module *owner;int ret;
 if(!residual_bypass)return 0;
 if(!addr || !owner_of)return -ENODEV;
 /* Keep this non-sleeping module lookup/reference acquisition unschedulable. */
 local_irq_save(irq_flags);
 owner=owner_of(addr);
 if(!owner || strcmp(owner->name,"dovi") || !try_module_get(owner))owner=NULL;
 local_irq_restore(irq_flags);
 if(!owner)return -ENODEV;
 stock_dovi_owner=owner;
 if(probe_kernel_read(actual,(void *)(addr+0xa78),sizeof(actual)) ||
    probe_kernel_read(error,(void *)(addr+0x1c20),sizeof(error)) ||
    memcmp(actual,expected,sizeof(actual)) || memcmp(error,expected_error,sizeof(error))){
  pr_err("dvfel: stock Dolby residual-check code guard failed\n");
  return -EINVAL;
 }
 residual_probe.addr=(kprobe_opcode_t *)(addr+0xa88);
 residual_probe.pre_handler=residual_pre;
 ret=register_kprobe(&residual_probe);
 if(ret)return ret;
 residual_probe_active=true;
 pr_info("dvfel: verified stock Dolby residual-check probe, inactive outside test session\n");
 return 0;
}
static bool fel_composer_mode;
module_param(fel_composer_mode,bool,0444);
MODULE_PARM_DESC(fel_composer_mode,"Session-only core1a composer mode matching working CE FEL reference");
static bool composer_mode_changed;
static unsigned long composer_mode_writes;
static bool composer = true;
module_param(composer, bool, 0444);
MODULE_PARM_DESC(composer,"Enable hardware composition during test session (default true)");
static int branch_pre(struct kprobe *p, struct pt_regs *r)
{
 if(READ_ONCE(fix_session) || READ_ONCE(g.closing)){ r->regs[8]=0; fixed_receiver++; }
 return 0;
}
static int peek_pre(struct kprobe *p, struct pt_regs *r)
{
 if(READ_ONCE(fix_session)){r->regs[8]=0;fixed_peek++;}
 return 0;
}
static int canvas_pre(struct kprobe *p, struct pt_regs *r)
{
 unsigned long lr=r->regs[30];
 if(READ_ONCE(fix_session) && r->regs[0]==VD1_ADDR &&
    (lr==0xffffffc010a96ac8UL || lr==0xffffffc010a96c48UL)){
  r->regs[0]=VD2_ADDR;fixed_canvas++;
 }
 return 0;
}
static bool coherent_bl_fallback;
module_param(coherent_bl_fallback,bool,0644);
MODULE_PARM_DESC(coherent_bl_fallback,"Synchronize composer and EL geometry for BL-only fallback; default off");
static unsigned long fallback_mode_writes;
static int last_route_enabled = -1;
static int reg_pre(struct kprobe *p, struct pt_regs *r)
{
 unsigned long lr=r->regs[30];u32 reg=r->regs[0], val=r->regs[1], off=reg&0xffffff;
 bool half, enabled;
 if(!READ_ONCE(fix_session) || !composer)return 0;
 if(!((lr>0xffffffc010c40fc4UL && lr<0xffffffc010c41cb8UL) ||
      (lr>0xffffffc010c41cb8UL && lr<0xffffffc010c423a0UL)))return 0;
 if((reg&0xff000000)!=0x01000000 && (reg&0xff000000)!=0x02000000 &&
    (reg&0xff000000)!=0x20000000)return 0;
 half=READ_ONCE(*(u8 *)(EL_FLAGS_ADDR+1));
 enabled=READ_ONCE(*(u8 *)EL_FLAGS_ADDR) &&
         (READ_ONCE(*(u32 *)AMDV_MASK_ADDR)&1) &&
         !(READ_ONCE(*(u32 *)DV_FLAGS_ADDR)&0x100);
 /* An absent EL must not keep the preceding half-resolution EL geometry. */
 if (coherent_bl_fallback && !enabled) half=false;
 if(off==1){val=(val&~8U)|(half?8:0);}
 else if(off==0xf3){
  val=(val&~0x36U)|(half?0x34:0)|((enabled&&(val&1))?2:0);
  if (coherent_bl_fallback && (reg&0xff000000)==0x01000000) {
   /* Stock caches mode1 even though our FEL path writes mode0; explicitly
    * write the current mode in the same VSYNC/RDMA batch as swap control. */
   write_dv_bits(0x01000032,enabled?0:1,0,1);
   composer_mode_changed=true;fallback_mode_writes++;
  }
  if((reg&0xff000000)==0x01000000 && route_el && (mux_allowed || (enabled && prepare_el_route()))){
   if (last_route_enabled != enabled) {
    pr_info("dvfel: route enabled=%d paired=%d queue=%d bl=%px\n",
      enabled,g.bl_found,g.nq,g.bl_vf);
    last_route_enabled=enabled;
   }
   /* Follow the driver's VSYNC/RDMA ordering. VD2 already targets core1a;
    * select VD2 (1), instead of null (3), for the core1a EL input only. */
   write_dv_bits(0x1a71,enabled?1:saved_el_mux,4,2);
   mux_changed=true;
   /* T7 also leaves VD2 bypassing DV. Route native EL through DV while
    * this session supplies frames; change only bit4, never the full word. */
   write_dv_bits(0x1a84,enabled?0:saved_vd2_bypass,4,1);
   vd2_changed=true;vd2_route_writes++;
  }
 }
 else if(off==0x32 && (reg&0xff000000)==0x01000000 &&
         fel_composer_mode && enabled && val==1){
  /* Stock comp_cfg_2_comp_reg hardcodes composer_mode=1. Working CE
   * on this device has identical comp coefficients but mode=0.
   * Intercept only this guarded session's open-driver register write. */
  val=0;composer_mode_changed=true;composer_mode_writes++;
 }
 else return 0;
 if(val!=r->regs[1]){r->regs[1]=val;fixed_register++;}
 return 0;
}
static int vd2_bits_pre(struct kprobe *p,struct pt_regs *r)
{
 /* Later core1b setup can otherwise overwrite core1a's EL routing. */
 if(READ_ONCE(fix_session) && READ_ONCE(g.active) && composer && route_el &&
    r->regs[0]==0x1a84 && r->regs[2]==4 && r->regs[3]==1 &&
    READ_ONCE(*(u8 *)EL_FLAGS_ADDR) &&
    (READ_ONCE(*(u32 *)AMDV_MASK_ADDR)&1) &&
    !(READ_ONCE(*(u32 *)DV_FLAGS_ADDR)&0x100) && prepare_el_route()){
  r->regs[1]=0;vd2_changed=true;vd2_route_writes++;
 }
 return 0;
}
static unsigned int meta_seen;
static unsigned int meta_unpaired;
static int meta_observe(struct kprobe *p, struct pt_regs *r)
{
 u32 el=0, size=0; u8 dual=0; void *aux=NULL; unsigned long flags; bool paired;
 if (!READ_ONCE(fix_session)) return 0;
 probe_kernel_read(&el,(void *)(r->regs[29]-0x24),4);
 probe_kernel_read(&size,(void *)(r->regs[29]-0x28),4);
 probe_kernel_read(&aux,(void *)(r->regs[29]-0x30),8);
 probe_kernel_read(&dual,(void *)(r->regs[20]+0x5ec),1);
 spin_lock_irqsave(&g.lock,flags);
 paired=g.active && g.bl_found && g.bl_vf==(struct vframe_s *)r->regs[20];
 spin_unlock_irqrestore(&g.lock,flags);
 /* The Android provider supplies AUX, bypassing the stock dual_layer fallback.
  * Only advertise EL when our own session has paired the current frame. */
 if (paired && dual && aux && size)
  WRITE_ONCE(*(u32 *)(r->regs[29]-0x24),1);
 if (!paired && meta_unpaired++ < 40)
  pr_info("dvfel: unpaired metadata toggle=%lu reqel=%u dual=%u size=%u vf=%px cached=%px found=%d queue=%d\n",
   r->regs[26]&255,el,dual,size,(void *)r->regs[20],g.bl_vf,g.bl_found,g.nq);
 if(meta_seen++ < 8)
  pr_info("dvfel: metadata gate toggle=%lu reqel=%u dual=%u size=%u paired=%d\n",
   r->regs[26]&255,el,dual,size,paired);
 return 0;
}
static int el_enable_pre(struct kprobe *p, struct pt_regs *r)
{
 unsigned long flags; bool paired;
 if(!READ_ONCE(fix_session))return 0;
 spin_lock_irqsave(&g.lock,flags);
 paired=g.active && g.bl_found && g.bl_vf==(struct vframe_s *)r->regs[20];
 spin_unlock_irqrestore(&g.lock,flags);
 /* Stock enable_mel also gates FEL. Override its loaded value for our BL only. */
 if(paired)r->regs[8]=1;
 return 0;
}
static struct kprobe fix_probes[]={
 {.symbol_name="amdv_parse_metadata_v2_stb",.offset=0xf20,.pre_handler=el_enable_pre},
 {.symbol_name="amdv_parse_metadata_v2_stb",.offset=0x184,.pre_handler=meta_observe},
 {.symbol_name="dvel_receiver_event_fun",.offset=0x70,.pre_handler=branch_pre},
 {.symbol_name="dvel_receiver_event_fun",.offset=0x180,.pre_handler=branch_pre},
 {.symbol_name="dv_toggle_frame",.offset=0x68,.pre_handler=peek_pre},
 {.symbol_name="set_layer_display_canvas",.pre_handler=canvas_pre},
 {.symbol_name="VSYNC_WR_DV_REG",.pre_handler=reg_pre},
 {.symbol_name="VSYNC_WR_DV_REG_BITS",.pre_handler=vd2_bits_pre},
};
static struct kretprobe bl_detach_probe;
static bool bl_detach_probe_active;
static int bl_detach_entry(struct kretprobe_instance *ri,struct pt_regs *r)
{
 const char *name=NULL;char text[16]={0};
 *(bool *)ri->data=false;
 if((READ_ONCE(fix_session)||READ_ONCE(g.closing)) && r->regs[2] &&
    !probe_kernel_read(&name,(void *)r->regs[2],sizeof(name)) && name &&
    !probe_kernel_read(text,name,sizeof(text)-1) && !strcmp(text,"video_render.0")){
  if(r->regs[0]==1)*(bool *)ri->data=true;
  else if(r->regs[0]==8)WRITE_ONCE(g.bl_detached,false); /* new codec/seek */
 }
 return 0;
}
static int bl_detach_ret(struct kretprobe_instance *ri,struct pt_regs *r)
{
 if(*(bool *)ri->data){
  WRITE_ONCE(g.bl_detached,true);
  if(READ_ONCE(g.closing))schedule_delayed_work(&g.orphan_work,msecs_to_jiffies(100));
 }
 return 0;
}
static int nfix;
/* Request the stock Dolby core3 black-picture path only during this test's
 * incomplete-frame gap. Do not alter global mute controls or any audio state.
 * A 750ms ceiling prevents missing EL from indefinitely hiding the display. */
static bool blank_seek_gaps;
module_param(blank_seek_gaps,bool,0644);
MODULE_PARM_DESC(blank_seek_gaps,"Video-only black interval during incomplete FEL seek/start frames; default off");
static void gap_arm_locked(bool restart)
{
 if (!g.gap_pending || restart) {
  g.gap_pending=true;
  g.gap_good_frames=0;
  g.gap_deadline=jiffies+msecs_to_jiffies(750);
  g.gap_ready_after=g.gap_deadline;
  g.gap_epochs++;
 }
}
static void fixes_exit(void)
{
 if(bl_detach_probe_active){unregister_kretprobe(&bl_detach_probe);bl_detach_probe_active=false;}
 if(residual_probe_active){unregister_kprobe(&residual_probe);residual_probe_active=false;}
 if(stock_dovi_owner){module_put(stock_dovi_owner);stock_dovi_owner=NULL;}
 while(nfix)unregister_kprobe(&fix_probes[--nfix]);
}
static int fixes_init(void)
{
 int i,ret;u8 buf[256],id[2];
 read_dv_reg=(void *)kallsyms_lookup_name("READ_VPP_DV_REG");
 write_dv_bits=(void *)kallsyms_lookup_name("VSYNC_WR_DV_REG_BITS");
 write_dv_bits_now=(void *)kallsyms_lookup_name("WRITE_VPP_DV_REG_BITS");
 if((unsigned long)read_dv_reg!=0xffffffc010c19d50UL ||
    (unsigned long)write_dv_bits!=0xffffffc010c19c54UL ||
    (unsigned long)write_dv_bits_now!=0xffffffc010c1a020UL)return -ENODEV;
 if(strcmp(init_uts_ns.name.release,"5.4.125-ab27"))return -ENODEV;
 for(i=0;i<ARRAY_SIZE(guards);i++){
  const struct code_guard *v=&guards[i];size_t off,j,n;u64 hash=14695981039346656037ULL;
  if(kallsyms_lookup_name(v->name)!=v->addr)return -EINVAL;
  for(off=0;off<v->size;off+=n){
   n=min(sizeof(buf),v->size-off);
   if(probe_kernel_read(buf,(void *)(v->addr+off),n))return -EFAULT;
   for(j=0;j<n;j++)hash=(hash^buf[j])*1099511628211ULL;
  }
  if(hash!=v->hash){pr_err("dvfel: code guard failed: %s\n",v->name);return -EINVAL;}
 }
 if(probe_kernel_read(&id[0],(void *)VD1_ADDR,1) ||
    probe_kernel_read(&id[1],(void *)VD2_ADDR,1) || id[0]!=0 || id[1]!=1)return -EINVAL;
 for(i=0;i<ARRAY_SIZE(fix_probes);i++){
  ret=register_kprobe(&fix_probes[i]);
  if(ret){pr_err("dvfel: fix probe %d failed %d\n",i,ret);fixes_exit();return ret;}
  nfix++;
 }
 bl_detach_probe.kp.symbol_name="common_receiver_event_fun";
 bl_detach_probe.entry_handler=bl_detach_entry;bl_detach_probe.handler=bl_detach_ret;
 bl_detach_probe.data_size=sizeof(bool);bl_detach_probe.maxactive=8;
 ret=register_kretprobe(&bl_detach_probe);if(ret){fixes_exit();return ret;}
 bl_detach_probe_active=true;
 ret=residual_init();if(ret){fixes_exit();return ret;}
 pr_info("dvfel: PS7702 code/layout guards passed, reversible fixes idle\n");
 return 0;
}
static void fixes_start(void)
{
 saved_enable_fel=READ_ONCE(*(u8 *)FEL_ENABLE_ADDR);
 saved_el_mux=(read_dv_reg(0x1a71)>>4)&3;
 saved_vd2_bypass=(read_dv_reg(0x1a84)>>4)&1;
 mux_allowed=((read_dv_reg(0x1a70)>>4)&7)==0 && saved_el_mux==3;
 mux_changed=false;composer_mode_changed=false;vd2_changed=false;
 pr_info("dvfel: T7 EL mux saved=%u route_allowed=%d\n",saved_el_mux,mux_allowed);
 WRITE_ONCE(fix_session,true);
 WRITE_ONCE(*(u8 *)FEL_ENABLE_ADDR,1);
}
static void fixes_stop(void)
{
 WRITE_ONCE(*(u8 *)FEL_ENABLE_ADDR,saved_enable_fel);
 WRITE_ONCE(fix_session,false);
 /* Flush the intended restoration through RDMA and also write it directly:
  * Android may stop the video/RDMA pipeline before our close callback. */
 if(vd2_changed){
  write_dv_bits(0x1a84,saved_vd2_bypass,4,1);
  write_dv_bits_now(0x1a84,saved_vd2_bypass,4,1);vd2_changed=false;
 }
 if(mux_changed){
  write_dv_bits(0x1a71,saved_el_mux,4,2);
  write_dv_bits_now(0x1a71,saved_el_mux,4,2);mux_changed=false;
 }
 if(composer_mode_changed){
  write_dv_bits(0x01000032,1,0,1);
  write_dv_bits_now(0x01000032,1,0,1);composer_mode_changed=false;
 }
 pr_info("dvfel: restore mux=%x vd2=%x composer=%x\n",read_dv_reg(0x1a71),
         read_dv_reg(0x1a84),read_dv_reg(0x01000032));
}
