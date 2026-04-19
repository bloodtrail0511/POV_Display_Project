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
#define LED_NUM 10

// 要發送給硬體的資料結構 (BGR 格式)
typedef struct POV_Frame {
    uint8_t data[NUM_SLICES][LED_NUM*2][3];
}POV_Frame;

// 採樣用的查表 (Sampling LUT)
int lut_x[NUM_SLICES][LED_NUM*2];
int lut_y[NUM_SLICES][LED_NUM*2];

struct Paddle {
    float center;
    float speed;
    int   score;
    bool  moving_left;
    bool  moving_right;
};

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
            float d_A = r_step * j + (r_step*0.25);
            int x_A = center - d_A * sin(angle_A);
            int y_A = center + d_A * cos(angle_A);

            // Strip B (交錯：向外推半格)
            float d_B = r_step * j + (r_step*0.75);
            int x_B = center - d_B * sin(angle_B);
            int y_B = center + d_B * cos(angle_B);

            lut_x[i][LED_NUM-j-1] = MAX(0, MIN(x_B, side_len - 1));
            lut_y[i][LED_NUM-j-1] = MAX(0, MIN(y_B, side_len - 1));
            
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
void simulate_pov_display(const struct POV_Frame& buffer, cv::Mat& display_canvas) {
    // 每次畫圖前，把畫布塗黑 (不需要重新 new cv::Mat，省效能)
    display_canvas = cv::Scalar(0, 0, 0); 
    
    int sim_w = display_canvas.cols;
    int sim_h = display_canvas.rows;
    int sim_center = sim_w / 2;

    // ⭐️ 核心改變：我們不再用畫布大小去反推間距。
    // 我們讓間距固定 (例如每顆 LED 距離 8 個 pixels)，這代表硬體的物理密度
    float r_step_sim = 8.0; 
    
    // LED 點的大小也跟著間距固定
    int dot_size = MAX(1, (int)(r_step_sim * 0.3));
    // int dot_size = 2;

    for (int i = 0; i < NUM_SLICES; i++) {
        float angle_phys_A = i * 2.0 * M_PI / NUM_SLICES;
        float angle_phys_B = angle_phys_A + M_PI;

        for (int j = 0; j < LED_NUM; j++) {
            // Strip A 
            float d_A = r_step_sim * j + (r_step_sim*0.25); // 間距固定是 8.0，半徑就會隨著 j 變大而真實變大
            // float d_A = r_step_sim * j; // 間距固定是 8.0，半徑就會隨著 j 變大而真實變大
            int draw_x_A = sim_center - d_A * sin(angle_phys_A);
            int draw_y_A = sim_center + d_A * cos(angle_phys_A);
            
            cv::Scalar color_A(buffer.data[i][j + LED_NUM][0], buffer.data[i][j + LED_NUM][1], buffer.data[i][j + LED_NUM][2]);
            cv::circle(display_canvas, cv::Point(draw_x_A, draw_y_A), dot_size, color_A, -1, cv::LINE_AA);

            // Strip B (交錯半格)
            float d_B = r_step_sim * j + (r_step_sim*0.75);
            // float d_B = r_step_sim * j + (r_step_sim / 2.0);
            int draw_x_B = sim_center - d_B * sin(angle_phys_B);
            int draw_y_B = sim_center + d_B * cos(angle_phys_B);
            
            cv::Scalar color_B(buffer.data[i][LED_NUM-j-1][0], buffer.data[i][LED_NUM-j-1][1], buffer.data[i][LED_NUM-j-1][2]);
            cv::circle(display_canvas, cv::Point(draw_x_B, draw_y_B), dot_size, color_B, -1, cv::LINE_AA);
        }
    }
    
    cv::circle(display_canvas, cv::Point(sim_center, sim_h - 10), 5, cv::Scalar(0, 0, 255), -1);
}

