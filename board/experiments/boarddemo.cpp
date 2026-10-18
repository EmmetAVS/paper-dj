// boarddemo: live "which button is being pressed" for the printed 16-button board, QNX + Pi 5 + Camera Module 3.
//
//  camera (libcamapi NV12 callbacks, ~30 fps, newest frame only)
//   -> fast loop every frame: find the 16 button squares (OpenCV quad candidates, refreshed when all 16 visible),
//      per-button "hand over it" score vs an empty-board reference, pick the fingertip button
//   -> Laya worker (as fast as the CPU allows): crop centred on the fingertip button -> Laya ONNX -> p(pressed)
//      (+ optional linear readout on Laya's decision features, fitted on labeled frames from this scene)
//   -> pressed button = hand over it AND Laya says pressed. Released immediately when the hand leaves.
//
// Build: g++ -O2 -std=c++17 -I. -I/usr/include/onnxruntime -I/usr/include/opencv4 boarddemo.cpp \
//        -lcamapi -lonnxruntime -lopencv_objdetect -lopencv_imgproc -lopencv_imgcodecs -lopencv_core -lpthread -o boarddemo
#include "camera/camera_api.h"
#include <onnxruntime_cxx_api.h>
#include <opencv2/opencv.hpp>
#include <opencv2/objdetect/aruco_detector.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

