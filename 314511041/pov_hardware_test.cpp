#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>

#include <opencv2/opencv.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

// 引入你們封裝好的 POV Display Library
#include "pov_display.hpp"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ============================================================
// Test mode 說明 (已改為命令列參數輸入)
// ============================================================
// 0: OpenCV 畫一條 0/180 度紅色直徑線
// 1: OpenCV 畫四條不同顏色直徑線，測角度定位
// 2: OpenCV 畫多條細同心圓，測半徑解析度
// 3: OpenCV 畫 0 度紅線 + 旁邊一條很近的紅線，測解析度是否變細
// 4: OpenCV 畫斜線 X，測整體座標是否正確
// 5: 左上角紅色方形
// 6: 左上角紅色空心方形 + 中心十字線，方便確認方向
// 7: 讀取圖片檔，縮放後顯示 (需要額外帶圖片路徑)
// 8: 刷新率測試：每一幀重新畫移動方塊 → 轉 slice → 寫入 driver，並印出實際 FPS
// 9: 顏色校正，顯示色相環

#define TARGET_UPDATE_FPS 10
#define FPS_PRINT_INTERVAL_SEC 1.0
#define CANVAS_SIZE 800

// ============================================================
// 影像前處理參數 (僅針對讀取圖片檔 TEST_MODE == 7 使用)
// ============================================================
#define SATURATION_SCALE 2.5 
#define VALUE_SCALE 0.55 
#define WHITE_CUTOFF 255

static inline int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

// 根據角度和半徑計算畫布上的座標 (僅供畫測試圖使用)
static cv::Point point_from_angle_radius(int center, double angle_rad, double radius) {
    int x = (int)lrint(center - radius * sin(angle_rad));
    int y = (int)lrint(center + radius * cos(angle_rad));
    return cv::Point(x, y);
}

// 針對圖片檔的前處理
cv::Mat preprocess_image_for_pov(const cv::Mat &src) {
    cv::Mat img = src.clone();

    for (int y = 0; y < img.rows; y++) {
        for (int x = 0; x < img.cols; x++) {
            cv::Vec3b &p = img.at<cv::Vec3b>(y, x);
            if (p[2] > WHITE_CUTOFF && p[1] > WHITE_CUTOFF && p[0] > WHITE_CUTOFF) {
                p = cv::Vec3b(0, 0, 0);
            }
        }
    }

    cv::Mat hsv;
    cv::cvtColor(img, hsv, cv::COLOR_BGR2HSV);
    for (int y = 0; y < hsv.rows; y++) {
        for (int x = 0; x < hsv.cols; x++) {
            cv::Vec3b &p = hsv.at<cv::Vec3b>(y, x);
            p[1] = clamp_int((int)(p[1] * SATURATION_SCALE), 0, 255);
            p[2] = clamp_int((int)(p[2] * VALUE_SCALE), 0, 255);
        }
    }

    cv::Mat out;
    cv::cvtColor(hsv, out, cv::COLOR_HSV2BGR);
    return out;
}

