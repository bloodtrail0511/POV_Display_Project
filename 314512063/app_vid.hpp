#ifndef APP_VID_HPP
#define APP_VID_HPP

#include <cstdlib>
#include <stdio.h>

#include "menu_window.hpp"

// 從 menu 連到 GIF/影片播放功能。
inline void run_video_app() {
    cv::destroyAllWindows();
#ifdef _WIN32
    int result = std::system("app_vid.exe");
#else
    int result = std::system("./app_vid");
#endif
    if (result != 0) {
        printf("Failed to run video app. Please build app_vid first and check pac_man.gif.\n");
    }
    restore_menu_windows();
}

#endif
