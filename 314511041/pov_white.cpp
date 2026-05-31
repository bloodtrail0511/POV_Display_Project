#include "pov_display.hpp"

#include <opencv2/opencv.hpp>
#include <unistd.h>
#include <cstdio>

double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

int main(int argc, char** argv) {

    cv::Mat frame;
    frame = cv::Mat(800, 800, CV_8UC3, cv::Scalar(255, 255, 255));
    if (frame.empty()) return -1;

    int side_len = std::min(frame.rows, frame.cols);
    int y_center = frame.rows / 2;
    int x_center = frame.cols / 2;

    POVDisplay::Config cfg;
    cfg.device_path = "/dev/pov_display";
    cfg.canvas_size = side_len;
    cfg.r_balance = 1.00;
    cfg.g_balance = 0.75;
    cfg.b_balance = 0.70;
    cfg.pixel_brightness_scale = 1.0;
    cfg.apa102_brightness = 5;
    cfg.auto_fit_canvas = false;

    POVDisplay pov(cfg);
    if (!pov.isOpen()) {
        printf("Error: 無法開啟 /dev/pov_display\n");
        return -1;
    }

    // cv::namedWindow("Original Cropped", cv::WINDOW_AUTOSIZE);

    // while (true) {
    //     cap >> frame;
    //     if (frame.empty()) {
    //         cap.set(cv::CAP_PROP_POS_FRAMES, 0);
    //         continue;
    //     }

    //     cv::Rect roi(x_center - side_len / 2, y_center - side_len / 2, side_len, side_len);
    //     cv::Mat frame_cropped = frame(roi);

    //     // cv::imshow("Original Cropped", frame_cropped);

    //     // 原本只 imshow；現在多加這行就送到 POV
    //     pov.show(frame_cropped);

    //     char key = (char)cv::waitKey(10);
    //     if (key == 'q' || key == 27) break;
    // }

    double target_fps = 10.0;
    double target_dt = 1.0 / target_fps;

    while (true) {
        double frame_start = now_sec();

        // 1. 產生畫面
        // cap >> frame;
        // if (frame.empty()) {
        //     cap.set(cv::CAP_PROP_POS_FRAMES, 0);
        //     continue;
        // }

        // cv::Rect roi(x_center - side_len / 2, y_center - side_len / 2, side_len, side_len);
        // cv::Mat frame_cropped = frame(roi);

        // 2. 顯示到螢幕
        // cv::imshow("canvas", canvas);

        // 3. 顯示到 POV
        pov.show(frame);

        // 4. 處理鍵盤，但不要用它主要控制時間
        // char key = (char)cv::waitKey(1);
        // if (key == 'q' || key == 27) break;

        // 5. 計算這一幀已花多久
        double elapsed = now_sec() - frame_start;

        // 6. 如果還沒到目標 frame time，就 sleep 剩下時間
        if (elapsed < target_dt) {
            usleep((useconds_t)((target_dt - elapsed) * 1000000.0));
        }
    }

    return 0;
}