// 產生測試圖案 (將 test_mode 作為參數傳入，內部改用 switch-case)
void draw_test_canvas(cv::Mat &canvas, int test_mode, int argc, char** argv) {
    canvas = cv::Mat::zeros(CANVAS_SIZE, CANVAS_SIZE, CV_8UC3);
    const int center = CANVAS_SIZE / 2;
    const int radius = center - 8;

    switch (test_mode) {
        case 0:
            cv::line(canvas, cv::Point(center, center - radius), cv::Point(center, center + radius), cv::Scalar(0, 0, 255), 3, cv::LINE_AA);
            break;
        case 1:
            cv::line(canvas, cv::Point(center, center - radius), cv::Point(center, center + radius), cv::Scalar(0, 0, 255), 3, cv::LINE_AA);
            cv::line(canvas, cv::Point(center - radius, center), cv::Point(center + radius, center), cv::Scalar(0, 255, 0), 3, cv::LINE_AA);
            cv::line(canvas, point_from_angle_radius(center, 45.0 * M_PI / 180.0, radius), point_from_angle_radius(center, 225.0 * M_PI / 180.0, radius), cv::Scalar(255, 0, 0), 3, cv::LINE_AA);
            cv::line(canvas, point_from_angle_radius(center, 135.0 * M_PI / 180.0, radius), point_from_angle_radius(center, 315.0 * M_PI / 180.0, radius), cv::Scalar(0, 255, 255), 3, cv::LINE_AA);
            break;
        case 2:
            for (int r = 20; r < radius; r += 20) {
                cv::circle(canvas, cv::Point(center, center), r, cv::Scalar(0, 0, 255), 2, cv::LINE_AA);
            }
            break;
        case 3:
            cv::line(canvas, cv::Point(center - 4, center - radius), cv::Point(center - 4, center + radius), cv::Scalar(0, 0, 255), 2, cv::LINE_AA);
            cv::line(canvas, cv::Point(center + 4, center - radius), cv::Point(center + 4, center + radius), cv::Scalar(0, 0, 255), 2, cv::LINE_AA);
            break;
        case 4:
            cv::line(canvas, cv::Point(center - radius, center - radius), cv::Point(center + radius, center + radius), cv::Scalar(0, 0, 255), 3, cv::LINE_AA);
            cv::line(canvas, cv::Point(center + radius, center - radius), cv::Point(center - radius, center + radius), cv::Scalar(0, 255, 0), 3, cv::LINE_AA);
            break;
        case 5:
            cv::rectangle(canvas, cv::Point(CANVAS_SIZE * 0.20, CANVAS_SIZE * 0.20), cv::Point(CANVAS_SIZE * 0.38, CANVAS_SIZE * 0.38), cv::Scalar(0, 0, 255), -1, cv::LINE_AA);
            break;
        case 6:
            cv::rectangle(canvas, cv::Point(CANVAS_SIZE * 0.20, CANVAS_SIZE * 0.20), cv::Point(CANVAS_SIZE * 0.42, CANVAS_SIZE * 0.42), cv::Scalar(0, 0, 255), 4, cv::LINE_AA);
            cv::line(canvas, cv::Point(center - 100, center), cv::Point(center + 100, center), cv::Scalar(0, 255, 255), 2, cv::LINE_AA);
            cv::line(canvas, cv::Point(center, center - 100), cv::Point(center, center + 100), cv::Scalar(0, 255, 255), 2, cv::LINE_AA);
            break;
        case 7:
            if (argc < 3) {
                fprintf(stderr, "錯誤：模式 7 需要圖片路徑！\n用法：sudo ./pov_hardware_test 7 [圖片路徑]\n");
                exit(1);
            }
            {
                cv::Mat img = cv::imread(argv[2], cv::IMREAD_COLOR);
                if (img.empty()) {
                    fprintf(stderr, "無法讀取圖片：%s\n", argv[2]);
                    exit(1);
                }
                img = preprocess_image_for_pov(img);
                double scale = std::min((double)CANVAS_SIZE / img.cols, (double)CANVAS_SIZE / img.rows);
                int new_w = (int)(img.cols * scale);
                int new_h = (int)(img.rows * scale);
                cv::Mat resized;
                cv::resize(img, resized, cv::Size(new_w, new_h), 0, 0, cv::INTER_AREA);
                int x0 = (CANVAS_SIZE - new_w) / 2;
                int y0 = (CANVAS_SIZE - new_h) / 2;
                resized.copyTo(canvas(cv::Rect(x0, y0, new_w, new_h)));
            }
            break;
        case 8:
            // 模式 8 是動態重新繪製，靜態畫布先只填背景
            cv::rectangle(canvas, cv::Point(CANVAS_SIZE * 0.20, CANVAS_SIZE * 0.20), cv::Point(CANVAS_SIZE * 0.38, CANVAS_SIZE * 0.38), cv::Scalar(0, 0, 255), -1, cv::LINE_AA);
            break;
        case 9:
            {
                cv::Mat hsv_mat = cv::Mat::zeros(CANVAS_SIZE, CANVAS_SIZE, CV_8UC3);
                for (int y = 0; y < CANVAS_SIZE; y++) {
                    for (int x = 0; x < CANVAS_SIZE; x++) {
                        double dx = x - center;
                        double dy = y - center;
                        double dist = std::sqrt(dx * dx + dy * dy);
                        
                        if (dist <= radius) {
                            double angle = std::atan2(dy, dx) * 180.0 / M_PI;
                            if (angle < 0) angle += 360.0;
                            int h = static_cast<int>(angle / 2.0);
                            int s = static_cast<int>((dist / radius) * 255.0);
                            int v = 255;
                            hsv_mat.at<cv::Vec3b>(y, x) = cv::Vec3b(h, s, v);
                        }
                    }
                }
                cv::cvtColor(hsv_mat, canvas, cv::COLOR_HSV2BGR);
            }
            break;
        default:
            fprintf(stderr, "未知測試模式：%d，請輸入 0 ~ 9 的數字。\n", test_mode);
            exit(1);
    }

    cv::circle(canvas, cv::Point(center, center), 3, cv::Scalar(255, 255, 255), -1, cv::LINE_AA);
}

