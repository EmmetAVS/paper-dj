// snap: grab one full-resolution NV12 frame from the camera and write a half-res PPM.
#include "camera/camera_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static volatile int done=0; static int skip=15; static const char* out="/data/home/qnxuser/touchdemo/full.ppm";
static unsigned char cl(float v){ return v<0?0:v>255?255:(unsigned char)v; }
static void cb(camera_handle_t h,camera_buffer_t*b,void*a){ (void)h;(void)a;
  if(done||b->frametype!=CAMERA_FRAMETYPE_NV12) return; if(skip-->0) return; // let auto-exposure settle
  const camera_frame_nv12_t*d=&b->framedesc.nv12; int W=d->width/2,H=d->height/2; FILE*f=fopen(out,"wb"); fprintf(f,"P6\n%d %d\n255\n",W,H);
  unsigned char*row=malloc(W*3); const unsigned char*uvp=b->framebuf+d->uv_offset;
  for(int yy=0;yy<H;yy++){ int y=yy*2; for(int xx=0;xx<W;xx++){ int x=xx*2; float Y=b->framebuf[(size_t)y*d->stride+x];
    const unsigned char*uv=uvp+(size_t)(y/2)*d->uv_stride+(x/2)*2; float U=uv[0]-128.f,V=uv[1]-128.f, c=(Y-16>0?Y-16:0)*1.164383f;
    row[xx*3]=cl(c+1.596027f*V); row[xx*3+1]=cl(c-.391762f*U-.812968f*V); row[xx*3+2]=cl(c+2.017232f*U);} fwrite(row,1,W*3,f);} fclose(f); free(row); done=1; }
int main(int argc,char**argv){ if(argc>1) out=argv[1]; camera_handle_t h; int e=camera_open(CAMERA_UNIT_3,CAMERA_MODE_PREAD|CAMERA_MODE_PWRITE|CAMERA_MODE_DREAD,&h); if(e){fprintf(stderr,"open %d\n",e);return 1;}
  camera_set_vf_property(h,CAMERA_IMGPROP_CREATEWINDOW,0); e=camera_start_viewfinder(h,cb,NULL,NULL); if(e){fprintf(stderr,"start %d\n",e);return 1;}
  for(int i=0;i<200&&!done;i++) usleep(50000); camera_stop_viewfinder(h); camera_close(h); printf(done?"ok\n":"timeout\n"); return !done; }
