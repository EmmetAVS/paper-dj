# Build on the Pi (QNX 8, aarch64). Needs: opencv-dev, libopencv_aruco (apk, repo.oss.qnx.com).
CXXFLAGS = -O2 -std=c++17 -I. -I/usr/include/opencv4
OPENCV   = -lopencv_objdetect -lopencv_imgproc -lopencv_imgcodecs -lopencv_calib3d -lopencv_core

all: deck snap

deck: deck.cpp camera/camera_api.h
	$(CXX) $(CXXFLAGS) deck.cpp -lcamapi -lsocket $(OPENCV) -lpthread -o $@

snap: tools/snap.c camera/camera_api.h
	$(CC) -O2 -I. tools/snap.c -lcamapi -o $@

# older multi-mode tracker (background-difference / Laya / pinch experiments); needs onnxruntime-dev
boarddemo: experiments/boarddemo.cpp camera/camera_api.h
	$(CXX) $(CXXFLAGS) -fpermissive -I/usr/include/onnxruntime experiments/boarddemo.cpp -lcamapi -lonnxruntime $(OPENCV) -lpthread -o $@

clean:
	rm -f deck snap boarddemo
