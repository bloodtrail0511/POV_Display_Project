#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

// OpenCV 核心與顯示模組
#include <opencv2/opencv.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#define NUM_SLICES 360
#define LED_NUM 20

#include "net_server.hpp"

/*
 * app_pong_multi.cpp 負責三件事：
 * 1. 跑 Pong 遊戲邏輯與 OpenCV 畫面。
 * 2. 從 net_server 讀取外部玩家 P3 / P4 的輸入狀態。
 * 3. 把 2D 畫面重新採樣成 POV LED 旋轉顯示用的 buffer。
 */

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

// 採樣用的查表 (Sampling LUT)：
// lut_x/lut_y[角度 slice][LED 位置] -> 2D game_canvas 上要取樣的 pixel 座標。
int lut_x[NUM_SLICES][LED_NUM*2];
int lut_y[NUM_SLICES][LED_NUM*2];

// 初始化採樣查表。
// POV 硬體有兩條相對的 LED strip，所以同一個 slice 會同時取 Strip A / Strip B 的點。
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

// 將 OpenCV 影像轉換為硬體 Buffer。
// game_canvas 是一般 2D 圖，這裡依照 LUT 抽樣成 NUM_SLICES x LED 數量的資料。
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

// 電腦模擬顯示函式。
// 實際硬體會旋轉 LED；這裡用小視窗把每個 slice / LED 點畫出來方便除錯。
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
            cv::Scalar color_A(
                buffer.data[i][j + LED_NUM][0],
                buffer.data[i][j + LED_NUM][1],
                buffer.data[i][j + LED_NUM][2]
            );
            cv::circle(display_canvas, cv::Point(draw_x_A, draw_y_A), dot_size, color_A, -1, cv::LINE_AA);

            float d_B = r_step_sim * j + (r_step_sim*0.75);
            int draw_x_B = sim_center - d_B * sin(angle_phys_B);
            int draw_y_B = sim_center + d_B * cos(angle_phys_B);
            cv::Scalar color_B(
                buffer.data[i][LED_NUM-j-1][0],
                buffer.data[i][LED_NUM-j-1][1],
                buffer.data[i][LED_NUM-j-1][2]
            );
            cv::circle(display_canvas, cv::Point(draw_x_B, draw_y_B), dot_size, color_B, -1, cv::LINE_AA);
        }
    }

    cv::circle(display_canvas, cv::Point(sim_center, sim_h - 10), 5, cv::Scalar(0, 0, 255), -1);
}

