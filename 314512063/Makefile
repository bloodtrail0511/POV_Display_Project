CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra
OPENCV_CFLAGS := $(shell pkg-config --cflags opencv4)
OPENCV_LIBS := $(shell pkg-config --libs opencv4)

TARGETS := app_clock_v1 menu menu_v2 app_pong_sim app_vid
MENU_HEADERS := app_clock_v1.hpp app_vid.hpp app_pong_sim.hpp menu_window.hpp
POV_DISPLAY_SRC := ../314511041/pov_display.cpp
POV_DISPLAY_HEADER := ../314511041/pov_display.hpp

.PHONY: all clean

all: $(TARGETS)

app_clock_v1: app_clock_v1.cpp
	$(CXX) $(CXXFLAGS) $(OPENCV_CFLAGS) $< -o $@ $(OPENCV_LIBS)

menu: menu.cpp $(MENU_HEADERS) app_clock_v1 app_vid app_pong_sim
	$(CXX) $(CXXFLAGS) $(OPENCV_CFLAGS) $< -o $@ $(OPENCV_LIBS)

menu_v2: menu_v2.cpp $(MENU_HEADERS) $(POV_DISPLAY_SRC) $(POV_DISPLAY_HEADER) app_clock_v1 app_vid app_pong_sim
	$(CXX) $(CXXFLAGS) $(OPENCV_CFLAGS) menu_v2.cpp $(POV_DISPLAY_SRC) -o $@ $(OPENCV_LIBS)

app_pong_sim: app_pong_sim.cpp
	$(CXX) $(CXXFLAGS) $(OPENCV_CFLAGS) $< -o $@ $(OPENCV_LIBS)

app_vid: app_vid.cpp
	$(CXX) $(CXXFLAGS) $(OPENCV_CFLAGS) $< -o $@ $(OPENCV_LIBS)

clean:
	rm -f $(TARGETS)
