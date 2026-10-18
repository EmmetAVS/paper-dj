#include "camera/camera_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
static int frames=0; static int64_t first_ts=0,last_ts=0; static int dumped=0;
static double now(){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static void cb(camera_handle_t h,camera_buffer_t*b,void*arg){
  (void)h;(void)arg; frames++;
  if(!dumped){ dumped=1;
    const unsigned char*p=(const unsigned char*)b; printf("raw camera_buffer_t bytes:\n");
    for(int i=0;i<160;i++) printf("%02x%s",p[i],(i%16==15)?"\n":" ");
    printf("interp: frametype=%d framesize=%llu framebuf=%p metasize=%llu meta=%p ts=%lld orient=%d\n",(int)b->frametype,(unsigned long long)b->framesize,(void*)b->framebuf,(unsigned long long)b->framemetasize,b->framemeta,(long long)b->frametimestamp,b->frameorientation);
    printf("nv12: h=%u w=%u stride=%u uv_offset=%lld uv_stride=%lld\n",b->framedesc.nv12.height,b->framedesc.nv12.width,b->framedesc.nv12.stride,(long long)b->framedesc.nv12.uv_offset,(long long)b->framedesc.nv12.uv_stride);
    const uint32_t*d=(const uint32_t*)b->framedesc.raw; printf("framedesc dwords:"); for(int i=0;i<12;i++) printf(" %u",d[i]); printf("\n");
    if(b->framebuf){ unsigned long sum=0; for(int i=0;i<4096;i++) sum+=b->framebuf[i]; printf("first 4096 Y bytes mean=%.1f\n",sum/4096.0);} 
    first_ts=b->frametimestamp; }
  last_ts=b->frametimestamp;
}
int main(int argc,char**argv){
  int unit=argc>1?atoi(argv[1]):3; camera_handle_t h=CAMERA_HANDLE_INVALID;
  int err=camera_open((camera_unit_t)unit,CAMERA_MODE_PREAD|CAMERA_MODE_PWRITE|CAMERA_MODE_DREAD,&h);
  printf("camera_open unit %d err=%d handle=%d\n",unit,err,h); if(err) return 1;
  for(int i=0;i<=24;i++){ union{int32_t i32;double d;uint8_t raw[16];} v; memset(&v,0,sizeof v); v.i32=-7777;
    err=camera_get_vf_property(h,i,&v); printf("get prop %2d err=%d i32=%d d=%g\n",i,err,v.i32,v.d); }
  if(argc>2){ double fps=atof(argv[2]); err=camera_set_vf_property(h,CAMERA_IMGPROP_FRAMERATE,fps); printf("set framerate %g err=%d\n",fps,err);} 
  if(argc>4){ err=camera_set_vf_property(h,CAMERA_IMGPROP_WIDTH,atoi(argv[3])); printf("set width err=%d\n",err); err=camera_set_vf_property(h,CAMERA_IMGPROP_HEIGHT,atoi(argv[4])); printf("set height err=%d\n",err);} 
  if(getenv("NOWIN")){ err=camera_set_vf_property(h,CAMERA_IMGPROP_CREATEWINDOW,0); printf("set createwindow=0 err=%d\n",err);} 
  double t0=now(); err=camera_start_viewfinder(h,cb,NULL,NULL); printf("start_viewfinder err=%d (%.0f ms)\n",err,(now()-t0)*1000); if(err) return 1;
  sleep(4); double el=now()-t0; int n=frames;
  printf("frames=%d in %.2fs -> %.1f fps; ts span %.3f s (%s)\n",n,el,n/el,(last_ts-first_ts)/1e6,"assuming us");
  camera_stop_viewfinder(h); camera_close(h); return 0;
}