// 產生動態更新率測試圖案
void draw_refresh_rate_canvas(cv::Mat &canvas, int frame_idx) {
    canvas = cv::Mat::zeros(CANVAS_SIZE, CANVAS_SIZE, CV_8UC3);
    const int center = CANVAS_SIZE / 2;
    const int margin = CANVAS_SIZE / 8;
    const int box = CANVAS_SIZE / 8;
    const int travel = CANVAS_SIZE - 2 * margin - box;
    int period_frames = TARGET_UPDATE_FPS * 2;
    if (period_frames < 2) period_frames = 2;

    int phase = frame_idx % (2 * period_frames);
    double t = (phase < period_frames) ? (double)phase / period_frames : (double)(2 * period_frames - phase) / period_frames;
    int x = margin + (int)lrint(travel * t);
    int y = margin;

    cv::Scalar color;
    switch ((frame_idx / 10) % 3) {
        case 0: color = cv::Scalar(0, 0, 255); break;
        case 1: color = cv::Scalar(0, 255, 0); break;
        default: color = cv::Scalar(255, 0, 0); break;
    }

    cv::rectangle(canvas, cv::Point(x, y), cv::Point(x + box, y + box), color, -1, cv::LINE_AA);
    cv::line(canvas, cv::Point(center - 70, center), cv::Point(center + 70, center), cv::Scalar(0, 255, 255), 2, cv::LINE_AA);
    cv::line(canvas, cv::Point(center, center - 70), cv::Point(center, center + 70), cv::Scalar(0, 255, 255), 2, cv::LINE_AA);
    if (frame_idx % 2 == 0) {
        cv::circle(canvas, cv::Point(CANVAS_SIZE - margin, CANVAS_SIZE - margin), 24, cv::Scalar(255, 255, 255), -1, cv::LINE_AA);
    }
}

// 執行更新率測試 (TEST_MODE 8)
int run_refresh_rate_test(POVDisplay& display) {
    printf("Refresh-rate test mode started. TARGET_UPDATE_FPS = %d\n", TARGET_UPDATE_FPS);
    
    cv::Mat canvas;
    int frame_idx = 0;
    double last_print = now_sec();
    double last_frame_start = now_sec();
    int frames_since_print = 0;

    while (1) {
        double frame_start = now_sec();

        draw_refresh_rate_canvas(canvas, frame_idx);
        
        // 核心：使用 Library 的 show 方法 (包含 convert + flush)
        if (!display.show(canvas)) {
             fprintf(stderr, "Display show failed\n");
             return -1;
        }

        frame_idx++;
        frames_since_print++;

        double now = now_sec();
        if (now - last_print >= FPS_PRINT_INTERVAL_SEC) {
            double fps = frames_since_print / (now - last_print);
            printf("actual update FPS = %.2f, frame = %d, frame interval ~= %.2f ms\n", fps, frame_idx, 1000.0 / fps);
            frames_since_print = 0;
            last_print = now;
        }

        double target_dt = 1.0 / (double)TARGET_UPDATE_FPS;
        double elapsed = now_sec() - frame_start;
        if (elapsed < target_dt) {
            usleep((useconds_t)((target_dt - elapsed) * 1000000.0));
        }
        last_frame_start = frame_start;
    }
    return 0;
}

int main(int argc, char** argv) {
    // 檢查是否有帶參數
    if (argc < 2) {
        fprintf(stderr, "使用方法:\n");
        fprintf(stderr, "  一般測試: sudo ./pov_hardware_test [0-6, 8-9]\n");
        fprintf(stderr, "  圖片測試: sudo ./pov_hardware_test 7 [圖片路徑]\n");
        fprintf(stderr, "範例:\n");
        fprintf(stderr, "  sudo ./pov_hardware_test 9\n");
        fprintf(stderr, "  sudo ./pov_hardware_test 7 my_photo.png\n");
        return -1;
    }

    // 將第一個輸入參數轉成整數 test_mode
    int test_mode = atoi(argv[1]);

    // 初始化 Library，使用預設 Config，如有需要可在此修改
    POVDisplay::Config cfg;
    cfg.canvas_size = CANVAS_SIZE; 
    cfg.r_balance = 1.00;
    cfg.g_balance = 0.75;
    cfg.b_balance = 0.70;
    cfg.pixel_brightness_scale = 1.0;
    cfg.apa102_brightness = 5;
    POVDisplay display(cfg);

    if (!display.isOpen()) {
        fprintf(stderr, "Cannot open POV device. Make sure driver is loaded and permissions are correct.\n");
        return -1;
    }

    // 動態更新率測試 (模式 8)
    if (test_mode == 8) {
        return run_refresh_rate_test(display);
    }

    cv::Mat canvas;
    draw_test_canvas(canvas, test_mode, argc, argv);
    
    printf("OpenCV test canvas generated (Mode %d).\n", test_mode);

    // 將 Canvas 轉換為極座標緩衝區資料
    if (!display.convert(canvas)) {
        fprintf(stderr, "Failed to convert canvas.\n");
        return -1;
    }
    
    // 可選：儲存反向取樣的預覽圖，檢查 LUT 運作是否正常
    // display.saveSampledPreview("/tmp/pov_preview.png");

    printf("Writing frame to POV device...\n");

    // 靜態圖案：重複寫入同一份 frame
    while (1) {
        if (!display.flush()) {
            fprintf(stderr, "Failed to write to device.\n");
            break;
        }
        usleep(100000); // 100 ms
    }

    return 0;
}