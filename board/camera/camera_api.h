/* Minimal libcamapi declarations for QNX SDP 8.0 on Raspberry Pi 5.
 * The SDP header is not shipped in this image; this subset was reconstructed
 * from the public QNX/BlackBerry camera_api.h layout and verified against the
 * installed camera_example* binaries (handle passed in w0 as int32, property
 * ids FORMAT=0 WIDTH=1 HEIGHT=2 FRAMERATE=3(double) WIN_GROUPID=11 WIN_ID=12,
 * terminator -1, camera_open mode 7). Buffer layout verified empirically. */
#ifndef CAMERA_CAMERA_API_H
#define CAMERA_CAMERA_API_H
#include <stdint.h>
#include <errno.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef int camera_error_t;
#define CAMERA_EOK 0
typedef int32_t camera_handle_t;
#define CAMERA_HANDLE_INVALID (-1)
typedef enum { CAMERA_UNIT_NONE=0, CAMERA_UNIT_1, CAMERA_UNIT_2, CAMERA_UNIT_3, CAMERA_UNIT_4 } camera_unit_t;
enum { CAMERA_MODE_PREAD=1<<0, CAMERA_MODE_PWRITE=1<<1, CAMERA_MODE_DREAD=1<<2, CAMERA_MODE_DWRITE=1<<3, CAMERA_MODE_ROLL=1<<4,
       CAMERA_MODE_RO=(CAMERA_MODE_PREAD|CAMERA_MODE_DREAD), CAMERA_MODE_RW=(CAMERA_MODE_PREAD|CAMERA_MODE_PWRITE|CAMERA_MODE_DREAD|CAMERA_MODE_DWRITE) };
typedef enum { CAMERA_FRAMETYPE_UNSPECIFIED=0, CAMERA_FRAMETYPE_NV12=1, CAMERA_FRAMETYPE_RGB8888=2, CAMERA_FRAMETYPE_RGB888=3 } camera_frametype_t;
typedef enum { CAMERA_IMGPROP_END=-1, CAMERA_IMGPROP_FORMAT=0, CAMERA_IMGPROP_WIDTH=1, CAMERA_IMGPROP_HEIGHT=2, CAMERA_IMGPROP_FRAMERATE=3,
  CAMERA_IMGPROP_BITRATE=4, CAMERA_IMGPROP_KEYFRAMEINTERVAL=5, CAMERA_IMGPROP_ROTATION=6, CAMERA_IMGPROP_STABILIZATION=7, CAMERA_IMGPROP_ZOOMFACTOR=8,
  CAMERA_IMGPROP_HWOVERLAY=9, CAMERA_IMGPROP_JPEGQFACTOR=10, CAMERA_IMGPROP_WIN_GROUPID=11, CAMERA_IMGPROP_WIN_ID=12, CAMERA_IMGPROP_BURSTMODE=13,
  CAMERA_IMGPROP_BURSTDIVISOR=14, CAMERA_IMGPROP_SLICESIZE=15, CAMERA_IMGPROP_VARIABLEFRAMERATE=16, CAMERA_IMGPROP_MINFRAMERATE=17,
  CAMERA_IMGPROP_ISEMBEDDED=18, CAMERA_IMGPROP_CREATEWINDOW=19 } camera_imgprop_t;
typedef int camera_devstatus_t;
typedef struct { uint32_t height, width, stride; int64_t uv_offset; int64_t uv_stride; } camera_frame_nv12_t;
typedef struct { uint32_t height, width, stride; } camera_frame_rgb8888_t;
typedef union { camera_frame_nv12_t nv12; camera_frame_rgb8888_t rgb8888; uint8_t raw[128]; } camera_framedesc_t;
typedef struct {
  camera_frametype_t frametype; uint64_t framesize; uint8_t *framebuf; uint64_t framemetasize; void *framemeta;
  int64_t frametimestamp; int32_t frameorientation; uint32_t reserved[4]; camera_framedesc_t framedesc;
} camera_buffer_t;
camera_error_t camera_open(camera_unit_t unit, uint32_t mode, camera_handle_t *handle);
camera_error_t camera_close(camera_handle_t handle);
camera_error_t camera_start_viewfinder(camera_handle_t h, void(*cb)(camera_handle_t,camera_buffer_t*,void*), void(*st)(camera_handle_t,camera_devstatus_t,uint16_t,void*), void *arg);
camera_error_t camera_stop_viewfinder(camera_handle_t h);
camera_error_t camera_private_get_vf_property(camera_handle_t h, ...);
camera_error_t camera_private_set_vf_property(camera_handle_t h, ...);
#define camera_get_vf_property(h, ...) camera_private_get_vf_property((h), __VA_ARGS__, CAMERA_IMGPROP_END)
#define camera_set_vf_property(h, ...) camera_private_set_vf_property((h), __VA_ARGS__, CAMERA_IMGPROP_END)
#ifdef __cplusplus
}
#endif
#endif