static double now_s(){ return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static int64_t mono_us(){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000; }

// ---------------- camera: newest full NV12 frame ----------------
struct Frame { std::shared_ptr<std::vector<uint8_t>> buf; int w=0,h=0; int64_t ts=0; uint64_t seq=0; };
static std::mutex g_fm; static std::condition_variable g_fcv; static Frame g_latest; static uint64_t g_seq=0; static std::atomic<bool> g_stop{false};
static void frame_cb(camera_handle_t, camera_buffer_t* b, void*){
  if(b->frametype!=CAMERA_FRAMETYPE_NV12) return; const auto& d=b->framedesc.nv12; int w=d.width,h=d.height;
  auto buf=std::make_shared<std::vector<uint8_t>>((size_t)w*h*3/2);
  for(int r=0;r<h;r++) memcpy(buf->data()+(size_t)r*w, b->framebuf+(size_t)r*d.stride, w);
  for(int r=0;r<h/2;r++) memcpy(buf->data()+(size_t)w*h+(size_t)r*w, b->framebuf+d.uv_offset+(size_t)r*d.uv_stride, w);
  { std::lock_guard<std::mutex> lk(g_fm); g_latest.buf=buf; g_latest.w=w; g_latest.h=h; g_latest.ts=b->frametimestamp; g_latest.seq=++g_seq; }
  g_fcv.notify_all();
}
static void status_cb(camera_handle_t, camera_devstatus_t st, uint16_t extra, void*){ if(st!=20) fprintf(stderr,"[camera] status %d extra %u\n",(int)st,(unsigned)extra); }

// ---------------- board layout ----------------
struct Layout { bool ok=false; std::vector<cv::Point2f> quad[16]; cv::Rect2f box[16]; };
// Find the 16 bright button squares in the half-res gray image and number them 1..16 row-major.
static bool find_layout(const cv::Mat& gray, Layout& L){
  static cv::aruco::ArucoDetector det(cv::aruco::getPredefinedDictionary(cv::aruco::DICT_4X4_50), cv::aruco::DetectorParameters());
  std::vector<int> ids; std::vector<std::vector<cv::Point2f>> c, rej; det.detectMarkers(gray,c,ids,rej);
  struct Q{ std::vector<cv::Point2f> q; cv::Point2f m; double area; };
  std::vector<Q> qs; cv::Mat mask(gray.size(),CV_8U);
  cv::Mat ring(gray.size(),CV_8U);
  for(auto& q:rej){ double a=cv::contourArea(q); if(a<1500) continue; mask=0; std::vector<cv::Point> p(q.begin(),q.end()); cv::fillConvexPoly(mask,p,255);
    // corner markers are much darker than the paper right around them; buttons are about as bright (lighting-independent)
    cv::Point2f c=(q[0]+q[1]+q[2]+q[3])/4; std::vector<cv::Point> big; for(auto& v:q) big.push_back(c+(v-c)*1.3f);
    ring=0; cv::fillConvexPoly(ring,big,255); ring.setTo(0,mask);
    double inside=cv::mean(gray,mask)[0], around=cv::mean(gray,ring)[0];
    if(inside<0.6*around) continue;
    qs.push_back({q,(q[0]+q[1]+q[2]+q[3])/4,a}); }
  if(qs.size()<10) return false;
  // drop size outliers
  std::vector<double> ar; for(auto& q:qs) ar.push_back(q.area); std::nth_element(ar.begin(),ar.begin()+ar.size()/2,ar.end()); double med=ar[ar.size()/2];
  qs.erase(std::remove_if(qs.begin(),qs.end(),[&](const Q& q){ return q.area<med*0.6||q.area>med*1.6; }),qs.end());
  if(qs.size()<10) return false;
  // board rotation: median edge angle (mod 90 deg) of the button squares
  std::vector<double> angs; for(auto& q:qs) for(int k=0;k<4;k++){ cv::Point2f e=q.q[(k+1)%4]-q.q[k]; double t=std::atan2(e.y,e.x); t=std::fmod(t+4*CV_PI,CV_PI/2); if(t>CV_PI/4) t-=CV_PI/2; angs.push_back(t); }
  std::nth_element(angs.begin(),angs.begin()+angs.size()/2,angs.end()); double th=angs[angs.size()/2], ct=std::cos(-th), st=std::sin(-th);
  // rows / columns: split rotated coordinates at the 3 largest gaps
  auto groups=[&](std::vector<double> v)->std::vector<double>{ // returns 3 thresholds
    std::sort(v.begin(),v.end()); std::vector<std::pair<double,double>> g; for(size_t i=1;i<v.size();i++) g.push_back({v[i]-v[i-1],(v[i]+v[i-1])/2});
    std::sort(g.rbegin(),g.rend()); std::vector<double> t; for(int i=0;i<3 && i<(int)g.size();i++) t.push_back(g[i].second); std::sort(t.begin(),t.end()); return t; };
  std::vector<double> xs,ys; for(auto& q:qs){ xs.push_back(q.m.x*ct-q.m.y*st); ys.push_back(q.m.x*st+q.m.y*ct); }
  auto tx=groups(xs), ty=groups(ys); if(tx.size()<3||ty.size()<3) return false;
  double side=std::sqrt(med);
  for(int i=0;i<2;i++){ if(tx[i+1]-tx[i]<side*0.7||ty[i+1]-ty[i]<side*0.7) return false; } // gaps must be a button pitch apart
  auto bin=[](double v,const std::vector<double>& t){ int k=0; while(k<3 && v>t[k]) k++; return k; };
  std::vector<int> cell(qs.size()); std::vector<cv::Point2f> src,dst; int used[16]={0};
  for(size_t i=0;i<qs.size();i++){ int c=bin(xs[i],tx), r=bin(ys[i],ty); cell[i]=r*4+c; used[cell[i]]++; src.push_back(cv::Point2f(c,r)); dst.push_back(qs[i].m); }
  for(int k=0;k<16;k++) if(used[k]>1) return false;   // two squares in one cell: not a clean grid
  cv::Mat Hg=cv::findHomography(src,dst,cv::RANSAC,side*0.25); if(Hg.empty()) return false;
  // reprojection check
  std::vector<cv::Point2f> proj; cv::perspectiveTransform(src,proj,Hg); int good=0; for(size_t i=0;i<proj.size();i++) if(cv::norm(proj[i]-dst[i])<side*0.25) good++;
  if(good<10) return false;
  // button half-size in grid units, from detected squares
  std::vector<cv::Point2f> unit={cv::Point2f(0,0),cv::Point2f(1,0)}, up; cv::perspectiveTransform(unit,up,Hg); double gpx=cv::norm(up[1]-up[0]); float hs=float(side/gpx/2);
  Layout N; N.ok=true;
  for(int k=0;k<16;k++){ int found=-1; for(size_t i=0;i<qs.size();i++) if(cell[i]==k) found=(int)i;
    if(found>=0) N.quad[k]=qs[found].q;
    else { int c=k%4,r=k/4; std::vector<cv::Point2f> g={{c-hs,r-hs},{c+hs,r-hs},{c+hs,r+hs},{c-hs,r+hs}}; cv::perspectiveTransform(g,N.quad[k],Hg); }
    N.box[k]=cv::boundingRect(N.quad[k]); }
  L=N; return true;
}
// Inner patch of a button, perspective-normalised to SxS (inset 12% to skip the printed outline).
static cv::Mat cell_patch(const cv::Mat& gray,const std::vector<cv::Point2f>& q,int S=24){
  cv::Point2f m=(q[0]+q[1]+q[2]+q[3])/4; std::vector<cv::Point2f> src(4); for(int i=0;i<4;i++) src[i]=m+(q[i]-m)*0.88f;
  // order corners by angle so the warp is consistent
  std::sort(src.begin(),src.end(),[&](const cv::Point2f&a,const cv::Point2f&b){ return std::atan2(a.y-m.y,a.x-m.x)<std::atan2(b.y-m.y,b.x-m.x); });
  std::vector<cv::Point2f> dst={{0,0},{(float)S,0},{(float)S,(float)S},{0,(float)S}};
  // atan2 order starts at top-left (-135deg), then top-right, bottom-right, bottom-left
  cv::Mat H=cv::getPerspectiveTransform(src,dst), out; cv::warpPerspective(gray,out,H,cv::Size(S,S),cv::INTER_AREA); out.convertTo(out,CV_32F); return out;
}

// ---------------- Laya crop preprocessing (full-res NV12 -> size x size, [-1,1] CHW) ----------------
static void crop_to_tensor(const Frame& f, cv::Rect roi, int size, float* out){
  roi&=cv::Rect(0,0,f.w,f.h); roi.x&=~1; roi.y&=~1; roi.width&=~1; roi.height&=~1;
  const uint8_t* Yp=f.buf->data(); const uint8_t* UVp=Yp+(size_t)f.w*f.h; const size_t n=(size_t)size*size;
  for(int oy=0;oy<size;oy++){ int ya=roi.y+oy*roi.height/size, yb=std::max(ya+1,roi.y+(oy+1)*roi.height/size);
    for(int ox=0;ox<size;ox++){ int xa=roi.x+ox*roi.width/size, xb=std::max(xa+1,roi.x+(ox+1)*roi.width/size);
      unsigned sy=0; for(int y=ya;y<yb;y++){ const uint8_t* r=Yp+(size_t)y*f.w; for(int x=xa;x<xb;x++) sy+=r[x]; }
      unsigned su=0,sv=0,cnt=0; for(int y=ya/2;y<std::max(ya/2+1,yb/2);y++){ const uint8_t* r=UVp+(size_t)y*f.w; for(int x=xa/2;x<std::max(xa/2+1,xb/2);x++){ su+=r[2*x]; sv+=r[2*x+1]; cnt++; } }
      float Y=sy/float((yb-ya)*(xb-xa)), U=su/float(cnt)-128.f, V=sv/float(cnt)-128.f, c=std::max(Y-16.f,0.f)*1.164383f;
      auto nz=[](float v){ v=std::min(std::max(v,0.f),255.f); return v/127.5f-1.f; }; size_t i=(size_t)oy*size+ox;
      out[i]=nz(c+1.596027f*V); out[n+i]=nz(c-.391762f*U-.812968f*V); out[2*n+i]=nz(c+2.017232f*U); } }
}

static void on_sig(int){ g_stop=true; g_fcv.notify_all(); }

int main(int argc,char** argv){
  std::string model="/data/home/qnxuser/touchdemo/laya_board_int8.onnx", probe_path, collect_dir, collect_label; int threads=4; double occ_th=22, on_th=0.5, off_th=0.4, margin=1.7; bool pretty=false; double run_seconds=0; bool use_laya=false; double dwell_ms=250; double blob_th=30; int min_blob=40; double release_s=0.3; double pinch_min=6, pinch_max=400; int pinch_release_frames=8; bool skin_mode=true; double skin_th=8, skin_ymin=40, skin_grow=1.3, skin_occ=5; int skin_min=25; float fader_rows=3.0f; std::string ft_model; double ft_th=0.5;
  for(int i=1;i<argc;i++){ std::string a=argv[i]; auto next=[&]{ if(i+1>=argc){fprintf(stderr,"missing value for %s\n",a.c_str()); exit(2);} return std::string(argv[++i]); };
    if(a=="--model") model=next(); else if(a=="--threads") threads=atoi(next().c_str()); else if(a=="--occ") occ_th=atof(next().c_str());
    else if(a=="--on") on_th=atof(next().c_str()); else if(a=="--off") off_th=atof(next().c_str()); else if(a=="--margin") margin=atof(next().c_str());
    else if(a=="--probe") probe_path=next(); else if(a=="--collect"){ collect_label=next(); collect_dir=next(); }
    else if(a=="--pretty") pretty=true; else if(a=="--laya") use_laya=true; else if(a=="--dwell") dwell_ms=atof(next().c_str()); else if(a=="--blob") blob_th=atof(next().c_str()); else if(a=="--release") release_s=atof(next().c_str()); else if(a=="--pinch-min") pinch_min=atof(next().c_str()); else if(a=="--pinch-max") pinch_max=atof(next().c_str()); else if(a=="--fader-rows") fader_rows=atof(next().c_str()); else if(a=="--pinch-release") pinch_release_frames=atoi(next().c_str());
    else if(a=="--mask"){ std::string m=next(); skin_mode=(m=="skin"); } else if(a=="--skin-th") skin_th=atof(next().c_str()); else if(a=="--skin-ymin") skin_ymin=atof(next().c_str()); else if(a=="--fingertip") ft_model=next(); else if(a=="--ft-th") ft_th=atof(next().c_str()); else if(a=="--min-blob") min_blob=atoi(next().c_str()); else if(a=="--seconds") run_seconds=atof(next().c_str());
    else { fprintf(stderr,"usage: boarddemo [--model m.onnx] [--threads 4] [--occ 22] [--on 0.5] [--off 0.4] [--margin 1.7] [--probe probe.txt] [--collect touch|hover DIR] [--pretty] [--seconds N] [--laya] [--dwell 250]\n"); return 2; } }
  signal(SIGINT,on_sig); signal(SIGTERM,on_sig);

  // Optional linear readout on Laya features: text file "d\nbias\nw0 w1 ... w{d-1}\n" (+ optional mean/std lines)
  std::vector<float> pw, pmu, psd; float pb=0;
  if(!probe_path.empty()){ std::ifstream in(probe_path); int d; in>>d>>pb; pw.resize(d); for(auto& v:pw) in>>v; pmu.assign(d,0); psd.assign(d,1); for(auto& v:pmu) in>>v; for(auto& v:psd) in>>v; if(!in){ fprintf(stderr,"bad probe file\n"); return 2; } fprintf(stderr,"[probe] %d-dim readout loaded\n",d); }
  if(!collect_dir.empty()) mkdir(collect_dir.c_str(),0755);

  Ort::Env env(ORT_LOGGING_LEVEL_ERROR,"laya"); Ort::SessionOptions so; so.SetIntraOpNumThreads(threads); so.SetInterOpNumThreads(1);
  so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  if(!collect_dir.empty()) use_laya=true;
  std::unique_ptr<Ort::Session> sess; std::thread loader([&]{ if(!use_laya) return; double t=now_s(); sess.reset(new Ort::Session(env,model.c_str(),so)); fprintf(stderr,"[laya] loaded %s in %.1f s\n",model.c_str(),now_s()-t); });

  std::unique_ptr<Ort::Session> ft; if(!ft_model.empty()){ Ort::SessionOptions fo; fo.SetIntraOpNumThreads(2); fo.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    ft.reset(new Ort::Session(env,ft_model.c_str(),fo)); fprintf(stderr,"[fingertip] model %s loaded\n",ft_model.c_str()); }
  camera_handle_t h=CAMERA_HANDLE_INVALID; int err=camera_open(CAMERA_UNIT_3,CAMERA_MODE_PREAD|CAMERA_MODE_PWRITE|CAMERA_MODE_DREAD,&h);
  if(err){ fprintf(stderr,"camera_open failed: %d (close other camera apps)\n",err); return 1; }
  camera_set_vf_property(h,CAMERA_IMGPROP_CREATEWINDOW,0); camera_set_vf_property(h,CAMERA_IMGPROP_FRAMERATE,30.0);
  if((err=camera_start_viewfinder(h,frame_cb,status_cb,nullptr))){ fprintf(stderr,"camera_start_viewfinder failed: %d\n",err); return 1; }
  loader.join();
  std::vector<int64_t> shape={1,1,3,128,128}; if(use_laya) shape=sess->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
  const int size=(int)shape.back(); size_t count=1; for(auto d:shape) count*=d;
  const bool has_feat=use_laya && sess->GetOutputCount()>1;

  // shared state between fast loop and Laya worker
  std::mutex sm; std::condition_variable scv; Frame job_frame; int job_cell=-1; cv::Rect job_roi; uint64_t job_seq=0;
  struct LayaOut{ int cell=-1; float p=0, praw=0; uint64_t seq=0; int64_t ts=0; double ms=0; bool fresh=false; } lout;

  std::thread worker([&]{ if(!use_laya) return;
    std::vector<float> px(count); auto mem=Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,OrtMemTypeDefault);
    const char* ins[]={"pixels"}; const char* outs1[]={"probabilities"}; const char* outs2[]={"probabilities","features"}; uint64_t done_seq=0; int saved=0;
    while(!g_stop){
      Frame f; int cell; cv::Rect roi; { std::unique_lock<std::mutex> lk(sm); scv.wait(lk,[&]{ return g_stop || job_seq>done_seq; }); if(g_stop) break; f=job_frame; cell=job_cell; roi=job_roi; done_seq=job_seq; }
      crop_to_tensor(f,roi,size,px.data()); double t0=now_s();
      auto x=Ort::Value::CreateTensor<float>(mem,px.data(),px.size(),shape.data(),shape.size());
      auto y=sess->Run(Ort::RunOptions{nullptr},ins,&x,1,has_feat?outs2:outs1,has_feat?2:1); double ms=(now_s()-t0)*1000;
      float praw=y[0].GetTensorData<float>()[1], p=praw;
      if(has_feat){ const float* ft=y[1].GetTensorData<float>(); size_t d=y[1].GetTensorTypeAndShapeInfo().GetElementCount();
        if(!pw.empty() && pw.size()==d){ double z=pb; for(size_t k=0;k<d;k++) z+=pw[k]*(ft[k]-pmu[k])/psd[k]; p=1.f/(1.f+std::exp(-z)); }
        if(!collect_dir.empty()){ char nm[512]; snprintf(nm,sizeof nm,"%s/%s_%lld_c%02d",collect_dir.c_str(),collect_label.c_str(),(long long)f.ts,cell+1);
          std::ofstream(std::string(nm)+".feat",std::ios::binary).write((const char*)ft,d*sizeof(float));
          std::ofstream(std::string(nm)+".px",std::ios::binary).write((const char*)px.data(),px.size()*sizeof(float)); saved++;
          fprintf(stderr,"[collect] %s #%d cell %d p=%.3f\n",collect_label.c_str(),saved,cell+1,praw); } }
      std::lock_guard<std::mutex> lk(sm); lout.cell=cell; lout.p=p; lout.praw=praw; lout.seq=f.seq; lout.ts=f.ts; lout.ms=ms; lout.fresh=true;
    }
  });

  if(use_laya) fprintf(stderr,"[ready] Laya %dx%d%s, %d threads. \n",size,size,has_feat?" (+features)":"",threads);
  fprintf(stderr,"[ready] mode: %s. Keep hands off the board for 2 s while the empty reference is captured.\n",!ft_model.empty()?"fingertip model (no reference image)":use_laya?"hand tracking + Laya touch check":(skin_mode?"skin-colour fingertip tracking (no reference image) + dwell":"hand tracking + dwell (no Laya)"));
  Layout L; std::vector<cv::Mat> ref(16); cv::Mat ref_small; cv::Point2f tip_pt(-1,-1), dbg_entry(-1,-1); int ref_frames=0; uint64_t seen=0; double t_start=now_s(), last_layout_try=0, last_print=0;
  int pressed=-1; int prev_tip=-1; std::deque<double> laya_times; int frames=0; double fps_t=now_s(); double fps=0;
  // pinch-to-fader state: 4 vertical faders, one per board column
  // faders start at dj_engine defaults: vol 0.8, filter 0 (= fader 0.5)
  float fader[4]={0.8f,0.5f,0.5f,0.8f}; bool pinching=false; int pinch_on=0, pinch_off=0, pinch_col=-1; float pinch_v0=0, fader0=0; cv::Point2f pinch_pt(-1,-1); std::vector<cv::Point> pinch_hole;
  bool all16=false; double ghost_since=0; cv::Point2f ghost_pt(-1,-1);
  float occ[16]={0}; double many_since=0; double tip_since=0; int dwell_tip=-1;
  while(!g_stop){
    if(run_seconds>0 && now_s()-t_start>run_seconds) break;
    Frame f; { std::unique_lock<std::mutex> lk(g_fm); g_fcv.wait(lk,[&]{ return g_stop || g_latest.seq>seen; }); if(g_stop) break; f=g_latest; seen=f.seq; }
    frames++; if(now_s()-fps_t>=1){ fps=frames/(now_s()-fps_t); frames=0; fps_t=now_s(); }
    // half-res gray view of the Y plane (2x2 average)
    cv::Mat Y(f.h,f.w,CV_8U,f.buf->data()), gray; cv::resize(Y,gray,cv::Size(f.w/2,f.h/2),0,0,cv::INTER_AREA);
    // (re)locate the buttons: until found, then every 0.5 s while the board is fully visible and nothing is pressed
    // Re-locate the buttons ~3x/s whatever the press state (pressing slides the paper). Seeing all 16 means the board
    // is unobstructed, so if it moved, the empty-board reference is re-captured right away.
    if(!L.ok || now_s()-last_layout_try>0.3){ last_layout_try=now_s(); Layout N;
      if(find_layout(gray,N)){
        if(!L.ok) fprintf(stderr,"[board] found 16 buttons\n");
        else { std::vector<float> sh; for(int i=0;i<16;i++){ cv::Point2f a=(L.quad[i][0]+L.quad[i][1]+L.quad[i][2]+L.quad[i][3])/4, b=(N.quad[i][0]+N.quad[i][1]+N.quad[i][2]+N.quad[i][3])/4; sh.push_back(cv::norm(a-b)); }
          std::nth_element(sh.begin(),sh.begin()+8,sh.end());
          if(sh[8]>3.0f && ref_frames>=20 && !ref_small.empty()){
            std::vector<cv::Point2f> a,b; for(int i=0;i<16;i++){ a.push_back((L.quad[i][0]+L.quad[i][1]+L.quad[i][2]+L.quad[i][3])/4*0.25f); b.push_back((N.quad[i][0]+N.quad[i][1]+N.quad[i][2]+N.quad[i][3])/4*0.25f); }
            cv::Mat Hm=cv::findHomography(a,b); if(!Hm.empty()){ cv::Mat w; cv::warpPerspective(ref_small,w,Hm,ref_small.size(),cv::INTER_LINEAR,cv::BORDER_REPLICATE); ref_small=w; }
            fprintf(stderr,"[board] board moved %.1f px, reference shifted with it\n",sh[8]); } }
        L=N; all16=true; } else all16=false; }
    if(!L.ok){ if(now_s()-last_print>2){ fprintf(stderr,"[board] looking for the 16 buttons...\n"); last_print=now_s(); } continue; }
    if(ft){ // ---------------- learned per-button fingertip model ----------------
      static std::vector<float> in(16*3*64*64); static double ft_ms=0; double t0=now_s();
      cv::Mat Yf(f.h,f.w,CV_8U,f.buf->data()), UVf(f.h/2,f.w/2,CV_8UC2,f.buf->data()+(size_t)f.w*f.h), bgr, half;
      cv::cvtColorTwoPlane(Yf,UVf,bgr,cv::COLOR_YUV2BGR_NV12); cv::resize(bgr,half,cv::Size(f.w/2,f.h/2),0,0,cv::INTER_AREA);
      for(int i=0;i<16;i++){ // same geometry as train/board_crops.py: button corners -> [16,48] box inside a 64x64 crop
        cv::Point2f m=(L.quad[i][0]+L.quad[i][1]+L.quad[i][2]+L.quad[i][3])/4; std::vector<cv::Point2f> src(L.quad[i].begin(),L.quad[i].end());
        std::sort(src.begin(),src.end(),[&](const cv::Point2f&a,const cv::Point2f&b){ return std::atan2(a.y-m.y,a.x-m.x)<std::atan2(b.y-m.y,b.x-m.x); });
        std::vector<cv::Point2f> dst={{16,16},{48,16},{48,48},{16,48}}; cv::Mat Hm=cv::getPerspectiveTransform(src,dst), crop;
        cv::warpPerspective(half,crop,Hm,cv::Size(64,64),cv::INTER_AREA,cv::BORDER_REPLICATE);
        float* o=&in[(size_t)i*3*64*64]; for(int y=0;y<64;y++){ const uint8_t* r=crop.ptr<uint8_t>(y); for(int x=0;x<64;x++) for(int c=0;c<3;c++) o[(size_t)c*4096+y*64+x]=r[x*3+c]/255.f; } }
      std::array<int64_t,4> shp{16,3,64,64}; auto mem=Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,OrtMemTypeDefault);
      auto xt=Ort::Value::CreateTensor<float>(mem,in.data(),in.size(),shp.data(),4); const char* fi[]={"crops"}; const char* fo2[]={"logits"};
      auto yo=ft->Run(Ort::RunOptions{nullptr},fi,&xt,1,fo2,1); const float* lg=yo[0].GetTensorData<float>();
      float prob[16]; int tip=-1; float bp=0; for(int i=0;i<16;i++){ prob[i]=1.f/(1.f+std::exp(-lg[i])); occ[i]=prob[i]*100; if(prob[i]>bp){ bp=prob[i]; tip=i; } }
      if(bp<ft_th) tip=-1; ft_ms=ft_ms*0.9+(now_s()-t0)*100;
      if(tip!=dwell_tip){ dwell_tip=tip; tip_since=now_s(); }
      int newpressed=pressed; static double pressed_seen=0; if(pressed>=0 && tip==pressed) pressed_seen=now_s();
      if(tip>=0 && tip!=pressed && (now_s()-tip_since)*1000>=dwell_ms) newpressed=tip;
      else if(pressed>=0 && tip!=pressed && now_s()-pressed_seen<release_s) newpressed=pressed;
      else if(pressed>=0 && tip!=pressed) newpressed=-1;
      if(newpressed>=0 && newpressed!=pressed) pressed_seen=now_s();
      static double last_emit=0; static int last_tip=-2;
      if(newpressed!=pressed || tip!=last_tip || now_s()-last_emit>1.0){ last_emit=now_s(); last_tip=tip;
        printf("{\"t\":%.2f,\"pressed\":%d,\"tip\":%d,\"p\":%.2f,\"model_ms\":%.1f,\"latency_ms\":%.0f,\"cam_fps\":%.1f,\"hands\":[",now_s()-t_start,newpressed+1,tip+1,bp,ft_ms,(mono_us()-f.ts)/1000.0,fps);
        bool first=true; for(int i=0;i<16;i++) if(prob[i]>0.2f){ printf("%s%d",first?"":",",i+1); first=false; } printf("]}\n"); fflush(stdout); }
      if(newpressed!=pressed){ fprintf(stderr,newpressed>=0?">>> BUTTON %d PRESSED\n":"<<< released (button %d)\n",newpressed>=0?newpressed+1:pressed+1); pressed=newpressed; }
      // labeled recording still works in this mode
      { static double last_rec=0, last_chk=0; static std::string rec_label;
        if(now_s()-last_chk>0.1){ last_chk=now_s(); std::ifstream rin("rec.label"); rec_label.clear(); if(rin) std::getline(rin,rec_label); }
        if(!rec_label.empty() && now_s()-last_rec>=0.1){ last_rec=now_s(); std::string dir="recordings/"+rec_label.substr(0,rec_label.find('/')); mkdir("recordings",0755); mkdir(dir.c_str(),0755);
          char nm[64]; snprintf(nm,sizeof nm,"%lld_",(long long)(mono_us()/1000)); cv::imwrite(dir+"/"+nm+rec_label.substr(rec_label.find('/')+1)+".jpg",half,{cv::IMWRITE_JPEG_QUALITY,90}); } }
      continue; }
    // empty-board reference: average the first frames after the board is found
    std::vector<cv::Mat> cur(16); for(int i=0;i<16;i++) cur[i]=cell_patch(gray,L.quad[i]);
    cv::Mat small; cv::resize(gray,small,cv::Size(gray.cols/4,gray.rows/4),0,0,cv::INTER_AREA); small.convertTo(small,CV_32F);
    if(ref_frames<20){ if(ref_frames==0) ref_small=small.clone(); else ref_small=ref_small*0.8+small*0.2;
      for(int i=0;i<16;i++){ if(ref[i].empty()) ref[i]=cur[i].clone(); else ref[i]=ref[i]*0.8+cur[i]*0.2; } if(++ref_frames==20) fprintf(stderr,"[board] empty reference captured. Go!\n"); continue; }
    // occupancy: mean abs difference after compensating global exposure changes (median per-cell gain)
    std::vector<float> gain(16); for(int i=0;i<16;i++) gain[i]=(float)(cv::mean(cur[i])[0]/std::max(1.0,cv::mean(ref[i])[0]));
    std::vector<float> g2=gain; std::nth_element(g2.begin(),g2.begin()+8,g2.end()); float G=std::max(0.3f,g2[8]);
    int nocc=0; for(int i=0;i<16;i++){ cv::Mat d; cv::absdiff(cur[i]/G,ref[i],d); occ[i]=(float)cv::mean(d)[0]; if(occ[i]>occ_th) nocc++; }
    if(!skin_mode && nocc>=12){ if(many_since==0) many_since=now_s(); else if(now_s()-many_since>3){ fprintf(stderr,"\n[board] reference looks stale (%d buttons covered for 3 s), recapturing; keep hands off\n",nocc); ref_frames=0; for(auto& r:ref) r.release(); many_since=0; continue; } } else many_since=0;
    // fingertip: largest changed blob vs the empty board; tip = blob pixel farthest from where it enters the frame
    int tip=-1; tip_pt=cv::Point2f(-1,-1);
    cv::Mat diff,mask;
    std::vector<cv::Point2f> skin_hull;   // quarter-res search region (paper around the button grid)
    if(skin_mode){
      // Skin colour instead of "different from the empty board": skin is tinted (Cr > Cb), paper and shadows are neutral.
      // Needs no reference image, so lighting changes, shadows and a sliding board don't create phantom hands.
      cv::Mat UVf(f.h/2,f.w/2,CV_8UC2,f.buf->data()+(size_t)f.w*f.h), uvq; cv::resize(UVf,uvq,small.size(),0,0,cv::INTER_AREA);
      std::vector<cv::Mat> ch; cv::split(uvq,ch); cv::Mat U,V; ch[0].convertTo(U,CV_32F); ch[1].convertTo(V,CV_32F); cv::Mat d=V-U;
      std::vector<cv::Point2f> pts; for(int i=0;i<16;i++) for(auto& q:L.quad[i]) pts.push_back(q*0.25f);
      cv::convexHull(pts,skin_hull); cv::Point2f hc(0,0); for(auto& q:skin_hull) hc+=q; hc*=1.f/skin_hull.size();
      std::vector<cv::Point> rp; for(auto& q:skin_hull) rp.push_back(cv::Point(cvRound(hc.x+(q.x-hc.x)*skin_grow),cvRound(hc.y+(q.y-hc.y)*skin_grow)));
      cv::Mat region=cv::Mat::zeros(small.size(),CV_8U); cv::fillConvexPoly(region,rp,255);
      std::vector<float> vals; vals.reserve(60000); for(int y=0;y<d.rows;y+=2){ const float* r=d.ptr<float>(y); const uint8_t* g=region.ptr<uint8_t>(y); for(int x=0;x<d.cols;x+=2) if(g[x]) vals.push_back(r[x]); }
      float med=0; if(!vals.empty()){ std::nth_element(vals.begin(),vals.begin()+vals.size()/2,vals.end()); med=vals[vals.size()/2]; }   // paper's own tint
      mask=cv::Mat::zeros(small.size(),CV_8U);
      for(int y=0;y<d.rows;y++){ const float* r=d.ptr<float>(y); const float* yy=small.ptr<float>(y); const uint8_t* g=region.ptr<uint8_t>(y); uint8_t* m=mask.ptr<uint8_t>(y);
        for(int x=0;x<d.cols;x++) m[x]=(g[x] && r[x]-med>skin_th && yy[x]>skin_ymin)?255:0; }
      cv::morphologyEx(mask,mask,cv::MORPH_OPEN,cv::getStructuringElement(cv::MORPH_ELLIPSE,cv::Size(5,5)));
      // per-button "hand over it" = share of skin pixels inside the button
      for(int i=0;i<16;i++){ std::vector<cv::Point> q; for(auto& v:L.quad[i]) q.push_back(cv::Point(cvRound(v.x*0.25f),cvRound(v.y*0.25f)));
        cv::Rect r=cv::boundingRect(q)&cv::Rect(0,0,mask.cols,mask.rows); if(r.area()<=0){ occ[i]=0; continue; }
        cv::Mat cm=cv::Mat::zeros(r.size(),CV_8U); for(auto& v:q) v-=r.tl(); cv::fillConvexPoly(cm,q,255);
        int in=cv::countNonZero(cm), sk=cv::countNonZero(mask(r)&cm); occ[i]=in? 100.f*sk/in : 0; }
      nocc=0; for(int i=0;i<16;i++) if(occ[i]>skin_occ) nocc++;
    } else { cv::absdiff(small/G,ref_small,diff); cv::threshold(diff,mask,blob_th,255,cv::THRESH_BINARY); mask.convertTo(mask,CV_8U); }
    cv::morphologyEx(mask,mask,cv::MORPH_OPEN,cv::getStructuringElement(cv::MORPH_ELLIPSE,cv::Size(5,5)));
    cv::Mat lab,stats,cents; int nl=cv::connectedComponentsWithStats(mask,lab,stats,cents,8,CV_32S); int best=-1,barea=0;
    if(skin_mode && nl>1){
      std::vector<cv::Point> gp; for(auto& q:skin_hull) gp.push_back(cv::Point(cvRound(q.x),cvRound(q.y)));
      cv::Mat gm=cv::Mat::zeros(lab.size(),CV_8U); if(gp.size()>=3) cv::fillConvexPoly(gm,gp,255);
      std::vector<int> over(nl,0); for(int y=0;y<lab.rows;y++){ const int* r=lab.ptr<int>(y); const uint8_t* g=gm.ptr<uint8_t>(y); for(int x=0;x<lab.cols;x++) if(r[x] && g[x]) over[r[x]]++; }
      int bo=0; for(int k=1;k<nl;k++) if(over[k]>bo){ bo=over[k]; best=k; barea=stats.at<int>(k,cv::CC_STAT_AREA); }
    } else
    for(int k=1;k<nl;k++){ int a=stats.at<int>(k,cv::CC_STAT_AREA); if(a>barea){ barea=a; best=k; } }
    // hand = the largest patch plus sizeable patches that touch it after growing it by ~half a button (split fingers),
    // but not stray specks elsewhere on the board
    int total=0; std::vector<char> keepc(nl,0);
    if(best>0){ cv::Mat main=(lab==best), near; cv::dilate(main,near,cv::getStructuringElement(cv::MORPH_ELLIPSE,cv::Size(25,25)));
      for(int y=0;y<lab.rows;y++){ const int* r=lab.ptr<int>(y); const uint8_t* n=near.ptr<uint8_t>(y); for(int x=0;x<lab.cols;x++) if(r[x]>0 && n[x]) keepc[r[x]]=1; }
      for(int k=1;k<nl;k++){ if(keepc[k] && (k==best || stats.at<int>(k,cv::CC_STAT_AREA)>=6)) total+=stats.at<int>(k,cv::CC_STAT_AREA); else keepc[k]=0; } }
    if(best>0){ for(int y=0;y<lab.rows;y++){ int* r=lab.ptr<int>(y); for(int x=0;x<lab.cols;x++) r[x]= (r[x]>0 && keepc[r[x]])? best : 0; } barea=total; }
    if(skin_mode && best>0 && barea>=skin_min){
      std::vector<cv::Point2f> P; for(int y=0;y<lab.rows;y++){ const int* r=lab.ptr<int>(y); for(int x=0;x<lab.cols;x++) if(r[x]==best) P.push_back(cv::Point2f(x,y)); }
      cv::Mat data((int)P.size(),2,CV_32F,P.data()); cv::PCA pca(data,cv::Mat(),cv::PCA::DATA_AS_ROW);
      cv::Point2f m(pca.mean.at<float>(0),pca.mean.at<float>(1)), ax(pca.eigenvectors.at<float>(0,0),pca.eigenvectors.at<float>(0,1));
      cv::Point2f gc(0,0); for(auto& q:skin_hull) gc+=q; gc*=1.f/skin_hull.size();
      if((gc-m).dot(ax)<0) ax=-ax;                          // fingers reach in from outside: tip end points toward the grid centre
      float bp=-1e9f; cv::Point2f far; for(auto& q:P){ float t=(q-m).dot(ax); if(t>bp){ bp=t; far=q; } }
      dbg_entry=(m-ax*40.f)*4.f; tip_pt=(far-ax*2.0f)*4.f; // pad of the finger, half-res coords
      for(int i=0;i<16;i++) if(cv::pointPolygonTest(L.quad[i],tip_pt,false)>=0){ tip=i; break; }   // gaps between buttons = no press
    }
    else if(!skin_mode && best>0 && barea>=min_blob){
      // Board-anchored: the grid region = hull of the 16 buttons (quarter-res), grown by 15%. Blob pixels outside it are
      // the arm; the fingertip is the blob pixel inside the grid farthest from the arm.
      std::vector<cv::Point2f> pts; for(int i=0;i<16;i++) for(auto& q:L.quad[i]) pts.push_back(q*0.25f);
      std::vector<cv::Point2f> hull; cv::convexHull(pts,hull); cv::Point2f hc(0,0); for(auto& q:hull) hc+=q; hc*=1.f/hull.size();
      for(auto& q:hull) q=hc+(q-hc)*1.3f;
      cv::Mat grid=cv::Mat::zeros(lab.size(),CV_8U); { std::vector<cv::Point> hp; for(auto& q:hull) hp.push_back(cv::Point(cvRound(q.x),cvRound(q.y))); cv::fillConvexPoly(grid,hp,255); }
      const int W=lab.cols,Hh=lab.rows; double ex=0,ey=0; int en=0, inside=0;
      for(int y=0;y<Hh;y++){ const int* r=lab.ptr<int>(y); const uint8_t* g=grid.ptr<uint8_t>(y); for(int x=0;x<W;x++) if(r[x]==best){ if(g[x]) inside++; else { ex+=x; ey+=y; en++; } } }
      // prefer where the hand crosses the camera frame edge (that is unambiguously the arm)
      double bx=0,by=0; int bn=0;
      for(int x=0;x<W;x++){ if(lab.at<int>(0,x)==best){bx+=x;bn++;} if(lab.at<int>(Hh-1,x)==best){bx+=x;by+=Hh-1;bn++;} }
      for(int y=0;y<Hh;y++){ if(lab.at<int>(y,0)==best){by+=y;bn++;} if(lab.at<int>(y,W-1)==best){bx+=W-1;by+=y;bn++;} }
      cv::Point2f entry;
      if(bn>=3) entry=cv::Point2f(bx/bn,by/bn);
      else if(en>=5) entry=cv::Point2f(ex/en,ey/en);
      else { // whole hand inside the grid: the arm is toward the nearest side of the grid
        cv::Point2f c(cents.at<double>(best,0),cents.at<double>(best,1)); cv::Rect gb=cv::boundingRect(hull);
        float dl=c.x-gb.x, dr=gb.x+gb.width-c.x, dt=c.y-gb.y, db=gb.y+gb.height-c.y; float m=std::min({dl,dr,dt,db});
        entry = m==dl? cv::Point2f(gb.x-50,c.y) : m==dr? cv::Point2f(gb.x+gb.width+50,c.y) : m==dt? cv::Point2f(c.x,gb.y-50) : cv::Point2f(c.x,gb.y+gb.height+50); }
      dbg_entry=entry*4.f; double bd=-1e18; cv::Point far(-1,-1);
      double gx=0,gy=0; int gn=0; for(int y=0;y<Hh;y++){ const int* r=lab.ptr<int>(y); const uint8_t* g=grid.ptr<uint8_t>(y); for(int x=0;x<W;x++) if(r[x]==best && g[x]){ gx+=x; gy+=y; gn++; } }
      cv::Point2f dirp(1,0); if(gn){ dirp=cv::Point2f(gx/gn,gy/gn)-entry; float n=std::sqrt(dirp.dot(dirp)); if(n>1e-3) dirp*=1.f/n; }
      for(int y=0;y<Hh;y++){ const int* r=lab.ptr<int>(y); const uint8_t* g=grid.ptr<uint8_t>(y); for(int x=0;x<W;x++) if(r[x]==best && g[x]){
        double along=(x-entry.x)*dirp.x+(y-entry.y)*dirp.y, side=std::fabs(-(x-entry.x)*dirp.y+(y-entry.y)*dirp.x);
        double score=along-0.25*side;   // reach along the pointing direction, small penalty for sideways offset
        if(score>bd){ bd=score; far=cv::Point(x,y);} } }
      if(inside<min_blob/4 || far.x<0) goto no_tip;
      // step back a little from the very tip toward the entry (the pad of the finger, not the nail edge)
      cv::Point2f dir=entry-cv::Point2f(far); float dl=std::sqrt(dir.dot(dir)); if(dl>0) dir*=1.f/dl;
      tip_pt=(cv::Point2f(far)+dir*2.0f)*4.f; // back to half-res coords
      double bestd=1e18; for(int i=0;i<16;i++){ double in=cv::pointPolygonTest(L.quad[i],tip_pt,true); if(in>=0){ tip=i; break; }
        cv::Point2f c=(L.quad[i][0]+L.quad[i][1]+L.quad[i][2]+L.quad[i][3])/4; double d=cv::norm(c-tip_pt); float half=std::max(L.box[i].width,L.box[i].height)*0.6f; if(d<half && d<bestd){ bestd=d; tip=i; } }
    }
    no_tip:;
    // ---- pinch: thumb + index touching close a loop, seen from above as a hole of paper inside the hand ----
    { bool hole=false; cv::Point2f hc(-1,-1); pinch_hole.clear();
      if(best>0 && barea>=min_blob){
        cv::Mat hand=(lab==best); cv::morphologyEx(hand,hand,cv::MORPH_CLOSE,cv::getStructuringElement(cv::MORPH_ELLIPSE,cv::Size(3,3)));
        std::vector<std::vector<cv::Point>> cs; std::vector<cv::Vec4i> hier; cv::findContours(hand,cs,hier,cv::RETR_CCOMP,cv::CHAIN_APPROX_SIMPLE);
        double bestA=0;
        for(size_t k=0;k<cs.size();k++){ if(hier[k][3]<0) continue;   // child contour = hole
          double A=cv::contourArea(cs[k]); if(A<pinch_min || A>pinch_max) continue;
          std::vector<cv::Point> hull; cv::convexHull(cs[k],hull); double solid=A/std::max(1.0,cv::contourArea(hull)); if(solid<0.6) continue;
          if(A>bestA){ bestA=A; cv::Moments mo=cv::moments(cs[k]); hc=cv::Point2f(mo.m10/mo.m00,mo.m01/mo.m00)*4.f; pinch_hole=cs[k]; hole=true; } } }
      if(hole){ pinch_on++; pinch_off=0; } else { pinch_off++; pinch_on=0; }
      // board coordinates of the hole: homography from the 16 button centres to grid (col,row) units
      auto to_grid=[&](cv::Point2f q)->cv::Point2f{ std::vector<cv::Point2f> src,dst; for(int i=0;i<16;i++){ src.push_back((L.quad[i][0]+L.quad[i][1]+L.quad[i][2]+L.quad[i][3])/4); dst.push_back(cv::Point2f(i%4,i/4)); }
        cv::Mat Hg=cv::findHomography(src,dst); std::vector<cv::Point2f> in{q},out; cv::perspectiveTransform(in,out,Hg); return out[0]; };
      if(!pinching && pinch_on>=3 && hc.x>=0){ cv::Point2f g=to_grid(hc);
        // a pinch only grabs a fader when it starts over one of the top-row buttons 1-4 (that button = that fader);
        // grid units: button centres at integers, so row 0 spans about -0.5..0.5 (a little extra above for hovering)
        if(g.x>-0.5f && g.x<3.5f && g.y>-0.9f && g.y<0.6f){ pinching=true; pinch_col=std::min(3,std::max(0,(int)std::lround(g.x))); pinch_v0=g.y; fader0=fader[pinch_col]; pinch_pt=hc;
          fprintf(stderr,">>> PINCH grab fader %d\n",pinch_col+1); } }
      else if(pinching && hole && hc.x>=0){ cv::Point2f g=to_grid(hc); pinch_pt=hc;
        fader[pinch_col]=std::min(1.f,std::max(0.f, fader0 + (pinch_v0-g.y)/fader_rows));   // drag up = louder; fader_rows grid rows = full range
      }
      else if(pinching && pinch_off>=pinch_release_frames){ pinching=false; fprintf(stderr,"<<< PINCH released fader %d at %.2f\n",pinch_col+1,fader[pinch_col]); pinch_col=-1; } }
    const bool blob=(best>0 && barea>=min_blob);
    if(!blob && nocc==0){ ref_small=ref_small*0.97+(small/G)*0.03; for(int i=0;i<16;i++) ref[i]=ref[i]*0.98+(cur[i]/G)*0.02; } // board clear: track lighting drift
    // 3. ghost hand: a blob that sits still while all 16 button outlines are fully visible (a real hand blocks some)
    if(!skin_mode && blob && all16 && tip_pt.x>=0 && (ghost_pt.x<0 || cv::norm(tip_pt-ghost_pt)<6)){ if(ghost_since==0){ ghost_since=now_s(); ghost_pt=tip_pt; }
      else if(now_s()-ghost_since>1.0){ fprintf(stderr,"\n[board] stale hand image cleared, re-capturing reference\n"); ghost_since=0; ghost_pt=cv::Point2f(-1,-1); ref_frames=0; for(auto& r:ref) r.release(); if(pressed>=0){ pressed=-1; } continue; } }
    else { ghost_since=0; ghost_pt=cv::Point2f(-1,-1); }
    // debug snapshot on request: touch ~/touchdemo/debug.req
    // labeled recording: the page server writes the current label to rec.label (empty file = stop).
    // Frames are saved as raw half-res color JPEGs at ~10 fps: <rec_dir>/<ms>_<label>.jpg
    { static double last_rec=0, last_chk=0; static std::string rec_label;
      if(now_s()-last_chk>0.1){ last_chk=now_s(); std::ifstream in("rec.label"); rec_label.clear(); if(in) std::getline(in,rec_label); }
      if(!rec_label.empty() && now_s()-last_rec>=0.1){ last_rec=now_s();
        std::string dir="recordings/"+rec_label.substr(0,rec_label.find('/')); mkdir("recordings",0755); mkdir(dir.c_str(),0755);
        cv::Mat Yf(f.h,f.w,CV_8U,f.buf->data()), UVf(f.h/2,f.w/2,CV_8UC2,f.buf->data()+(size_t)f.w*f.h), bgr, half;
        cv::cvtColorTwoPlane(Yf,UVf,bgr,cv::COLOR_YUV2BGR_NV12); cv::resize(bgr,half,cv::Size(f.w/2,f.h/2),0,0,cv::INTER_AREA);
        char nm[64]; snprintf(nm,sizeof nm,"%lld_",(long long)(mono_us()/1000));
        cv::imwrite(dir+"/"+nm+rec_label.substr(rec_label.find('/')+1)+".jpg",half,{cv::IMWRITE_JPEG_QUALITY,90}); } }
    static double last_auto_dbg=0; static int dbg_n=0; bool auto_dbg=(best>0 && barea>=min_blob && now_s()-last_auto_dbg>1.0 && access("debug.auto",F_OK)==0);
    if(auto_dbg || access("debug.req",F_OK)==0){ if(!auto_dbg) unlink("debug.req"); else last_auto_dbg=now_s(); cv::Mat dbg; cv::cvtColor(gray,dbg,cv::COLOR_GRAY2BGR);
      cv::Mat m2; cv::resize(mask,m2,gray.size(),0,0,cv::INTER_NEAREST); dbg.setTo(cv::Scalar(255,120,0),m2);
      if(best>0){ cv::Mat kept=(lab==best), k2; cv::resize(kept,k2,gray.size(),0,0,cv::INTER_NEAREST); dbg.setTo(cv::Scalar(0,140,255),k2); }
      if(dbg_entry.x>-1000) cv::circle(dbg,dbg_entry,9,cv::Scalar(255,255,0),3);
      if(!pinch_hole.empty()){ std::vector<cv::Point> hh; for(auto& q:pinch_hole) hh.push_back(q*4); cv::polylines(dbg,std::vector<std::vector<cv::Point>>{hh},true,cv::Scalar(255,0,255),3); }
      if(pinching) cv::putText(dbg,"PINCH fader "+std::to_string(pinch_col+1),cv::Point(10,50),0,0.8,cv::Scalar(255,0,255),2);
      for(int i=0;i<16;i++){ std::vector<cv::Point> q(L.quad[i].begin(),L.quad[i].end()); cv::polylines(dbg,q,true,i==tip?cv::Scalar(0,0,255):cv::Scalar(0,200,0),i==tip?3:1); cv::putText(dbg,std::to_string(i+1),q[0]+cv::Point(4,14),0,0.45,cv::Scalar(255,0,0),1); }
      if(tip_pt.x>=0) cv::circle(dbg,tip_pt,7,cv::Scalar(255,0,255),2); std::string fn= auto_dbg? "debug_"+std::to_string(dbg_n++%10)+".png" : std::string("debug.png");
      cv::putText(dbg,"tip "+std::to_string(tip+1)+" t="+std::to_string(int((now_s()-t_start)*10)/10.0).substr(0,6),cv::Point(10,24),0,0.7,cv::Scalar(0,0,255),2);
      cv::imwrite(fn,dbg); cv::imwrite("raw_"+fn,mask); if(best>0){ cv::Mat kept=(lab==best); cv::imwrite("kept_"+fn,kept); } fprintf(stderr,"[debug] wrote %s tip=%d blob=%d\n",fn.c_str(),tip+1,barea); }
    if(tip!=dwell_tip){ dwell_tip=tip; tip_since=now_s(); }
    if(use_laya && tip>=0){ // hand Laya the newest frame + fingertip crop (full-res coords, square, with margin for finger + shadow context)
      cv::Rect2f b=L.box[tip]; float s=std::max(b.width,b.height)*2*margin; cv::Point2f m(2*(b.x+b.width/2),2*(b.y+b.height/2));
      std::lock_guard<std::mutex> lk(sm); job_frame=f; job_cell=tip; job_roi=cv::Rect(int(m.x-s/2),int(m.y-s/2),int(s),int(s)); job_seq++; scv.notify_one(); }
    // decision
    LayaOut lo; { std::lock_guard<std::mutex> lk(sm); lo=lout; lout.fresh=false; }
    int newpressed=pressed;
    if(pinching){ tip=-1; }
    if(!use_laya){
      static double pressed_seen=0; if(pressed>=0 && tip==pressed) pressed_seen=now_s();
      if(tip>=0 && tip!=pressed && (now_s()-tip_since)*1000>=dwell_ms) newpressed=tip;                 // settled on a (new) button
      else if(pressed>=0 && tip!=pressed && now_s()-pressed_seen<release_s) newpressed=pressed;           // brief tip loss: hold
      else if(pressed>=0 && tip!=pressed) newpressed=-1;
      if(newpressed>=0 && newpressed!=pressed) pressed_seen=now_s(); }
    else if(tip<0) newpressed=-1;                                   // no hand over any button: released at camera rate
    else if(lo.fresh && lo.cell==tip){ if(pressed==tip) newpressed=(lo.p>=off_th)?tip:-1; else newpressed=(lo.p>=on_th)?tip:-1; }
    else if(pressed>=0 && pressed!=tip) newpressed=-1;        // fingertip moved to another button: wait for Laya on it
    if(lo.fresh){ laya_times.push_back(now_s()); while(laya_times.size()>10) laya_times.pop_front();
      double rate=laya_times.size()>1?(laya_times.size()-1)/(laya_times.back()-laya_times.front()):0;
      printf("{\"t\":%.2f,\"pressed\":%d,\"tip\":%d,\"laya_button\":%d,\"p\":%.3f,\"p_laya\":%.3f,\"laya_ms\":%.0f,\"laya_hz\":%.1f,\"latency_ms\":%.0f,\"cam_fps\":%.1f,\"hands\":[",
        now_s()-t_start,newpressed+1,tip+1,lo.cell+1,lo.p,lo.praw,lo.ms,rate,(mono_us()-lo.ts)/1000.0,fps);
      bool first=true; for(int i=0;i<16;i++) if(occ[i]>occ_th){ printf("%s%d",first?"":",",i+1); first=false; } printf("]}\n"); fflush(stdout); }
    unsigned hmask=0; for(int i=0;i<16;i++) if(occ[i]>(skin_mode?skin_occ:occ_th)) hmask|=1u<<i;
    static unsigned prev_hmask=~0u; static double last_emit=0;
    static float prev_f[4]={-1,-1,-1,-1}; static bool prev_pinch=false; bool fchg=false; for(int k=0;k<4;k++) if(std::fabs(fader[k]-prev_f[k])>0.004f) fchg=true;
    bool emit=(newpressed!=pressed) || (pinching!=prev_pinch) || (fchg && now_s()-last_emit>0.04) || (hmask!=prev_hmask && now_s()-last_emit>0.1) || now_s()-last_emit>1.0;
    if(emit){ for(int k=0;k<4;k++) prev_f[k]=fader[k]; prev_pinch=pinching; }
    if(emit && !use_laya){ prev_hmask=hmask; last_emit=now_s(); printf("{\"t\":%.2f,\"pressed\":%d,\"tip\":%d,\"pinch\":%s,\"fader\":%d,\"faders\":[%.3f,%.3f,%.3f,%.3f],\"latency_ms\":%.0f,\"cam_fps\":%.1f,\"hands\":[",
        now_s()-t_start,newpressed+1,tip+1,pinching?"true":"false",pinching?pinch_col+1:0,fader[0],fader[1],fader[2],fader[3],(mono_us()-f.ts)/1000.0,fps);
      bool first=true; for(int i=0;i<16;i++) if(occ[i]>(skin_mode?skin_occ:occ_th)){ printf("%s%d",first?"":",",i+1); first=false; } printf("]}\n"); fflush(stdout); }
    if(newpressed!=pressed){ fprintf(stderr,newpressed>=0?"\n>>> BUTTON %d PRESSED\n":"\n<<< released (button %d)\n",newpressed>=0?newpressed+1:pressed+1); pressed=newpressed; }
    if(pretty && now_s()-last_print>0.25){ last_print=now_s(); fprintf(stderr,"\r");
      for(int i=0;i<16;i++) fprintf(stderr,"%s%2d%s ", i==pressed?"[":(occ[i]>occ_th?"(":" "), i+1, i==pressed?"]":(occ[i]>occ_th?")":" ")); fprintf(stderr," cam %.0ffps  ",fps); }
    prev_tip=tip;
  }
  g_stop=true; g_fcv.notify_all(); scv.notify_all(); worker.join(); camera_stop_viewfinder(h); camera_close(h); return 0;
}
