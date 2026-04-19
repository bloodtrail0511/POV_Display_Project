#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <stdlib.h>

// OpenCV 核心與顯示模組
#include <opencv2/opencv.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#define NUM_SLICES 360
#define LED_NUM 10

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
} POV_Frame;

// 採樣用的查表 (Sampling LUT)
int lut_x[NUM_SLICES][LED_NUM*2];
int lut_y[NUM_SLICES][LED_NUM*2];

// 初始化採樣查表 
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

// 將 OpenCV 影像轉換為硬體 Buffer
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

// 電腦模擬顯示函式
void simulate_pov_display(const struct POV_Frame& buffer, cv::Mat& display_canvas) {
    display_canvas = cv::Scalar(0, 0, 0); 
    int sim_w = display_canvas.cols;
    int sim_h = display_canvas.rows;
    int sim_center = sim_w / 2;
    float r_step_sim = 8.0; 
    int dot_size = MAX(1, (int)(r_step_sim * 0.3));

    for (int i = 0; i < NUM_SLICES; i++) {
        float angle_phys_A = i * 2.0 * M_PI / NUM_SLICES;
        float angle_phys_B = angle_phys_A + M_PI;

        for (int j = 0; j < LED_NUM; j++) {
            float d_A = r_step_sim * j + (r_step_sim*0.25);
            int draw_x_A = sim_center - d_A * sin(angle_phys_A);
            int draw_y_A = sim_center + d_A * cos(angle_phys_A);
            cv::Scalar color_A(buffer.data[i][j + LED_NUM][0], buffer.data[i][j + LED_NUM][1], buffer.data[i][j + LED_NUM][2]);
            cv::circle(display_canvas, cv::Point(draw_x_A, draw_y_A), dot_size, color_A, -1, cv::LINE_AA);

            float d_B = r_step_sim * j + (r_step_sim*0.75);
            int draw_x_B = sim_center - d_B * sin(angle_phys_B);
            int draw_y_B = sim_center + d_B * cos(angle_phys_B);
            cv::Scalar color_B(buffer.data[i][LED_NUM-j-1][0], buffer.data[i][LED_NUM-j-1][1], buffer.data[i][LED_NUM-j-1][2]);
            cv::circle(display_canvas, cv::Point(draw_x_B, draw_y_B), dot_size, color_B, -1, cv::LINE_AA);
        }
    }
    cv::circle(display_canvas, cv::Point(sim_center, sim_h - 10), 5, cv::Scalar(0, 0, 255), -1);
}

