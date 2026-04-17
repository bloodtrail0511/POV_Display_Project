#include <stdio.h>
#include <stdint.h>
#include <math.h>
// #include <fcntl.h>  // PC 模擬不需要
// #include <unistd.h> // PC 模擬不需要

// OpenCV 核心與顯示模組
#include <opencv2/opencv.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#define NUM_SLICES 360
#define LED_NUM 30

// 解決 MAX/MIN 巨集重複定義警告
#undef MAX
#undef MIN
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// 要發送給硬體的資料結構 (BGR 格式)
typedef struct POV_Frame {
    uint8_t data[NUM_SLICES][LED_NUM*2][3];
}POV_Frame;
// uint8_t data[NUM_SLICES][LED_NUM*2][3];

// 採樣用的查表 (Sampling LUT)
int lut_x[NUM_SLICES][LED_NUM*2];
int lut_y[NUM_SLICES][LED_NUM*2];

// 初始化採樣查表 (使用你習慣的公式)
// 0度在正下方，順時針採樣 (下 -> 左 -> 上 -> 右)
void init_sampling_lut(int side_len) {
    int center = side_len / 2;
    float r_step = (float)center / LED_NUM;

    for (int i = 0; i < NUM_SLICES; i++) {
        float angle_A = i * 2.0 * M_PI / NUM_SLICES;
        float angle_B = angle_A + M_PI; // Strip B 在對面

        for (int j = 0; j < LED_NUM; j++) {
            // Strip A
            float d_A = r_step * j;
            int x_A = center - d_A * sin(angle_A);
            int y_A = center + d_A * cos(angle_A);
            // lut_x_A[i][j] = MAX(0, MIN(x_A, side_len - 1));
            // lut_y_A[i][j] = MAX(0, MIN(y_A, side_len - 1));

            // Strip B (交錯：向外推半格)
            float d_B = r_step * j + (r_step / 2.0);
            int x_B = center - d_B * sin(angle_B);
            int y_B = center + d_B * cos(angle_B);
            // lut_x_B[i][j] = MAX(0, MIN(x_B, side_len - 1));
            // lut_y_B[i][j] = MAX(0, MIN(y_B, side_len - 1));

            lut_x[i][j] = MAX(0, MIN(x_B, side_len - 1));
            lut_y[i][j] = MAX(0, MIN(y_B, side_len - 1));
            
            lut_x[i][j+LED_NUM] = MAX(0, MIN(x_A, side_len - 1));
            lut_y[i][j+LED_NUM] = MAX(0, MIN(y_A, side_len - 1));
        }
    }
}

// 將 OpenCV 影像轉換為硬體 Buffer (封裝好的 function)
void convert_to_pov_buffer(const cv::Mat& frame_cropped, POV_Frame* buffer) {
    uint8_t* raw_pixels = (uint8_t*)frame_cropped.data;
    int step = frame_cropped.step; 
    int channels = 3; 

    for (int i = 0; i < NUM_SLICES; i++) {
        for (int j = 0; j < LED_NUM*2; j++) {
            // // Strip A 採樣
            // int offset_A = (lut_y_A[i][j] * step) + (lut_x_A[i][j] * channels);
            // buffer->data_A[i][j][0] = raw_pixels[offset_A + 0]; // B
            // buffer->data_A[i][j][1] = raw_pixels[offset_A + 1]; // G
            // buffer->data_A[i][j][2] = raw_pixels[offset_A + 2]; // R

            // // Strip B 採樣
            // int offset_B = (lut_y_B[i][j] * step) + (lut_x_B[i][j] * channels);
            // buffer->data_B[i][j][0] = raw_pixels[offset_B + 0];
            // buffer->data_B[i][j][1] = raw_pixels[offset_B + 1];
            // buffer->data_B[i][j][2] = raw_pixels[offset_B + 2];

            int offset = lut_y[i][j] * step + lut_x[i][j] * channels;
            buffer->data[i][j][0] = raw_pixels[offset + 0]; // B
            buffer->data[i][j][1] = raw_pixels[offset + 1]; // G
            buffer->data[i][j][2] = raw_pixels[offset + 2]; // R
        }
    }
}