// ==============================================================================
// 主程式 (POV 遊戲引擎骨架 - Step 1)
// ==============================================================================
// ==============================================================================
// 主程式 (POV 遊戲引擎 - Step 3: 玩家弧形擋板與鍵盤輸入)
// ==============================================================================
int main(int argc, char** argv) {
    int side_len = 800; 
    init_sampling_lut(side_len);
    POV_Frame pov_buffer; 

    cv::Mat game_canvas = cv::Mat::zeros(side_len, side_len, CV_8UC3);
    float r_step_sim = 8.0; 
    int sim_window_size = (int)(LED_NUM * r_step_sim * 2) + 40; 
    cv::Mat simulation_canvas = cv::Mat::zeros(sim_window_size, sim_window_size, CV_8UC3);

    cv::namedWindow("1. 2D Game Canvas", cv::WINDOW_AUTOSIZE);
    cv::namedWindow("2. POV Simulator", cv::WINDOW_AUTOSIZE);

    // --- 遊戲狀態初始化 ---
    int center_x = side_len / 2;
    int center_y = side_len / 2;
    int r_step = center_x / LED_NUM; 

    float ball_x = center_x;
    float ball_y = center_y;
    float ball_vx = 7; 
    float ball_vy = 8.4; 
    int ball_radius = r_step * 0.8; 
    int arena_radius = center_x - r_step; 

    // ⭐️ 新增：玩家擋板的角度與大小
    float p1_angle = 180.0; // 左邊玩家
    float p2_angle = 0.0;   // 右邊玩家
    float paddle_size = 25.0; // 擋板涵蓋的角度 (正負 25 度)
    float paddle_speed = 6.0; // 擋板移動速度

    printf("遊戲引擎啟動！請點擊 2D 畫布視窗，然後按 W/S 和 I/K 控制擋板。\n");

    while (1) {
        // --- A. 清空畫布 ---
        game_canvas = cv::Scalar(0, 0, 0);

        // --- B. 物理與輸入更新 ---
        ball_x += ball_vx;
        ball_y += ball_vy;

        // 碰撞邊界 (暫時維持撞牆反彈，方便我們測試板子)
        float dx = ball_x - center_x;
        float dy = ball_y - center_y;
        float distance = sqrt(dx * dx + dy * dy);
        if (distance + ball_radius > arena_radius) {
            float overlap = (distance + ball_radius) - arena_radius;
            ball_x -= (dx / distance) * overlap;
            ball_y -= (dy / distance) * overlap;
            float nx = dx / distance;
            float ny = dy / distance;
            float dot_product = (ball_vx * nx) + (ball_vy * ny);
            ball_vx -= 2 * dot_product * nx;
            ball_vy -= 2 * dot_product * ny;
        }

        // --- C. 遊戲繪圖 ---
        // 1. 畫出競技場的虛線或邊界 (改成畫一個很暗的灰圈當作軌道參考)
        cv::circle(game_canvas, cv::Point(center_x, center_y), arena_radius, cv::Scalar(30, 30, 30), 30);
        
        // 2. ⭐️ 畫出 Player 1 擋板 (左側，藍色弧線)
        cv::ellipse(game_canvas, cv::Point(center_x, center_y), cv::Size(arena_radius, arena_radius), 
                    0, p1_angle - paddle_size, p1_angle + paddle_size, cv::Scalar(255, 100, 50), 40);

        // 3. ⭐️ 畫出 Player 2 擋板 (右側，綠色弧線)
        cv::ellipse(game_canvas, cv::Point(center_x, center_y), cv::Size(arena_radius, arena_radius), 
                    0, p2_angle - paddle_size, p2_angle + paddle_size, cv::Scalar(50, 255, 100), 40);

        // 4. 畫乒乓球
        cv::circle(game_canvas, cv::Point((int)ball_x, (int)ball_y), ball_radius, cv::Scalar(0, 0, 255), -1);

        // --- D. 轉換與模擬 ---
        convert_to_pov_buffer(game_canvas, &pov_buffer);
        simulate_pov_display(pov_buffer, simulation_canvas);

        // --- E. 顯示與鍵盤輸入控制 ---
        cv::imshow("1. 2D Game Canvas", game_canvas);
        cv::imshow("2. POV Simulator", simulation_canvas);

        // ⭐️ 捕捉鍵盤輸入 (注意：滑鼠必須點擊在 OpenCV 的視窗上才有效)
        int key = cv::waitKey(16); 
        if (key == 'q' || key == 27) break;
        if (key == 'w') p1_angle -= paddle_speed;
        if (key == 's') p1_angle += paddle_speed;
        if (key == 'i') p2_angle -= paddle_speed;
        if (key == 'k') p2_angle += paddle_speed;
    }

    cv::destroyAllWindows();
    return 0;
}