// ==============================================================================
// 主程式 (POV 遊戲引擎 - 終極整合版)
// ==============================================================================
int main(int argc, char** argv) {
    srand(time(NULL));
    int side_len = 800; 
    init_sampling_lut(side_len);
    POV_Frame pov_buffer; 

    cv::Mat game_canvas = cv::Mat::zeros(side_len, side_len, CV_8UC3);
    float r_step_sim = 8.0; 
    int sim_window_size = (int)(LED_NUM * r_step_sim * 2) + 40; 
    cv::Mat simulation_canvas = cv::Mat::zeros(sim_window_size, sim_window_size, CV_8UC3);

    cv::namedWindow("1. 2D Game Canvas", cv::WINDOW_AUTOSIZE);
    cv::namedWindow("2. POV Simulator", cv::WINDOW_AUTOSIZE);

    // --- 遊戲空間初始化 ---
    int center_x = side_len / 2;
    int center_y = side_len / 2;
    int r_step = center_x / LED_NUM; 
    int arena_radius = center_x - r_step; 

    // --- 球的初始化 ---
    float ball_x = center_x;
    float ball_y = center_y;
    float ball_vx = 7.0; 
    float ball_vy = 8.4; 
    int ball_radius = r_step * 0.8; 

    // --- 擋板初始化 ---
    float p1_angle = 180.0; // 左邊玩家
    float p2_angle = 0.0;   // 右邊玩家
    float paddle_size = 25.0; // 擋板涵蓋的角度 (正負 25 度)
    float paddle_speed = 8.0; // 擋板移動速度

    // ==========================================
    // ⭐️⭐️⭐️ [新增] 染色系統與計分板 ⭐️⭐️⭐️
    // ==========================================
    int p1_score = 0;
    int p2_score = 0;
    int ball_owner = 0; // 0: 中立, 1: P1, 2: P2
    // 發球停頓計時器 (設定 60 幀約等於 1 秒)
    int serve_delay_timer = 60;

    // OpenCV 是 BGR 格式
    cv::Scalar color_neutral(255, 255, 255); // 白色 (剛發球)
    cv::Scalar color_p1(255, 100, 50);       // 藍色 (P1)
    cv::Scalar color_p2(50, 50, 255);        // 紅色 (P2)
    cv::Scalar ball_color = color_neutral;

    // --- 輸入狀態機變數 ---
    int p1_up_pressed = 0, p1_down_pressed = 0;
    int p2_up_pressed = 0, p2_down_pressed = 0;

    printf("POV Pong 遊戲引擎啟動！\n");
    printf("P1(左) 控制: W/S | P2(右) 控制: I/K\n");

    while (1) {
        game_canvas = cv::Scalar(0, 0, 0);

        // ==========================================
        // B1. 輸入更新層 (Input Handling)
        // ==========================================
        // 16ms -> 62.5Hz
        int key = cv::waitKey(16); 
        if (key == 'q' || key == 27) break;
        
        // 每次迴圈先歸零狀態
        p1_up_pressed = 0; p1_down_pressed = 0;
        p2_up_pressed = 0; p2_down_pressed = 0;

        if (key == 'w') p1_up_pressed = 1;
        if (key == 's') p1_down_pressed = 1;
        if (key == 'i') p2_up_pressed = 1;
        if (key == 'k') p2_down_pressed = 1;

        // ==========================================
        // B2. 物理更新層 (Physics Update)
        // ==========================================
        // 1. 移動擋板
        if (p1_up_pressed)   p1_angle -= paddle_speed;
        if (p1_down_pressed) p1_angle += paddle_speed;
        if (p2_up_pressed)   p2_angle -= paddle_speed;
        if (p2_down_pressed) p2_angle += paddle_speed;

        p1_angle = fmod(p1_angle + 360.0, 360.0);
        p2_angle = fmod(p2_angle + 360.0, 360.0);

        // 移動球 (受發球延遲影響)
        if (serve_delay_timer > 0) {
            // 還在倒數中，球不動，只扣減計時器
            serve_delay_timer--;
        } else {
            // 計時器歸零了，球開始根據速度飛行
            ball_x += ball_vx;
            ball_y += ball_vy;
        }

        // 3. 碰撞與得分偵測
        float dx = ball_x - center_x;
        float dy = ball_y - center_y;
        float distance = sqrt(dx * dx + dy * dy);

        if (distance + ball_radius > arena_radius) {
            
            // ⭐️ 計算球的撞擊角度
            float ball_angle_rad = atan2(dy, dx); 
            float ball_angle_deg = ball_angle_rad * 180.0 / M_PI;
            if (ball_angle_deg < 0) ball_angle_deg += 360.0;

            // ⭐️ 計算角度差 (解決 0 與 360 的跨界問題)
            auto get_angle_diff = [](float a1, float a2) {
                float diff = fmod(fabs(a1 - a2), 360.0);
                return diff > 180.0 ? 360.0 - diff : diff;
            };

            float diff_p1 = get_angle_diff(ball_angle_deg, p1_angle);
            float diff_p2 = get_angle_diff(ball_angle_deg, p2_angle);
            
            // ==========================================
            // 🛠️ 優化 1：加大碰撞箱容錯率 (Hitbox Buffer)
            // ==========================================
            // 捨棄原本嚴苛的數學比例，直接固定增加 15 度的隱形判定區！
            // 這樣視覺上看起來是擦邊的球，也能完美接住，手感大幅提升。
            float hit_tolerance = paddle_size + 5.0;

            // ==========================================
            // 🛠️ 優化 2：自己的攻擊直接穿透 (Ghost Paddle)
            // ==========================================
            // 判定成功擋下的條件：
            // 1. 角度在容錯範圍內
            // 2. 並且這顆球「不是」自己的顏色
            bool hit_p1 = (diff_p1 <= hit_tolerance) && (ball_owner != 1);
            bool hit_p2 = (diff_p2 <= hit_tolerance) && (ball_owner != 2);

            // 判斷是否被板子擋下
            if (hit_p1 || hit_p2) {
                
                // 染色系統
                if (hit_p1) {
                    ball_owner = 1;
                    ball_color = color_p1;
                } else {
                    ball_owner = 2;
                    ball_color = color_p2;
                }

                // --- (以下物理反彈與加速邏輯完全不變) ---
                float overlap = (distance + ball_radius) - arena_radius;
                ball_x -= (dx / distance) * overlap;
                ball_y -= (dy / distance) * overlap;

                float nx = dx / distance;
                float ny = dy / distance;
                float dot_product = (ball_vx * nx) + (ball_vy * ny);
                ball_vx -= (2 * dot_product * nx);
                ball_vy -= (2 * dot_product * ny);

                // ==========================================
                // 新增切球與旋轉傳遞 (Paddle English)
                // ==========================================
                // 1. 計算「切線向量 (Tangent Vector)」：把法向量旋轉 90 度
                float tx = -ny; 
                float ty = nx;  

                // 2. 抓出當下這塊板子的移動速度與方向
                float current_paddle_v = 0.0;
                if (hit_p1) {
                    if (p1_up_pressed) current_paddle_v = -paddle_speed;
                    else if (p1_down_pressed) current_paddle_v = paddle_speed;
                } else {
                    if (p2_up_pressed) current_paddle_v = -paddle_speed;
                    else if (p2_down_pressed) current_paddle_v = paddle_speed;
                }

                // 3. 將板子的速度「摩擦」給球 (摩擦係數設為 0.4，你可以自由調整手感！)
                float friction_coeff = 0.4;
                ball_vx += (tx * current_paddle_v * friction_coeff);
                ball_vy += (ty * current_paddle_v * friction_coeff);
                // ==========================================

                ball_vx *= 1.03;
                ball_vy *= 1.03;

                float noise_angle = ((rand() % 100) / 100.0f - 0.5f) * 0.1f;
                float temp_vx = ball_vx * cos(noise_angle) - ball_vy * sin(noise_angle);
                float temp_vy = ball_vx * sin(noise_angle) + ball_vy * cos(noise_angle);
                ball_vx = temp_vx;
                ball_vy = temp_vy;

                float MAX_SPEED = r_step * 1.5;
                float current_speed = sqrt(ball_vx * ball_vx + ball_vy * ball_vy);
                if (current_speed > MAX_SPEED) {
                    ball_vx *= (MAX_SPEED / current_speed);
                    ball_vy *= (MAX_SPEED / current_speed);
                }
            } else {
                // --- (以下漏接與得分結算邏輯完全不變) ---
                if (ball_owner == 1) {
                    p1_score++;
                    printf(">> 玩家 1 (藍) 攻擊成功！ 得分！\n");
                } else if (ball_owner == 2) {
                    p2_score++;
                    printf(">> 玩家 2 (紅) 攻擊成功！ 得分！\n");
                } else {
                    printf(">> 無效球！發球後沒人碰到就飛出去了。\n");
                }
                printf("[目前比分] P1: %d | P2: %d\n\n", p1_score, p2_score);

                ball_x = center_x;
                ball_y = center_y;
                ball_owner = 0;
                ball_color = color_neutral;

                float serve_angle = (rand() % 360) * M_PI / 180.0;
                float serve_speed = 7.0;
                ball_vx = serve_speed * cos(serve_angle);
                ball_vy = serve_speed * sin(serve_angle);

                serve_delay_timer = 60;
            }
        }

        // ==========================================
        // C. 遊戲繪圖 (Render)
        // ==========================================
        cv::circle(game_canvas, cv::Point(center_x, center_y), arena_radius, cv::Scalar(30, 30, 30), 40);
        
        // 畫板子 (顏色對應玩家)
        cv::ellipse(game_canvas, cv::Point(center_x, center_y), cv::Size(arena_radius, arena_radius), 
                    0, p1_angle - paddle_size, p1_angle + paddle_size, color_p1, 40);

        cv::ellipse(game_canvas, cv::Point(center_x, center_y), cv::Size(arena_radius, arena_radius), 
                    0, p2_angle - paddle_size, p2_angle + paddle_size, color_p2, 40);

        // 畫球 (顏色對應擁有者)
        cv::circle(game_canvas, cv::Point((int)ball_x, (int)ball_y), ball_radius, ball_color, -1);

        // ==========================================
        // D. 轉換與顯示
        // ==========================================
        convert_to_pov_buffer(game_canvas, &pov_buffer);
        simulate_pov_display(pov_buffer, simulation_canvas);

        cv::imshow("1. 2D Game Canvas", game_canvas);
        cv::imshow("2. POV Simulator", simulation_canvas);
    }

    cv::destroyAllWindows();
    return 0;
}