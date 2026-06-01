#ifndef APP_PONG_SIM_HPP
#define APP_PONG_SIM_HPP

#include <cstdlib>
#include <stdio.h>

#include "menu_window.hpp"

// 從 menu 連到 Pong 功能。
inline void run_pong_app() {
    cv::destroyAllWindows();
#ifdef _WIN32
    int result = std::system("app_pong_sim.exe");
#else
    int result = std::system("./app_pong_sim");
#endif
    if (result != 0) {
        printf("Failed to run pong app. Please build app_pong_sim first.\n");
    }
    restore_menu_windows();
}

#endif
