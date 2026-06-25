#ifndef APP_CLOCK_V1_HPP
#define APP_CLOCK_V1_HPP

#include <cstdlib>
#include <stdio.h>

#include "menu_window.hpp"

// 從 menu 連到時鐘功能。
inline void run_clock_app() {
    cv::destroyAllWindows();
#ifdef _WIN32
    int result = std::system("app_clock_v1.exe");
#else
    int result = std::system("./app_clock_v1");
#endif
    if (result != 0) {
        printf("Failed to run clock app. Please build app_clock_v1 first.\n");
    }
    restore_menu_windows();
}

#endif
