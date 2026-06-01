#ifndef MENU_WINDOW_HPP
#define MENU_WINDOW_HPP

#include <opencv2/highgui.hpp>

inline void restore_menu_windows() {
    cv::namedWindow("1. Menu Canvas", cv::WINDOW_NORMAL);
    cv::namedWindow("2. POV Simulator", cv::WINDOW_NORMAL);
    cv::resizeWindow("1. Menu Canvas", 600, 600);
    cv::resizeWindow("2. POV Simulator", 520, 520);
}

#endif
