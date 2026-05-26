CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra
OPENCV_CFLAGS := $(shell pkg-config --cflags opencv4)
OPENCV_LIBS := $(shell pkg-config --libs opencv4)

TARGETS := app_clock_v1 menu app_pong_sim app_vid

.PHONY: all clean

all: $(TARGETS)

app_clock_v1: app_clock_v1.cpp
	$(CXX) $(CXXFLAGS) $(OPENCV_CFLAGS) $< -o $@ $(OPENCV_LIBS)

menu: menu.cpp
	$(CXX) $(CXXFLAGS) $(OPENCV_CFLAGS) $< -o $@ $(OPENCV_LIBS)

app_pong_sim: app_pong_sim.cpp
	$(CXX) $(CXXFLAGS) $(OPENCV_CFLAGS) $< -o $@ $(OPENCV_LIBS)

app_vid: app_vid.cpp
	$(CXX) $(CXXFLAGS) $(OPENCV_CFLAGS) $< -o $@ $(OPENCV_LIBS)

clean:
	rm -f $(TARGETS)
