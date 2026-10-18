// deck: paper DJ controller for QNX / Raspberry Pi 5 / Camera Module 3.
// Finds the printed controls (16-button grid, vertical sliders, round dials) by shape, tracks the fingertip by skin
// colour (no reference image), and drives paper-dj's dj_engine over UDP:
//   button N   -> "pad N"                       (fingertip rests 250 ms on the button)
//   sliders    -> VOL A, FILTER A, FILTER B, VOL B (left to right): "a.vol v", "a.filter x", "b.filter x", "b.vol v"
//   dials      -> left = deck A, right = deck B: "a.scratch 1", "a.platter <deg>" ..., "a.scratch 0"
// One JSON line per state change on stdout (the board page shows it).
// Build: g++ -O2 -std=c++17 -I. -I/usr/include/opencv4 deck.cpp -lcamapi -lsocket -lopencv_objdetect -lopencv_imgproc \
//        -lopencv_imgcodecs -lopencv_calib3d -lopencv_core -lpthread -o deck
#include "camera/camera_api.h"
#include <opencv2/opencv.hpp>
#include <opencv2/objdetect/aruco_detector.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

static double now_s(){ return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static int64_t mono_us(){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000; }

// ---------------- camera: newest NV12 frame ----------------
struct Frame { std::shared_ptr<std::vector<uint8_t>> buf; int w=0,h=0; int64_t ts=0; uint64_t seq=0; };
static std::mutex g_fm; static std::condition_variable g_fcv; static Frame g_latest; static uint64_t g_seq=0; static std::atomic<bool> g_stop{false};
static void frame_cb(camera_handle_t, camera_buffer_t* b, void*){
  if(b->frametype!=CAMERA_FRAMETYPE_NV12) return; const auto& d=b->framedesc.nv12; int w=d.width,h=d.height;
  auto buf=std::make_shared<std::vector<uint8_t>>((size_t)w*h*3/2);
  for(int r=0;r<h;r++) memcpy(buf->data()+(size_t)r*w,b->framebuf+(size_t)r*d.stride,w);
  for(int r=0;r<h/2;r++) memcpy(buf->data()+(size_t)w*h+(size_t)r*w,b->framebuf+d.uv_offset+(size_t)r*d.uv_stride,w);
  { std::lock_guard<std::mutex> lk(g_fm); g_latest.buf=buf; g_latest.w=w; g_latest.h=h; g_latest.ts=b->frametimestamp; g_latest.seq=++g_seq; }
  g_fcv.notify_all();
}
static void on_sig(int){ g_stop=true; g_fcv.notify_all(); }

// ---------------- scene: sheet orientation (ArUco), 16-button grid, sliders, dials ----------------
// Everything is ordered in SHEET coordinates: "up" comes from the printed ArUco corner markers (each marker's own
// top edge), so the camera can look at the sheets from any angle. Without markers, image-up is assumed.
struct Layout { bool ok=false; int direct=0; std::vector<cv::Point2f> quad[16]; cv::Rect2f box[16]; };
// Slider: printed outline as a perspective quad; H maps image -> slider frame (x 0..1 across, y 0 = bottom .. 1 = top)
struct Slider { cv::RotatedRect r; cv::Point2f top, bottom; float halfw; cv::Mat H; };
// Dial: printed circle seen as an ellipse; angles are measured on the un-squashed circle
struct Dial { cv::Point2f c; float rad; cv::RotatedRect e; };
static inline cv::Point2f slider_uv(const Slider& s, cv::Point2f p){ std::vector<cv::Point2f> a{p},b; cv::perspectiveTransform(a,b,s.H); return b[0]; }
static inline cv::Point2f dial_unit(const Dial& d, cv::Point2f p){   // point in the dial's circle frame (1 = on the ring)
  cv::Point2f v=p-d.e.center; float a=-d.e.angle*CV_PI/180.f, c=std::cos(a), s=std::sin(a);
  cv::Point2f r(v.x*c-v.y*s, v.x*s+v.y*c); return cv::Point2f(r.x/(d.e.size.width/2), r.y/(d.e.size.height/2)); }
struct Scene { Layout L; std::vector<Slider> sliders; std::vector<Dial> dials; cv::Point2f up{0,-1}; int markers=0; std::vector<cv::Point2f> paper, marker_pts; };
static inline cv::Point2f right_of(cv::Point2f up){ return cv::Point2f(-up.y,up.x); }   // sheet "right" in image coordinates

static void find_grid(const cv::Mat& gray, const std::vector<std::vector<cv::Point2f>>& rej, cv::Point2f up, Layout& L){
  struct Q{ std::vector<cv::Point2f> q; cv::Point2f m; double area; };
  std::vector<Q> qs; cv::Mat mask(gray.size(),CV_8U), ring(gray.size(),CV_8U);
  for(auto& q:rej){ double a=cv::contourArea(q); if(a<250) continue; mask=0; std::vector<cv::Point> p(q.begin(),q.end()); cv::fillConvexPoly(mask,p,255);
    cv::Point2f c=(q[0]+q[1]+q[2]+q[3])/4; std::vector<cv::Point> big; for(auto& v:q) big.push_back(c+(v-c)*1.3f);
    ring=0; cv::fillConvexPoly(ring,big,255); ring.setTo(0,mask);
    if(cv::mean(gray,mask)[0]<0.6*cv::mean(gray,ring)[0]) continue;     // dark squares are markers, buttons are paper-bright
    qs.push_back({q,c,a}); }
  if(qs.size()<10) return;
  std::vector<double> ar; for(auto& q:qs) ar.push_back(q.area); std::nth_element(ar.begin(),ar.begin()+ar.size()/2,ar.end()); double med=ar[ar.size()/2];
  qs.erase(std::remove_if(qs.begin(),qs.end(),[&](const Q& q){ return q.area<med*0.6||q.area>med*1.6; }),qs.end());
  { // grid buttons always have neighbours; drop isolated look-alikes (e.g. a square printed elsewhere)
    double side0=std::sqrt(med); std::vector<Q> keep;
    for(auto& q:qs){ int nb=0; for(auto& o:qs) if(&o!=&q && cv::norm(o.m-q.m)<1.7*side0) nb++; if(nb>=2) keep.push_back(q); }
    qs.swap(keep); }
  if(qs.size()<10) return;
  cv::Point2f rt=right_of(up), dn=-up;
  auto groups=[&](std::vector<double> v){ std::sort(v.begin(),v.end()); std::vector<std::pair<double,double>> g; for(size_t i=1;i<v.size();i++) g.push_back({v[i]-v[i-1],(v[i]+v[i-1])/2});
    std::sort(g.rbegin(),g.rend()); std::vector<double> t; for(int i=0;i<3&&i<(int)g.size();i++) t.push_back(g[i].second); std::sort(t.begin(),t.end()); return t; };
  std::vector<double> xs,ys; for(auto& q:qs){ xs.push_back(q.m.dot(rt)); ys.push_back(q.m.dot(dn)); }
  auto tx=groups(xs), ty=groups(ys); if(tx.size()<3||ty.size()<3) return; double side=std::sqrt(med);
  for(int i=0;i<2;i++) if(tx[i+1]-tx[i]<side*0.7||ty[i+1]-ty[i]<side*0.7) return;
  auto bin=[](double v,const std::vector<double>& t){ int k=0; while(k<3&&v>t[k]) k++; return k; };
  std::vector<int> cell(qs.size()); std::vector<cv::Point2f> src,dst; int used[16]={0};
  for(size_t i=0;i<qs.size();i++){ int c=bin(xs[i],tx), r=bin(ys[i],ty); cell[i]=r*4+c; used[cell[i]]++; src.push_back(cv::Point2f(c,r)); dst.push_back(qs[i].m); }
  for(int k=0;k<16;k++) if(used[k]>1) return;
  cv::Mat Hg=cv::findHomography(src,dst,cv::RANSAC,side*0.25); if(Hg.empty()) return;
  std::vector<cv::Point2f> proj; cv::perspectiveTransform(src,proj,Hg); int good=0; for(size_t i=0;i<proj.size();i++) if(cv::norm(proj[i]-dst[i])<side*0.25) good++;
  if(good<10) return;
  std::vector<cv::Point2f> unit={{0,0},{1,0}}, upt; cv::perspectiveTransform(unit,upt,Hg); float hs=float(side/cv::norm(upt[1]-upt[0])/2);
  Layout N; N.ok=true; N.direct=(int)qs.size();
  for(int k=0;k<16;k++){ int found=-1; for(size_t i=0;i<qs.size();i++) if(cell[i]==k) found=(int)i;
    if(found>=0) N.quad[k]=qs[found].q; else { int c=k%4,r=k/4; std::vector<cv::Point2f> gg={{c-hs,r-hs},{c+hs,r-hs},{c+hs,r+hs},{c-hs,r+hs}}; cv::perspectiveTransform(gg,N.quad[k],Hg); }
    N.box[k]=cv::boundingRect(N.quad[k]); }
  L=N;
}

// Printed outlines: inner contours of dark lines. Tall rectangles = sliders, round shapes = dials (sizes relative to the
// buttons when the grid is visible, so it works at any camera height).
static void find_panel(const cv::Mat& gray, Scene& S){
  cv::Mat th; cv::adaptiveThreshold(gray,th,255,cv::ADAPTIVE_THRESH_MEAN_C,cv::THRESH_BINARY_INV,31,12);
  std::vector<std::vector<cv::Point>> cs; std::vector<cv::Vec4i> hier; cv::findContours(th,cs,hier,cv::RETR_CCOMP,cv::CHAIN_APPROX_SIMPLE);
  double unit=0; if(S.L.ok){ for(int k=0;k<16;k++) unit+=cv::contourArea(S.L.quad[k]); unit/=16; }
  double minA= unit>0? unit*0.6 : 300, minDial= unit>0? unit*1.5 : 1200;
  cv::Point2f up=S.up, rt=right_of(up);
  std::vector<Slider> sl; std::vector<Dial> dl;
  for(size_t i=0;i<cs.size();i++){ if(hier[i][3]<0) continue; double A=cv::contourArea(cs[i]); if(A<minA) continue;
    double per=cv::arcLength(cs[i],true), circ=4*CV_PI*A/(per*per); cv::RotatedRect rr=cv::minAreaRect(cs[i]);
    float lo=std::max(rr.size.width,rr.size.height), sh=std::min(rr.size.width,rr.size.height); double fill=A/(rr.size.area()+1e-6);
    cv::Point2f c=rr.center;
    if(S.markers>=4 && S.paper.size()>=3 && cv::pointPolygonTest(S.paper,c,false)<0) continue;   // only shapes printed on the sheets
    bool in_grid=false; if(S.L.ok) for(int k=0;k<16;k++) if(cv::pointPolygonTest(S.L.quad[k],c,false)>=0) in_grid=true;
    if(in_grid) continue;
    if(lo/std::max(sh,1.f)>2.5f && fill>0.8){ cv::Point2f p[4]; rr.points(p);
      int si=0; float sm=1e9f; for(int k=0;k<4;k++){ float l=cv::norm(p[(k+1)%4]-p[k]); if(l<sm){ sm=l; si=k; } }
      cv::Point2f e1=(p[si]+p[(si+1)%4])*0.5f, e2=(p[(si+2)%4]+p[(si+3)%4])*0.5f;
      bool e1top=e1.dot(up)>e2.dot(up);
      // true perspective corners: 4-vertex approximation of the outline (fallback: the rotated rectangle)
      std::vector<cv::Point> ap; cv::approxPolyDP(cs[i],ap,0.04*per,true); std::vector<cv::Point2f> v4;
      if(ap.size()==4) for(auto& q:ap) v4.push_back(q); else v4.assign(p,p+4);
      std::sort(v4.begin(),v4.end(),[&](const cv::Point2f& a,const cv::Point2f& b){ return a.dot(up)<b.dot(up); });   // first two = bottom end
      cv::Point2f rtv=right_of(up); if(v4[0].dot(rtv)>v4[1].dot(rtv)) std::swap(v4[0],v4[1]); if(v4[2].dot(rtv)>v4[3].dot(rtv)) std::swap(v4[2],v4[3]);
      std::vector<cv::Point2f> dst={{0,0},{1,0},{0,1},{1,1}};   // bottom-left, bottom-right, top-left, top-right
      cv::Mat Hs=cv::getPerspectiveTransform(v4,dst);
      sl.push_back({rr, e1top?e1:e2, e1top?e2:e1, sh/2, Hs}); }
    else if(circ>0.8 && A>=minDial && cs[i].size()>=5){ cv::RotatedRect e=cv::fitEllipse(cs[i]); dl.push_back({e.center,(e.size.width+e.size.height)/4,e}); } }
  std::sort(sl.begin(),sl.end(),[&](const Slider&a,const Slider&b){ return a.r.center.dot(rt)<b.r.center.dot(rt); });
  std::sort(dl.begin(),dl.end(),[&](const Dial&a,const Dial&b){ return a.c.dot(rt)<b.c.dot(rt); });
  if(sl.size()>=S.sliders.size()) S.sliders=sl;     // keep the previous set while a hand hides part of it
  if(dl.size()>=S.dials.size()) S.dials=dl;
}

static int g_stable=0;
static void find_scene(const cv::Mat& gray, Scene& S){
  static cv::aruco::ArucoDetector det(cv::aruco::getPredefinedDictionary(cv::aruco::DICT_4X4_50), cv::aruco::DetectorParameters());
  std::vector<int> ids; std::vector<std::vector<cv::Point2f>> mk, rej; det.detectMarkers(gray,mk,ids,rej);
  if(!mk.empty()){
    std::vector<float> ax,ay; std::vector<cv::Point2f> corners;
    for(auto& q:mk){ cv::Point2f u=q[0]-q[3]; float n=std::sqrt(u.dot(u)); if(n>0){ ax.push_back(u.x/n); ay.push_back(u.y/n); } for(auto& v:q) corners.push_back(v); }
    std::nth_element(ax.begin(),ax.begin()+ax.size()/2,ax.end()); std::nth_element(ay.begin(),ay.begin()+ay.size()/2,ay.end());
    cv::Point2f u(ax[ax.size()/2],ay[ay.size()/2]); float n=std::sqrt(u.dot(u)); if(n>0.5f) S.up=u*(1.f/n);
    S.markers=(int)mk.size(); S.marker_pts=corners;
  }
  size_t ns=S.sliders.size(), nd=S.dials.size(); bool had=S.L.ok;
  Layout N; find_grid(gray,rej,S.up,N);
  if(N.ok && (!S.L.ok || g_stable<3 || N.direct>=14)) S.L=N;     // once locked, only accept a clean (unobstructed) grid
  find_panel(gray,S);
  bool same=(S.L.ok==had) && S.sliders.size()==ns && S.dials.size()==nd && (S.L.ok||!S.sliders.empty());
  static bool oriented=false; if(S.markers>0) oriented=true;      // never lock before the markers have given the sheet orientation
  g_stable= (same && oriented)? g_stable+1 : 0;
  // search region for fingertips: the controls and markers, grown a little (keeps tape/clutter off the sheets out)
  std::vector<cv::Point2f> all=S.marker_pts;
  if(S.L.ok) for(int i=0;i<16;i++) for(auto& q:S.L.quad[i]) all.push_back(q);
  for(auto& sl:S.sliders){ cv::Point2f p4[4]; sl.r.points(p4); for(auto& q:p4) all.push_back(q); }
  for(auto& d:S.dials) for(int k=0;k<8;k++) all.push_back(d.c+d.rad*cv::Point2f(std::cos(k*CV_PI/4),std::sin(k*CV_PI/4)));
  if(all.size()>=3){ std::vector<cv::Point2f> h; cv::convexHull(all,h); cv::Point2f c(0,0); for(auto& v:h) c+=v; c*=1.f/h.size();
    S.paper.clear(); for(auto& v:h) S.paper.push_back(c+(v-c)*1.12f); }
}

// ---------------- UDP to dj_engine ----------------
static int g_udp=-1; static sockaddr_in g_dst{};
static void dj(const char* fmt,...){ if(g_udp<0) return; char b[256]; va_list ap; va_start(ap,fmt); int n=vsnprintf(b,sizeof b,fmt,ap); va_end(ap);
  sendto(g_udp,b,n,0,(sockaddr*)&g_dst,sizeof g_dst); }

int main(int argc,char** argv){
  std::string djaddr="127.0.0.1:9000"; double dwell_ms=250, release_s=0.3, skin_th=10, skin_ymin=70, skin_grow=1.3; int skin_min=60; double tip_back=0.25; int dial_engage=6;
  for(int i=1;i<argc;i++){ std::string a=argv[i]; auto next=[&]{ if(i+1>=argc){ fprintf(stderr,"missing value for %s\n",a.c_str()); exit(2);} return std::string(argv[++i]); };
    if(a=="--dj") djaddr=next(); else if(a=="--dwell") dwell_ms=atof(next().c_str()); else if(a=="--release") release_s=atof(next().c_str());
    else if(a=="--tip-back") tip_back=atof(next().c_str()); else if(a=="--dial-frames") dial_engage=atoi(next().c_str());
    else if(a=="--skin-th") skin_th=atof(next().c_str()); else if(a=="--skin-ymin") skin_ymin=atof(next().c_str());
    else { fprintf(stderr,"usage: deck [--dj host:port|off] [--dwell 250] [--release 0.3] [--skin-th 8] [--skin-ymin 40]\n"); return 2; } }
  signal(SIGINT,on_sig); signal(SIGTERM,on_sig);
  if(djaddr!="off"){ g_udp=socket(AF_INET,SOCK_DGRAM,0); g_dst.sin_family=AF_INET; g_dst.sin_port=htons(atoi(djaddr.substr(djaddr.find(':')+1).c_str()));
    inet_pton(AF_INET,djaddr.substr(0,djaddr.find(':')).c_str(),&g_dst.sin_addr); fprintf(stderr,"[dj] sending to udp %s\n",djaddr.c_str()); }

  camera_handle_t h=CAMERA_HANDLE_INVALID; int err=camera_open(CAMERA_UNIT_3,CAMERA_MODE_PREAD|CAMERA_MODE_PWRITE|CAMERA_MODE_DREAD,&h);
  if(err){ fprintf(stderr,"camera_open failed: %d (close other camera apps)\n",err); return 1; }
  camera_set_vf_property(h,CAMERA_IMGPROP_CREATEWINDOW,0); camera_set_vf_property(h,CAMERA_IMGPROP_FRAMERATE,30.0);
  if((err=camera_start_viewfinder(h,frame_cb,nullptr,nullptr))){ fprintf(stderr,"camera_start_viewfinder failed: %d\n",err); return 1; }
  fprintf(stderr,"[ready] skin-colour fingertip tracking; buttons + sliders + dials\n");

  Scene S; double last_find=0, t_start=now_s(), fps_t=now_s(), fps=0; int frames=0; uint64_t seen=0; bool announced=false;
  int pressed=-1, dwell_tip=-1; double tip_since=0, pressed_seen=0;
  float fv[4]={0.8f,0.5f,0.5f,0.8f}; int sl_active=-1, sl_frames=0; double sl_last=0, sl_sent=0; float sent_v[4]={-1,-1,-1,-1};
  int dial_active=-1, dial_frames=0; double dial_last=0; float dial_deg[2]={0,0}, dial_raw_last=0, dial_unwrap=0; bool dial_have=false;
  cv::Point2f tip_pt(-1,-1); int last_emit_state=-999; double last_emit=0; unsigned prev_hands=0;

  while(!g_stop){
    Frame f; { std::unique_lock<std::mutex> lk(g_fm); g_fcv.wait(lk,[&]{ return g_stop || g_latest.seq>seen; }); if(g_stop) break; f=g_latest; seen=f.seq; }
    frames++; if(now_s()-fps_t>=1){ fps=frames/(now_s()-fps_t); frames=0; fps_t=now_s(); }
    cv::Mat Y(f.h,f.w,CV_8U,f.buf->data()), gray; cv::resize(Y,gray,cv::Size(f.w/2,f.h/2),0,0,cv::INTER_AREA);
    if(now_s()-last_find>(g_stable>=3?2.0:0.3) || (!S.L.ok && S.sliders.empty() && S.dials.empty())){ last_find=now_s(); find_scene(gray,S); }
    Layout& L=S.L; auto& Psl=S.sliders; auto& Pdl=S.dials;
    if(!L.ok && Psl.empty() && Pdl.empty()){ if(now_s()-t_start>2 && !announced){ fprintf(stderr,"[board] looking for buttons / sliders / dials...\n"); announced=true; } continue; }
    if(!announced || announced==true){ static bool told=false; if(!told){ told=true; fprintf(stderr,"[board] found: %s%d sliders, %d dials (%d markers, sheet up = %.2f,%.2f)\n",L.ok?"16 buttons, ":"",(int)Psl.size(),(int)Pdl.size(),S.markers,S.up.x,S.up.y); } }

    // ---- skin-colour fingertip, half resolution (1152x648): fingers stay several pixels wide even with a high camera ----
    cv::Mat UVh(f.h/2,f.w/2,CV_8UC2,f.buf->data()+(size_t)f.w*f.h);     // NV12 chroma plane is already half resolution
    std::vector<cv::Point2f> pts;    // every control's outline, half-res
    if(L.ok) for(int i=0;i<16;i++) for(auto& q:L.quad[i]) pts.push_back(q);
    for(auto& s:Psl){ cv::Point2f p4[4]; s.r.points(p4); for(auto& q:p4) pts.push_back(q); }
    for(auto& dl:Pdl) for(int k=0;k<8;k++) pts.push_back(dl.c+dl.rad*cv::Point2f(std::cos(k*CV_PI/4),std::sin(k*CV_PI/4)));
    std::vector<cv::Point2f> hull; cv::convexHull(pts,hull);
    // search region: the sheets themselves (marker hull) when known, else the controls grown a bit
    std::vector<cv::Point> rp;
    if(S.paper.size()>=3) for(auto& q:S.paper) rp.push_back(cv::Point(cvRound(q.x),cvRound(q.y)));
    else { cv::Point2f c(0,0); for(auto& q:hull) c+=q; c*=1.f/hull.size(); for(auto& q:hull) rp.push_back(cv::Point(cvRound(c.x+(q.x-c.x)*skin_grow),cvRound(c.y+(q.y-c.y)*skin_grow))); }
    std::vector<cv::Point> gp; for(auto& q:hull) gp.push_back(cv::Point(cvRound(q.x),cvRound(q.y)));
    // gm = the controls themselves (buttons, sliders, dials, slightly enlarged): the patch that lies most on controls is the finger
    cv::Mat region=cv::Mat::zeros(gray.size(),CV_8U), gm=cv::Mat::zeros(gray.size(),CV_8U); cv::fillConvexPoly(region,rp,255);
    if(L.ok) for(int i=0;i<16;i++){ cv::Point2f c=(L.quad[i][0]+L.quad[i][1]+L.quad[i][2]+L.quad[i][3])/4; std::vector<cv::Point> q; for(auto& v:L.quad[i]) q.push_back(c+(v-c)*1.3f); cv::fillConvexPoly(gm,q,255); }
    for(auto& sl:Psl){ cv::RotatedRect r=sl.r; r.size.width*=1.3f; r.size.height*=1.1f; cv::Point2f p4[4]; r.points(p4); std::vector<cv::Point> q(p4,p4+4); cv::fillConvexPoly(gm,q,255); }
    for(auto& dl:Pdl) cv::circle(gm,dl.c,(int)(dl.rad*1.15f),255,-1);
    (void)gp;
    cv::Rect rb=cv::boundingRect(rp)&cv::Rect(0,0,gray.cols,gray.rows);
    // local tint reference: median of the chroma over a wide neighbourhood (quarter res, 21 px = ~84 px here), so a
    // lighting tint that varies across the sheets cancels out, while a finger (narrow) still stands out
    cv::Mat dch(gray.size(),CV_8U); for(int y=0;y<gray.rows;y++){ const uint8_t* uv=UVh.ptr<uint8_t>(y); uint8_t* o=dch.ptr<uint8_t>(y);
      for(int x=0;x<gray.cols;x++){ int v=(int)uv[2*x+1]-(int)uv[2*x]+128; o[x]=(uint8_t)std::min(255,std::max(0,v)); } }
    cv::Mat dq,bgq,bg; cv::resize(dch,dq,cv::Size(gray.cols/4,gray.rows/4),0,0,cv::INTER_AREA); cv::medianBlur(dq,bgq,21); cv::resize(bgq,bg,gray.size(),0,0,cv::INTER_LINEAR);
    cv::Mat mask=cv::Mat::zeros(gray.size(),CV_8U);
    for(int y=rb.y;y<rb.y+rb.height;y++){ const uint8_t* dd=dch.ptr<uint8_t>(y); const uint8_t* bb=bg.ptr<uint8_t>(y); const uint8_t* yy=gray.ptr<uint8_t>(y); const uint8_t* g=region.ptr<uint8_t>(y); uint8_t* m=mask.ptr<uint8_t>(y);
      for(int x=rb.x;x<rb.x+rb.width;x++) m[x]=(g[x] && (int)dd[x]-(int)bb[x]>skin_th && yy[x]>skin_ymin)?255:0; }
    cv::morphologyEx(mask,mask,cv::MORPH_OPEN,cv::getStructuringElement(cv::MORPH_ELLIPSE,cv::Size(5,5)));
    cv::Mat lab,stats,cents; int nl=cv::connectedComponentsWithStats(mask,lab,stats,cents,8,CV_32S);
    std::vector<int> over(nl,0); for(int y=rb.y;y<rb.y+rb.height;y++){ const int* r=lab.ptr<int>(y); const uint8_t* g=gm.ptr<uint8_t>(y); for(int x=rb.x;x<rb.x+rb.width;x++) if(r[x] && g[x]) over[r[x]]++; }
    // Leading fingertip only: of the skin patches lying on the controls, take the point reaching furthest up the sheet
    // (when you point, the index finger leads; other fingertips and stray marks are ignored).
    int best=-1, bo=0; float lead=-1e9f; cv::Point2f lead_pt(-1,-1);
    for(int k=1;k<nl;k++){ if(over[k]<15 || stats.at<int>(k,cv::CC_STAT_AREA)<20) continue;
      cv::Rect bb(stats.at<int>(k,0),stats.at<int>(k,1),stats.at<int>(k,2),stats.at<int>(k,3));
      for(int y=bb.y;y<bb.y+bb.height;y++){ const int* r=lab.ptr<int>(y); for(int x=bb.x;x<bb.x+bb.width;x++) if(r[x]==k){ float t=x*S.up.x+y*S.up.y; if(t>lead){ lead=t; lead_pt=cv::Point2f(x,y); best=k; } } } }
    if(best>0) bo=over[best];
    tip_pt=cv::Point2f(-1,-1);
    if(best>0){
      // the skin edge seen is the front of the fingertip; the pad touching the paper sits about a quarter button behind it
      float pitch=20.f; if(L.ok){ double a=0; for(int i=0;i<16;i++) a+=cv::contourArea(L.quad[i]); pitch=(float)std::sqrt(a/16); }
      tip_pt=lead_pt-S.up*(tip_back*pitch);
    }

    // ---- which control is under the fingertip (dial > slider > button) ----
    int on_dial=-1, on_slider=-1, on_button=-1; float slider_v=0;
    if(tip_pt.x>=0){
      for(size_t k=0;k<Pdl.size() && k<2;k++){ cv::Point2f u=dial_unit(Pdl[k],tip_pt); if(u.dot(u)<=1.15f*1.15f){ on_dial=(int)k; break; } }
      if(on_dial<0) for(size_t k=0;k<Psl.size() && k<4;k++){ cv::Point2f uv=slider_uv(Psl[k],tip_pt);   // perspective-correct
        if(uv.x>-0.2f && uv.x<1.2f && uv.y>-0.05f && uv.y<1.05f){ on_slider=(int)k; slider_v=std::min(1.f,std::max(0.f,uv.y)); break; } }
      if(on_dial<0 && on_slider<0 && L.ok) for(int i=0;i<16;i++) if(cv::pointPolygonTest(L.quad[i],tip_pt,false)>=0){ on_button=i; break; } }

    // buttons: dwell to press, short grace before release
    if(on_button!=dwell_tip){ dwell_tip=on_button; tip_since=now_s(); }
    int newpressed=pressed; if(pressed>=0 && on_button==pressed) pressed_seen=now_s();
    if(on_button>=0 && on_button!=pressed && (now_s()-tip_since)*1000>=dwell_ms){ newpressed=on_button; pressed_seen=now_s(); dj("pad %d",on_button+1); }
    else if(pressed>=0 && on_button!=pressed && now_s()-pressed_seen>=release_s) newpressed=-1;
    if(newpressed!=pressed){ fprintf(stderr,newpressed>=0?">>> BUTTON %d PRESSED\n":"<<< released (button %d)\n",newpressed>=0?newpressed+1:pressed+1); pressed=newpressed; }

    // sliders: absolute position while the fingertip is on it (2 frames to engage), smoothed, <= 60 msgs/s
    static const char* SK[4]={"a.vol","a.filter","b.filter","b.vol"};
    if(on_slider>=0){ if(on_slider==sl_active) sl_frames++; else { sl_active=on_slider; sl_frames=1; } sl_last=now_s(); }
    else if(sl_active>=0 && now_s()-sl_last>0.2){ sl_active=-1; sl_frames=0; }
    if(on_slider>=0 && sl_frames>=2){ int k=on_slider; fv[k]=sl_frames==2? slider_v : fv[k]*0.5f+slider_v*0.5f;
      if(std::fabs(fv[k]-sent_v[k])>0.01f && now_s()-sl_sent>=1.0/60){ sl_sent=now_s(); sent_v[k]=fv[k];
        if(k==1||k==2){ float x=fv[k]*2-1; if(std::fabs(x)<0.06f) x=0; dj("%s %.3f",SK[k],x); } else dj("%s %.3f",SK[k],fv[k]); } }

    // dials: scratch while the fingertip circles the platter
    if(on_dial>=0){ if(on_dial==dial_active) dial_frames++; else if(dial_active<0){ dial_active=on_dial; dial_frames=1; dial_have=false; } dial_last=now_s(); }
    char deck=dial_active==1?'b':'a';
    if(dial_active>=0 && on_dial==dial_active){
      // angle on the sheet: un-squash the ellipse, then measure in sheet axes so clockwise on paper = forward
      cv::Point2f vu=dial_unit(Pdl[dial_active],tip_pt); float a=-Pdl[dial_active].e.angle*CV_PI/180.f;
      cv::Point2f vi(vu.x*std::cos(-a)-vu.y*std::sin(-a), vu.x*std::sin(-a)+vu.y*std::cos(-a));   // back to image axes, circle-scaled
      cv::Point2f rt2=right_of(S.up); float raw=-std::atan2(vi.dot(S.up), vi.dot(rt2))*180.f/CV_PI;   // sheet frame: clockwise = positive = forward
      if(dial_frames==dial_engage){ dj("%c.scratch 1",deck); fprintf(stderr,">>> SCRATCH deck %c\n",deck-32); }
      if(!dial_have){ dial_have=true; dial_raw_last=raw; dial_unwrap=0; }
      float dd=raw-dial_raw_last; if(dd>180) dd-=360; if(dd<-180) dd+=360; dial_raw_last=raw; dial_unwrap+=dd; dial_deg[dial_active]+=dd;
      if(dial_frames>=dial_engage) dj("%c.platter %.1f",deck,dial_unwrap);
    } else if(dial_active>=0 && now_s()-dial_last>0.25){ if(dial_frames>=dial_engage){ dj("%c.scratch 0",deck); fprintf(stderr,"<<< scratch released deck %c\n",deck-32); } dial_active=-1; dial_frames=0; }

    // ---- JSON for the page ----
    unsigned hands=0; if(on_button>=0) hands|=1u<<on_button;
    int state=pressed*1000+sl_active*100+(dial_active>=0 && dial_frames>=dial_engage? dial_active:9)*10;
    if(state!=last_emit_state || hands!=prev_hands || (sl_active>=0 && now_s()-last_emit>0.05) || (dial_active>=0 && now_s()-last_emit>0.05) || now_s()-last_emit>1.0){
      last_emit_state=state; prev_hands=hands; last_emit=now_s();
      printf("{\"t\":%.2f,\"pressed\":%d,\"tip\":%d,\"pinch\":%s,\"fader\":%d,\"faders\":[%.3f,%.3f,%.3f,%.3f],\"scratch\":%d,\"dials\":[%.1f,%.1f],\"sliders\":%d,\"ndials\":%d,\"latency_ms\":%.0f,\"cam_fps\":%.1f,\"hands\":[%s]}\n",
        now_s()-t_start,pressed+1,on_button+1,sl_active>=0?"true":"false",sl_active+1,fv[0],fv[1],fv[2],fv[3],(dial_active>=0&&dial_frames>=dial_engage)?dial_active+1:0,dial_deg[0],dial_deg[1],
        (int)Psl.size(),(int)Pdl.size(),(mono_us()-f.ts)/1000.0,fps,on_button>=0?std::to_string(on_button+1).c_str():"");
      fflush(stdout); }

    // debug image on request: touch ~/touchdemo/debug.req
    // debug.auto present: save a frame every 0.5 s while skin is on the controls (debug_0..19.png, rotating)
    static double last_auto=0; static int auto_n=0; bool auto_dbg=(bo>0 && now_s()-last_auto>=0.5 && access("debug.auto",F_OK)==0);
    if(auto_dbg || access("debug.req",F_OK)==0){ if(!auto_dbg) unlink("debug.req"); else last_auto=now_s(); cv::Mat dbg; cv::cvtColor(gray,dbg,cv::COLOR_GRAY2BGR); dbg.setTo(cv::Scalar(0,140,255),mask);
      if(S.paper.size()>=3){ std::vector<cv::Point> pp; for(auto& q:S.paper) pp.push_back(q); cv::polylines(dbg,pp,true,cv::Scalar(255,255,0),1); }
      for(int i=0;i<16 && L.ok;i++){ cv::Point2f c=(L.quad[i][0]+L.quad[i][1]+L.quad[i][2]+L.quad[i][3])/4; cv::putText(dbg,std::to_string(i+1),c+cv::Point2f(-6,5),0,0.4,cv::Scalar(255,0,0),1); }
      if(L.ok) for(int i=0;i<16;i++){ std::vector<cv::Point> q(L.quad[i].begin(),L.quad[i].end()); cv::polylines(dbg,q,true,cv::Scalar(0,200,0),1); }
      for(size_t k=0;k<Psl.size();k++){ cv::Point2f p4[4]; Psl[k].r.points(p4); std::vector<cv::Point> q(p4,p4+4); cv::polylines(dbg,q,true,(int)k==sl_active?cv::Scalar(0,0,255):cv::Scalar(0,200,0),2);
        cv::circle(dbg,Psl[k].top,4,cv::Scalar(255,0,0),-1); cv::putText(dbg,std::to_string(k+1),Psl[k].bottom+cv::Point2f(-5,18),0,0.6,cv::Scalar(255,0,0),2); }
      for(size_t k=0;k<Pdl.size();k++) cv::circle(dbg,Pdl[k].c,(int)Pdl[k].rad,(int)k==dial_active?cv::Scalar(0,0,255):cv::Scalar(0,200,0),2);
      if(tip_pt.x>=0) cv::circle(dbg,tip_pt,7,cv::Scalar(255,0,255),2);
      char lbl[160]; snprintf(lbl,sizeof lbl,"t=%.1f tip=%s button=%d slider=%d dial=%d pressed=%d skin=%d",now_s()-t_start,tip_pt.x>=0?"yes":"no",on_button+1,on_slider+1,on_dial+1,pressed+1,bo);
      cv::putText(dbg,lbl,cv::Point(8,22),0,0.6,cv::Scalar(0,0,255),2);
      std::string fn= auto_dbg? "debug_"+std::to_string(auto_n++%20)+".png" : std::string("debug.png");
      cv::imwrite(fn,dbg); if(!auto_dbg) fprintf(stderr,"[debug] wrote debug.png\n"); }
  }
  if(dial_active>=0 && dial_frames>=dial_engage) dj("%c.scratch 0",dial_active==1?'b':'a');
  camera_stop_viewfinder(h); camera_close(h); return 0;
}