// ==============================================================================
// 🆕 電腦模擬顯示函式 (取代 write syscall)
// 這是採樣公式的「逆運算」，用來驗證資料是否正確。
// ==============================================================================
void simulate_pov_display(const POV_Frame& buffer, cv::Mat& display_canvas) {
    // 確保畫布是乾淨的黑色
    display_canvas = cv::Mat::zeros(display_canvas.size(), CV_8UC3);
    
    int sim_w = display_canvas.cols;
    int sim_h = display_canvas.rows;
    int sim_center = sim_w / 2;
    // 為了美觀，縮小一點模擬半徑，留點邊框
    float r_step_sim = (float)(sim_center * 0.9) / LED_NUM; 
    
    // LED 點的大小 (類似 Python 的 circle_radius)
    int dot_size = MAX(1, (int)(r_step_sim * 0.6));

    for (int i = 0; i < NUM_SLICES; i++) {
        // 物理旋轉角度 (對應採樣時的 A 角度)
        float angle_phys_A = i * 2.0 * M_PI / NUM_SLICES;
        // Strip B 的物理位置永遠在 A 的對面
        float angle_phys_B = angle_phys_A + M_PI;

        for (int j = 0; j < LED_NUM; j++) {
            // -----------------------------------------------------------
            // 畫出 Strip A 的 LED 狀態
            // -----------------------------------------------------------
            float d_A = r_step_sim * j;
            // 使用跟你採樣時完全一樣的幾何公式 (Polar -> Cartesian)
            // 修正：變數名稱改回 angle_phys_A
            int draw_x_A = sim_center - d_A * sin(angle_phys_A);
            int draw_y_A = sim_center + d_A * cos(angle_phys_A);
            
            // 從 Buffer 中取出 BGR 資料
            cv::Scalar color_A(buffer.data[i][j + LED_NUM][0], 
                               buffer.data[i][j + LED_NUM][1], 
                               buffer.data[i][j + LED_NUM][2]);
            
            // 在模擬畫布上畫一個圓點代表 LED
            cv::circle(display_canvas, cv::Point(draw_x_A, draw_y_A), dot_size, color_A, -1, cv::LINE_AA);

            // -----------------------------------------------------------
            // 畫出 Strip B 的 LED 狀態 (交錯排列)
            // -----------------------------------------------------------
            // 關鍵：物理半徑加上半格，且使用對面的角度
            float d_B = r_step_sim * j + (r_step_sim / 2.0);
            int draw_x_B = sim_center - d_B * sin(angle_phys_B);
            int draw_y_B = sim_center + d_B * cos(angle_phys_B);
            
            cv::Scalar color_B(buffer.data[i][j][0], 
                               buffer.data[i][j][1], 
                               buffer.data[i][j][2]);
            
            cv::circle(display_canvas, cv::Point(draw_x_B, draw_y_B), dot_size, color_B, -1, cv::LINE_AA);
        }
    }
    
    // 畫一個小紅點標示感測器位置 (正下方)
    cv::circle(display_canvas, cv::Point(sim_center, sim_h - 10), 5, cv::Scalar(0, 0, 255), -1);
}
// ==============================================================================
// 主程式 (PC 模擬版)
// ==============================================================================
int main(int argc, char** argv) {
    // 預設影片檔名
    const char* filename = "bad_apple.mp4";
    if (argc > 1) filename = argv[1]; // 允許從命令列輸入檔名

    cv::VideoCapture cap(filename);
    if (!cap.isOpened()) {
        printf("Error: 無法開啟影片檔案 %s！\n", filename);
        return -1;
    }

    cv::Mat frame;
    cap >> frame;
    if (frame.empty()) return -1;

    // 計算採樣尺寸
    int side_len = MIN(frame.rows, frame.cols);
    int center = side_len / 2;
    int y_center = frame.rows / 2;
    int x_center = frame.cols / 2;

    // 1. 初始化採樣查表
    printf("正在建立採樣查表 (LUT)... 尺寸: %d x %d\n", side_len, side_len);
    init_sampling_lut(side_len);
    
    // 這是我們要發送給硬體的真實資料 Buffer
    POV_Frame pov_buffer; 

    // 2. 準備模擬用的畫布與視窗
    int sim_window_size = 600; // 固定模擬視窗大小，方便觀察
    cv::Mat simulation_canvas = cv::Mat::zeros(sim_window_size, sim_window_size, CV_8UC3);
    cv::namedWindow("NTU POV 電腦模擬器 (雙燈條交錯版)", cv::WINDOW_AUTOSIZE);

    printf("開始執行模擬迴圈... 按 'q' 鍵退出。\n");

    // 3. 即時模擬迴圈
    while (1) {
        cap >> frame;
        if (frame.empty()) {
            cap.set(cv::CAP_PROP_POS_FRAMES, 0); // 自動重播
            continue;
        }

        // 裁切正方形 (這是你的 Game Canvas)
        cv::Rect roi(x_center - center, y_center - center, side_len, side_len);
        cv::Mat frame_cropped = frame(roi);

        // --- 核心步驟 ---
        // A. 模擬 User Space：將畫布轉為硬體 Buffer 資料
        convert_to_pov_buffer(frame_cropped, &pov_buffer);

        // B. 模擬視覺殘留：讀取 Buffer 資料，反向畫出旋轉效果
        simulate_pov_display(pov_buffer, simulation_canvas);

        // 4. 顯示結果
        cv::imshow("NTU POV 電腦模擬器 (雙燈條交錯版)", simulation_canvas);
        // 可選：顯示原始裁切畫面供對比
        // cv::imshow("Original Cropped", frame_cropped);

        // 控制播放速度，按 'q' 退出
        char key = (char)cv::waitKey(10); 
        if (key == 'q' || key == 27) break;
    }

    cv::destroyAllWindows();
    printf("模擬結束。\n");
    return 0;
}