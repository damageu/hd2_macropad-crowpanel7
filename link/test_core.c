#include <stdio.h>
#include <assert.h>
#include "link_core.h"
static char log_[2048]; static int ll=0;
static void cb(uint16_t sc,int down,void*c){ (void)c; ll+=sprintf(log_+ll,"%s%03X ",down?"+":"-",sc); }
int main(){
  struct {uint8_t u; uint16_t sc;} chk[]={{0x04,0x1E},{0x1D,0x2C},{0x1A,0x11},{0x16,0x1F},{0x07,0x20},{0x1E,0x02},{0x27,0x0B},{0x28,0x1C},{0x2C,0x39},{0x38,0x35},{0x39,0x3A},{0x3A,0x3B},{0x45,0x58},{0x46,0x137},{0x49,0x152},{0x4F,0x14D},{0x50,0x14B},{0x51,0x150},{0x52,0x148},{0x53,0x45},{0x58,0x11C},{0x59,0x4F},{0x61,0x49},{0x62,0x52},{0x63,0x53},{0x64,0x56},{0x65,0x15D},{0x68,0x64},{0x73,0x76}};
  for(unsigned i=0;i<sizeof chk/sizeof chk[0];i++){ if(hid_to_scancode(chk[i].u)!=chk[i].sc){printf("FAIL %02X -> %03X want %03X\n",chk[i].u,hid_to_scancode(chk[i].u),chk[i].sc);return 1;} }
  // simulate firmware sequence for ctrl + W S D (mask 0x01): fptr(mask,0,0); press/release each; fptr(0,0,0)
  uint8_t stream[512]; int n=0; uint8_t rep[7];
  n+=sprintf((char*)stream+n,"I (123) boot: garbage log line\r\n");
  uint8_t seq[]={0x1A,0x16,0x07};
  memset(rep,0,7); rep[0]=1; n+=link_build_frame(stream+n,1,rep,7);
  for(int i=0;i<3;i++){ memset(rep,0,7); rep[0]=1; rep[1]=seq[i]; n+=link_build_frame(stream+n,1,rep,7); memset(rep,0,7); rep[0]=1; n+=link_build_frame(stream+n,1,rep,7);
     if(i==1){ n+=sprintf((char*)stream+n,"\xA5\x5A\x01 junk"); } }
  memset(rep,0,7); n+=link_build_frame(stream+n,1,rep,7);
  uint8_t pong[4]={'H','D','2',1}; n+=link_build_frame(stream+n,2,pong,4);
  link_parser_t p={0}; link_state_t s={0}; int frames=0, pongs=0;
  for(int i=0;i<n;i++){ if(link_parse_byte(&p,stream[i])){ frames++; if(p.type==1&&p.len==7) link_apply_report(&s,p.payload,cb,0); if(p.type==2) pongs++; } }
  link_release_all(&s, cb, 0);
  printf("frames=%d pongs=%d\n%s\n",frames,pongs,log_);
  const char *want = "+01D +011 -011 +01F -01F +020 -020 -01D ";
  if (frames != 9 || pongs != 1 || strcmp(log_, want) != 0) {
    printf("FAIL want frames=9 pongs=1\n%s\n", want);
    return 1;
  }
  return 0;
}