// ==============================================================================
// 主程式
// ==============================================================================
int main(int argc, char** argv) {
    srand(time(NULL));

    // 啟動 TCP server。server 會在背景 thread 接收 pong_client 的 UP / DOWN / QUIT。
    start_network_server();

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
    float p3_angle = 90.0;  // 外部連線玩家 P3
    float p4_angle = 270.0; // 外部連線玩家 P4
    float paddle_size = 25.0;
    float paddle_speed = 8.0;

    // --- 分數與球權 ---
    int p1_score = 0;
    int p2_score = 0;
    int p3_score = 0;
    int p4_score = 0;
    int ball_owner = 0; // 0: 中立, 1: P1, 2: P2, 3: P3, 4: P4
    int serve_delay_timer = 60;

    // OpenCV 是 BGR 格式
    cv::Scalar color_neutral(255, 255, 255);
    cv::Scalar color_p1(255, 100, 50);
    cv::Scalar color_p2(50, 50, 255);
    cv::Scalar color_p3(50, 255, 50);
    cv::Scalar color_p4(0, 255, 255);
    cv::Scalar ball_color = color_neutral;

    // --- 輸入狀態 ---
    int p1_up_pressed = 0, p1_down_pressed = 0;
    int p2_up_pressed = 0, p2_down_pressed = 0;
    int p3_up_pressed = 0, p3_down_pressed = 0;
    int p4_up_pressed = 0, p4_down_pressed = 0;
    int p3_connected = 0;
    int p4_connected = 0;

    printf("POV Pong 遊戲引擎啟動！\n");
    printf("P1(左) 控制: W/S | P2(右) 控制: I/K | P3/P4(網路) 控制: 外部 client 上下鍵\n");
    printf("按 Q 離開遊戲\n");
    printf("外部玩家連線方式: ./pong_client <game_host_ip>\n");

    while (1) {
        game_canvas = cv::Scalar(0, 0, 0);

        // ==========================================
        // B1. 輸入更新層
        // ==========================================
        int key = cv::waitKey(16);
        // OpenCV waitKey() 在不同平台可能回傳超過 8 bit，遮罩後比較 ASCII 比較穩。
        int key_code = key & 0xff;

        if (key_code == 'q' || key_code == 'Q' || key_code == 27) {
            break;
        }
        
        p1_up_pressed = 0;
        p1_down_pressed = 0;
        p2_up_pressed = 0;
        p2_down_pressed = 0;

        if (key_code == 'w' || key_code == 'W') {
            p1_up_pressed = 1;
        }

        if (key_code == 's' || key_code == 'S') {
            p1_down_pressed = 1;
        }

        if (key_code == 'i' || key_code == 'I') {
            p2_up_pressed = 1;
        }

        if (key_code == 'k' || key_code == 'K') {
            p2_down_pressed = 1;
        }

        // P3 / P4 由外部連線玩家控制。
        // exchange(false) 代表「讀取這次按鍵後立刻清掉」，所以 client 按一次只移動一格。
        p3_connected = net_players[0].connected.load();
        p3_up_pressed = net_players[0].up_pressed.exchange(false);
        p3_down_pressed = net_players[0].down_pressed.exchange(false);
        p4_connected = net_players[1].connected.load();
        p4_up_pressed = net_players[1].up_pressed.exchange(false);
        p4_down_pressed = net_players[1].down_pressed.exchange(false);

        // ==========================================
        // B2. 物理更新層
        // ==========================================
        if (p1_up_pressed) {
            p1_angle -= paddle_speed;
        }

        if (p1_down_pressed) {
            p1_angle += paddle_speed;
        }

        if (p2_up_pressed) {
            p2_angle -= paddle_speed;
        }

        if (p2_down_pressed) {
            p2_angle += paddle_speed;
        }

        if (p3_connected && p3_up_pressed) {
            p3_angle -= paddle_speed;
        }

        if (p3_connected && p3_down_pressed) {
            p3_angle += paddle_speed;
        }

        if (p4_connected && p4_up_pressed) {
            p4_angle -= paddle_speed;
        }

        if (p4_connected && p4_down_pressed) {
            p4_angle += paddle_speed;
        }

        // 角度限制在 0~360 度，避免一直加減後數值變很大。
        p1_angle = fmod(p1_angle + 360.0, 360.0);
        p2_angle = fmod(p2_angle + 360.0, 360.0);
        p3_angle = fmod(p3_angle + 360.0, 360.0);
        p4_angle = fmod(p4_angle + 360.0, 360.0);

        if (serve_delay_timer > 0) {
            serve_delay_timer--;
        } else {
            ball_x += ball_vx;
            ball_y += ball_vy;
        }

        // ==========================================
        // B3. 碰撞與得分偵測
        // ==========================================
        float dx = ball_x - center_x;
        float dy = ball_y - center_y;
        float distance = sqrt(dx * dx + dy * dy);

        if (distance + ball_radius > arena_radius) {
            float ball_angle_rad = atan2(dy, dx); 
            float ball_angle_deg = ball_angle_rad * 180.0 / M_PI;

            if (ball_angle_deg < 0) {
                ball_angle_deg += 360.0;
            }

            // 比較球飛出邊界的角度與各玩家擋板角度，差距夠小就算擊中。
            auto get_angle_diff = [](float a1, float a2) {
                float diff = fmod(fabs(a1 - a2), 360.0);
                return diff > 180.0 ? 360.0 - diff : diff;
            };

            float diff_p1 = get_angle_diff(ball_angle_deg, p1_angle);
            float diff_p2 = get_angle_diff(ball_angle_deg, p2_angle);
            float diff_p3 = get_angle_diff(ball_angle_deg, p3_angle);
            float diff_p4 = get_angle_diff(ball_angle_deg, p4_angle);
            
            float hit_tolerance = paddle_size + 5.0;

            bool hit_p1 = (diff_p1 <= hit_tolerance) && (ball_owner != 1);
            bool hit_p2 = (diff_p2 <= hit_tolerance) && (ball_owner != 2);
            bool hit_p3 = p3_connected && (diff_p3 <= hit_tolerance) && (ball_owner != 3);
            bool hit_p4 = p4_connected && (diff_p4 <= hit_tolerance) && (ball_owner != 4);

            if (hit_p1 || hit_p2 || hit_p3 || hit_p4) {
                if (hit_p1) {
                    ball_owner = 1;
                    ball_color = color_p1;
                } else if (hit_p2) {
                    ball_owner = 2;
                    ball_color = color_p2;
                } else if (hit_p3) {
                    ball_owner = 3;
                    ball_color = color_p3;
                } else if (hit_p4) {
                    ball_owner = 4;
                    ball_color = color_p4;
                }

                // 球已經超出邊界時，先把球推回 arena 裡，避免下一幀重複判定。
                float overlap = (distance + ball_radius) - arena_radius;
                ball_x -= (dx / distance) * overlap;
                ball_y -= (dy / distance) * overlap;

                // 用圓邊界的法向量反射速度，形成撞牆/撞擋板反彈。
                float nx = dx / distance;
                float ny = dy / distance;
                float dot_product = (ball_vx * nx) + (ball_vy * ny);

                ball_vx -= (2 * dot_product * nx);
                ball_vy -= (2 * dot_product * ny);

                float tx = -ny; 
                float ty = nx;  

                float current_paddle_v = 0.0;

                if (hit_p1) {
                    if (p1_up_pressed) {
                        current_paddle_v = -paddle_speed;
                    } else if (p1_down_pressed) {
                        current_paddle_v = paddle_speed;
                    }
                } else if (hit_p2) {
                    if (p2_up_pressed) {
                        current_paddle_v = -paddle_speed;
                    } else if (p2_down_pressed) {
                        current_paddle_v = paddle_speed;
                    }
                } else if (hit_p3) {
                    if (p3_up_pressed) {
                        current_paddle_v = -paddle_speed;
                    } else if (p3_down_pressed) {
                        current_paddle_v = paddle_speed;
                    }
                } else if (hit_p4) {
                    if (p4_up_pressed) {
                        current_paddle_v = -paddle_speed;
                    } else if (p4_down_pressed) {
                        current_paddle_v = paddle_speed;
                    }
                }

                // 擋板移動方向會額外影響球速，讓擊球有一點旋轉/切球感。
                float friction_coeff = 0.4;
                ball_vx += (tx * current_paddle_v * friction_coeff);
                ball_vy += (ty * current_paddle_v * friction_coeff);

                ball_vx *= 1.03;
                ball_vy *= 1.03;

                float noise_angle = ((rand() % 100) / 100.0f - 0.5f) * 0.1f;
                float temp_vx = ball_vx * cos(noise_angle) - ball_vy * sin(noise_angle);
                float temp_vy = ball_vx * sin(noise_angle) + ball_vy * cos(noise_angle);

                ball_vx = temp_vx;
                ball_vy = temp_vy;

                // 限制最高速度，避免球越打越快到無法遊玩。
                float MAX_SPEED = r_step * 1.5;
                float current_speed = sqrt(ball_vx * ball_vx + ball_vy * ball_vy);

                if (current_speed > MAX_SPEED) {
                    ball_vx *= (MAX_SPEED / current_speed);
                    ball_vy *= (MAX_SPEED / current_speed);
                }
            } else {
                if (ball_owner == 1) {
                    p1_score++;
                    printf(">> 玩家 1 (藍) 攻擊成功！ 得分！\n");
                } else if (ball_owner == 2) {
                    p2_score++;
                    printf(">> 玩家 2 (紅) 攻擊成功！ 得分！\n");
                } else if (ball_owner == 3) {
                    p3_score++;
                    printf(">> 玩家 3 (綠 / 網路) 攻擊成功！ 得分！\n");
                } else if (ball_owner == 4) {
                    p4_score++;
                    printf(">> 玩家 4 (黃 / 網路) 攻擊成功！ 得分！\n");
                } else {
                    printf(">> 無效球！發球後沒人碰到就飛出去了。\n");
                }

                printf("[目前比分] P1: %d | P2: %d | P3: %d | P4: %d\n\n", p1_score, p2_score, p3_score, p4_score);

                // 沒有玩家接到球，記分後回到中心重新發球。
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
        // C. 遊戲繪圖
        // ==========================================
        cv::circle(game_canvas, cv::Point(center_x, center_y), arena_radius, cv::Scalar(30, 30, 30), 40);
        
        cv::ellipse(
            game_canvas,
            cv::Point(center_x, center_y),
            cv::Size(arena_radius, arena_radius), 
            0,
            p1_angle - paddle_size,
            p1_angle + paddle_size,
            color_p1,
            40
        );

        cv::ellipse(
            game_canvas,
            cv::Point(center_x, center_y),
            cv::Size(arena_radius, arena_radius), 
            0,
            p2_angle - paddle_size,
            p2_angle + paddle_size,
            color_p2,
            40
        );

        if (p3_connected) {
            cv::ellipse(
                game_canvas,
                cv::Point(center_x, center_y),
                cv::Size(arena_radius, arena_radius),
                0,
                p3_angle - paddle_size,
                p3_angle + paddle_size,
                color_p3,
                40
            );
        }

        if (p4_connected) {
            cv::ellipse(
                game_canvas,
                cv::Point(center_x, center_y),
                cv::Size(arena_radius, arena_radius),
                0,
                p4_angle - paddle_size,
                p4_angle + paddle_size,
                color_p4,
                40
            );
        }

        cv::circle(game_canvas, cv::Point((int)ball_x, (int)ball_y), ball_radius, ball_color, -1);

        // ==========================================
        // D. 轉換與顯示
        // ==========================================
        // 顯示流程：2D 遊戲畫面 -> POV buffer -> POV 模擬視窗。
        convert_to_pov_buffer(game_canvas, &pov_buffer);
        simulate_pov_display(pov_buffer, simulation_canvas);

        cv::imshow("1. 2D Game Canvas", game_canvas);
        cv::imshow("2. POV Simulator", simulation_canvas);
    }

    stop_network_server();
    cv::destroyAllWindows();

    return 0;
}
