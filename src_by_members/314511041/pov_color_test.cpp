#include <opencv2/opencv.hpp>
#include <cmath>
#include <iostream>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int main() {
    // 設定畫布大小，800x800 與你們的專題設定一致
    const int CANVAS_SIZE = 800;
    cv::Mat hsv = cv::Mat::zeros(CANVAS_SIZE, CANVAS_SIZE, CV_8UC3);
    
    int center = CANVAS_SIZE / 2;
    int radius = center - 20; // 留一點黑邊

    for (int y = 0; y < CANVAS_SIZE; y++) {
        for (int x = 0; x < CANVAS_SIZE; x++) {
            double dx = x - center;
            double dy = y - center;
            double dist = std::sqrt(dx * dx + dy * dy);

            // 只在半徑範圍內畫圖
            if (dist <= radius) {
                // 1. 取得角度 (-180 ~ 180)，並轉為 0 ~ 360
                double angle = std::atan2(dy, dx) * 180.0 / M_PI;
                if (angle < 0) {
                    angle += 360.0;
                }

                // 2. 轉換為 OpenCV 的 HSV 格式
                // OpenCV 的 H 範圍是 0~179，所以角度要除以 2
                int h = static_cast<int>(angle / 2.0);
                
                // 飽和度 S：中心為 0 (白)，邊緣為 255 (最鮮豔)
                int s = static_cast<int>((dist / radius) * 255.0);
                
                // 明度 V：全部保持最亮 255
                int v = 255;

                hsv.at<cv::Vec3b>(y, x) = cv::Vec3b(h, s, v);
            }
        }
    }

    // 將 HSV 轉回 BGR 以便顯示與存檔
    cv::Mat bgr;
    cv::cvtColor(hsv, bgr, cv::COLOR_HSV2BGR);

    std::cout << "色相環產生完成！按下任意鍵退出..." << std::endl;

    // 顯示圖片
    cv::imshow("Standard Hue Ring", bgr);
    
    // 將圖片存下來，你可以隨時打開來看
    cv::imwrite("standard_hue_ring.png", bgr);
    
    cv::waitKey(0);
    return 0;
}