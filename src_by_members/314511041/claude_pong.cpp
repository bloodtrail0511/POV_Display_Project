/*
 * app_pong_sim.cpp  (v2 - LUT 架構)
 * 環狀雙人桌球 (Cylindrical Pong) - OpenCV WSL 模擬器
 *
 * ============================================================
 * 資料流（與 pov_pc_sim_v2.cpp 完全相同的 LUT 架構）：
 *
 *   [遊戲邏輯]
 *       │  render_game_to_canvas()
 *       ▼
 *   cv::Mat game_canvas  (圓形畫布，極座標繪圖)
 *       │  convert_canvas_to_pov_buffer()  ← 查 LUT
 *       ▼
 *   POV_Buffer pov_buffer[NUM_SLICES][NUM_LEDS]  (RGB)
 *       │  simulate_pov_display()  ← 逆向顯示
 *       ▼
 *   cv::Mat display  →  imshow()
 *       │
 *       └─ (未來在 RPi) write(fd_pov, ...)  →  /dev/pov_display
 *
 * ============================================================
 * 座標系統（與 pov_display_driver_v3.c 一致）：
 *   slice  : 0 ~ NUM_SLICES-1，對應 360 個旋轉切片
 *   led    : 0 ~ NUM_LEDS-1，對應 20 顆 LED（0=內圈, NUM_LEDS-1=外圈）
 *
 * ============================================================
 * 按鍵：
 *   P1 (紅)  : A / D
 *   P2 (藍)  : J / L
 *   SPACE    : 暫停
 *   R        : 重置
 *   ESC / Q  : 離開
 */

#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include <opencv2/opencv.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#undef MAX
#undef MIN
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ============================================================
// 硬體常數（與 Kernel Driver 保持一致）
// ============================================================
#define NUM_LEDS          20    // 每根燈條 LED 數
#define NUM_SLICES        360   // 對應 DEGREE_RESOLUTION
#define CANVAS_SIZE       400   // 遊戲畫布（正方形邊長，像素）

// ============================================================
// 遊戲常數
// ============================================================
#define PADDLE_LEN_DEG    30    // Paddle 弧長（度）
#define PADDLE_LED        (NUM_LEDS - 2)   // Paddle 所在 LED 索引（外圈內側一格）
#define INNER_WALL_LED    1     // 內壁 LED 索引
#define FPS               60
#define TICK_MS           (1000 / FPS)
#define WIN_SCORE         5

// ============================================================
// POV Buffer（BGR，與 pov_pc_sim_v2 相同格式）
// ============================================================
typedef struct {
    uint8_t data[NUM_SLICES][NUM_LEDS][3];  // [slice][led][B, G, R]
} POV_Buffer;

static POV_Buffer pov_buffer;

// ============================================================
// LUT：(slice, led) → 畫布像素座標 (px, py)
// 與 pov_pc_sim_v2 的 init_sampling_lut 相同公式
// ============================================================
static int lut_px[NUM_SLICES][NUM_LEDS];   // 畫布 x
static int lut_py[NUM_SLICES][NUM_LEDS];   // 畫布 y

void init_lut() {
    int center   = CANVAS_SIZE / 2;
    float r_max  = (float)center - 5.0f;
    float r_step = r_max / NUM_LEDS;

    for (int s = 0; s < NUM_SLICES; s++) {
        // 0度在正上方，順時針（與 pov_pc_sim_v2 相同）
        float angle = (float)s * 2.0f * (float)M_PI / NUM_SLICES;

        for (int l = 0; l < NUM_LEDS; l++) {
            // led=0 最內圈，led=NUM_LEDS-1 最外圈
            float d = r_step * l + r_step * 0.5f;

            int px = (int)(center - d * sinf(angle));
            int py = (int)(center + d * cosf(angle));

            lut_px[s][l] = MAX(0, MIN(px, CANVAS_SIZE - 1));
            lut_py[s][l] = MAX(0, MIN(py, CANVAS_SIZE - 1));
        }
    }

    printf("[LUT] init done. CANVAS_SIZE=%d, r_step=%.2f px/led\n",
           CANVAS_SIZE, r_step);
}

// ============================================================
// 查 LUT：畫布 → pov_buffer（模擬硬體採樣）
// 與 pov_pc_sim_v2 的 convert_to_pov_buffer 相同
// ============================================================
void convert_canvas_to_pov_buffer(const cv::Mat& canvas) {
    const uint8_t* raw = canvas.data;
    int step     = (int)canvas.step;
    int channels = canvas.channels();  // 3 (BGR)

    for (int s = 0; s < NUM_SLICES; s++) {
        for (int l = 0; l < NUM_LEDS; l++) {
            int offset = lut_py[s][l] * step + lut_px[s][l] * channels;
            pov_buffer.data[s][l][0] = raw[offset + 0]; // B
            pov_buffer.data[s][l][1] = raw[offset + 1]; // G
            pov_buffer.data[s][l][2] = raw[offset + 2]; // R
        }
    }
}

// ============================================================
// 從 pov_buffer 逆向畫回顯示視窗（simulate_pov_display）
// 與 pov_pc_sim_v2 完全相同的邏輯
// ============================================================
void simulate_pov_display(cv::Mat& display) {
    display = cv::Scalar(8, 8, 12);  // 深色背景

    int   center   = display.cols / 2;
    float r_step   = 8.0f;  // 顯示視窗每顆 LED 固定 8px（與 pov_pc_sim_v2 相同）
    int   dot_size = MAX(1, (int)(r_step * 0.38f));

    for (int s = 0; s < NUM_SLICES; s++) {
        float angle = (float)s * 2.0f * (float)M_PI / NUM_SLICES;

        for (int l = 0; l < NUM_LEDS; l++) {
            uint8_t b = pov_buffer.data[s][l][0];
            uint8_t g = pov_buffer.data[s][l][1];
            uint8_t r = pov_buffer.data[s][l][2];

            if (b == 0 && g == 0 && r == 0) continue;  // 跳過全黑

            float d    = r_step * l + r_step * 0.5f;
            int draw_x = (int)(center - d * sinf(angle));
            int draw_y = (int)(center + d * cosf(angle));

            cv::circle(display, {draw_x, draw_y}, dot_size,
                       cv::Scalar(b, g, r), -1, cv::LINE_AA);
        }
    }
}

// ============================================================
// 遊戲邏輯
// ============================================================

float norm_angle(float a) {
    while (a <  0)          a += NUM_SLICES;
    while (a >= NUM_SLICES) a -= NUM_SLICES;
    return a;
}

float angle_diff(float from, float to) {
    float d = to - from;
    while (d >  NUM_SLICES / 2.0f) d -= NUM_SLICES;
    while (d < -NUM_SLICES / 2.0f) d += NUM_SLICES;
    return d;
}

struct Ball {
    float angle;
    float radius;
    float vangle;
    float vradius;
};

struct Paddle {
    float center;
    float speed;
    int   score;
    bool  moving_left;
    bool  moving_right;
};

struct GameState {
    Ball   ball;
    Paddle p1;
    Paddle p2;
    bool   game_over;
    int    win_player;
    bool   paused;
};

GameState game_init() {
    GameState gs;
    memset(&gs, 0, sizeof(gs));

    gs.ball.angle   = NUM_SLICES / 4.0f;
    gs.ball.radius  = NUM_LEDS / 2.0f;
    gs.ball.vangle  = 2.5f;
    gs.ball.vradius = 0.3f;

    gs.p1.center = 0.0f;
    gs.p1.speed  = 3.5f;
    gs.p2.center = NUM_SLICES / 2.0f;
    gs.p2.speed  = 3.5f;

    return gs;
}

void reset_ball(Ball& b, float start_angle) {
    b.angle   = norm_angle(start_angle);
    b.radius  = NUM_LEDS / 2.0f;
    b.vangle  = 2.5f * ((rand() % 2 == 0) ? 1.0f : -1.0f);
    b.vradius = 0.3f;
}

bool in_paddle_range(float ball_angle, float paddle_center, float half_len) {
    return fabsf(angle_diff(paddle_center, ball_angle)) <= half_len;
}

void game_update(GameState& gs) {
    if (gs.game_over || gs.paused) return;

    Ball&   b  = gs.ball;
    Paddle& p1 = gs.p1;
    Paddle& p2 = gs.p2;

    if (p1.moving_left)  p1.center = norm_angle(p1.center - p1.speed);
    if (p1.moving_right) p1.center = norm_angle(p1.center + p1.speed);
    if (p2.moving_left)  p2.center = norm_angle(p2.center - p2.speed);
    if (p2.moving_right) p2.center = norm_angle(p2.center + p2.speed);

    b.angle  = norm_angle(b.angle + b.vangle);
    b.radius += b.vradius;

    float half_paddle = PADDLE_LEN_DEG / 2.0f;

    // 外圈碰撞
    if (b.radius >= PADDLE_LED) {
        if (in_paddle_range(b.angle, p1.center, half_paddle) ||
            in_paddle_range(b.angle, p2.center, half_paddle)) {
            b.radius  = (float)PADDLE_LED - 0.1f;
            b.vradius = -fabsf(b.vradius);
            b.vangle += ((float)(rand() % 3) - 1.0f) * 0.3f;
        } else {
            float d1 = fabsf(angle_diff(p1.center, b.angle));
            float d2 = fabsf(angle_diff(p2.center, b.angle));
            if (d1 < d2) {
                p2.score++;
                printf("P2 scores! P1:%d P2:%d\n", p1.score, p2.score);
            } else {
                p1.score++;
                printf("P1 scores! P1:%d P2:%d\n", p1.score, p2.score);
            }

            if (p1.score >= WIN_SCORE || p2.score >= WIN_SCORE) {
                gs.game_over  = true;
                gs.win_player = (p1.score >= WIN_SCORE) ? 1 : 2;
            } else {
                reset_ball(b, b.angle + NUM_SLICES / 2.0f);
            }
        }
    }

    // 內壁碰撞
    if (b.radius <= INNER_WALL_LED) {
        b.radius  = (float)INNER_WALL_LED + 0.1f;
        b.vradius =  fabsf(b.vradius);
        b.vangle  = -b.vangle;
    }

    // 逐漸加速
    if (fabsf(b.vangle) < 7.0f) b.vangle  *= 1.0008f;
    if (b.vradius       < 0.8f) b.vradius *= 1.0008f;
}

// ============================================================
// 把遊戲狀態畫到 game_canvas（OpenCV Mat，BGR，極座標）
// ============================================================
void render_game_to_canvas(const GameState& gs, cv::Mat& canvas) {
    canvas = cv::Scalar(0, 0, 0);

    int   center = CANVAS_SIZE / 2;
    float r_max  = (float)center - 5.0f;
    float r_step = r_max / NUM_LEDS;

    // (slice, led) → 畫布像素
    auto to_pt = [&](float s, float l) -> cv::Point {
        float angle = s * 2.0f * (float)M_PI / NUM_SLICES;
        float d     = r_step * l + r_step * 0.5f;
        int px = (int)(center - d * sinf(angle));
        int py = (int)(center + d * cosf(angle));
        return {MAX(0, MIN(px, CANVAS_SIZE-1)),
                MAX(0, MIN(py, CANVAS_SIZE-1))};
    };

    int dot_r = MAX(2, (int)(r_step * 0.48f));

    // 內壁（暗灰環）
    cv::circle(canvas, {center, center},
               (int)(r_step * INNER_WALL_LED + r_step * 0.5f),
               cv::Scalar(40, 40, 40), 1, cv::LINE_AA);

    // 外壁邊界（暗綠環）
    cv::circle(canvas, {center, center},
               (int)(r_step * PADDLE_LED + r_step * 0.5f),
               cv::Scalar(0, 60, 0), 1, cv::LINE_AA);

    float half = PADDLE_LEN_DEG / 2.0f;

    // Player 1 Paddle（紅，BGR: 50,50,255）
    for (int d = -(int)half; d <= (int)half; d++) {
        float s = norm_angle(gs.p1.center + d);
        cv::circle(canvas, to_pt(s, PADDLE_LED),   dot_r,
                   cv::Scalar(50, 50, 255), -1, cv::LINE_AA);
        cv::circle(canvas, to_pt(s, PADDLE_LED-1), MAX(1, dot_r-1),
                   cv::Scalar(25, 25, 120), -1, cv::LINE_AA);
    }

    // Player 2 Paddle（藍，BGR: 255,100,50）
    for (int d = -(int)half; d <= (int)half; d++) {
        float s = norm_angle(gs.p2.center + d);
        cv::circle(canvas, to_pt(s, PADDLE_LED),   dot_r,
                   cv::Scalar(255, 100, 50), -1, cv::LINE_AA);
        cv::circle(canvas, to_pt(s, PADDLE_LED-1), MAX(1, dot_r-1),
                   cv::Scalar(120, 50, 25), -1, cv::LINE_AA);
    }

    // 球拖尾（黃橙）
    for (int t = 3; t >= 1; t--) {
        float ts = norm_angle(gs.ball.angle - gs.ball.vangle * t * 1.5f);
        int alpha = 60 - t * 15;
        cv::circle(canvas, to_pt(ts, gs.ball.radius),
                   MAX(1, dot_r - 1),
                   cv::Scalar(0, alpha, alpha*2), -1, cv::LINE_AA);
    }

    // 球本體（亮黃，BGR: 0,220,255）
    cv::circle(canvas, to_pt(gs.ball.angle, gs.ball.radius), dot_r,
               cv::Scalar(0, 220, 255), -1, cv::LINE_AA);
}

// ============================================================
// HUD（疊加在 display 上，不影響 pov_buffer）
// ============================================================
void draw_hud(cv::Mat& display, const GameState& gs) {
    char buf[128];
    snprintf(buf, sizeof(buf), "P1 Red:%d   P2 Blue:%d", gs.p1.score, gs.p2.score);
    cv::putText(display, buf, {12, 24},
                cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(220,220,220), 1, cv::LINE_AA);

    cv::putText(display, "P1:A/D  P2:J/L  SPACE:Pause  R:Reset  ESC:Quit",
                {8, display.rows - 8},
                cv::FONT_HERSHEY_SIMPLEX, 0.4, cv::Scalar(120,120,120), 1, cv::LINE_AA);

    if (gs.paused && !gs.game_over) {
        cv::putText(display, "-- PAUSED --",
                    {display.cols/2 - 75, display.rows/2},
                    cv::FONT_HERSHEY_SIMPLEX, 0.85, cv::Scalar(0,255,255), 2, cv::LINE_AA);
    }

    if (gs.game_over) {
        snprintf(buf, sizeof(buf), "Player %d  WINS!", gs.win_player);
        cv::Scalar c = (gs.win_player == 1) ? cv::Scalar(80,80,255) : cv::Scalar(255,140,50);
        cv::putText(display, buf,
                    {display.cols/2 - 105, display.rows/2},
                    cv::FONT_HERSHEY_SIMPLEX, 1.05, c, 2, cv::LINE_AA);
        cv::putText(display, "Press R to restart",
                    {display.cols/2 - 90, display.rows/2 + 42},
                    cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(200,200,200), 1, cv::LINE_AA);
    }
}

// ============================================================
// Main
// ============================================================
int main() {
    srand((unsigned)time(NULL));

    // 1. 建立 LUT
    init_lut();

    // 2. 準備畫布 & 顯示視窗
    cv::Mat game_canvas = cv::Mat::zeros(CANVAS_SIZE, CANVAS_SIZE, CV_8UC3);

    float r_step_sim = 8.0f;
    int display_size = (int)(NUM_LEDS * r_step_sim * 2) + 60;
    cv::Mat display  = cv::Mat::zeros(display_size, display_size, CV_8UC3);

    cv::namedWindow("POV Pong Simulator", cv::WINDOW_AUTOSIZE);

    // 3. 初始化遊戲
    GameState gs = game_init();

    printf("=== POV Cylindrical Pong Simulator (LUT version) ===\n");
    printf("P1 Red  : A=Left  D=Right\n");
    printf("P2 Blue : J=Left  L=Right\n");
    printf("SPACE=Pause  R=Reset  ESC/Q=Quit\n\n");

    // 4. 主迴圈
    while (true) {
        gs.p1.moving_left  = false;
        gs.p1.moving_right = false;
        gs.p2.moving_left  = false;
        gs.p2.moving_right = false;

        int key = cv::waitKey(TICK_MS) & 0xFF;

        if (key == 27 || key == 'q' || key == 'Q') break;
        if (key == 'a' || key == 'A') gs.p1.moving_left  = true;
        if (key == 'd' || key == 'D') gs.p1.moving_right = true;
        if (key == 'j' || key == 'J') gs.p2.moving_left  = true;
        if (key == 'l' || key == 'L') gs.p2.moving_right = true;
        if (key == ' ')               gs.paused = !gs.paused;
        if (key == 'r' || key == 'R') { gs = game_init(); printf("Reset!\n"); }

        // 遊戲更新
        game_update(gs);

        // 遊戲 → 畫布
        render_game_to_canvas(gs, game_canvas);

        // 畫布 → pov_buffer（查 LUT，模擬硬體採樣）
        convert_canvas_to_pov_buffer(game_canvas);

        // pov_buffer → display（POV 視覺模擬）
        simulate_pov_display(display);

        // HUD 疊加
        draw_hud(display, gs);

        cv::imshow("POV Pong Simulator", display);
    }

    cv::destroyAllWindows();
    printf("Bye!\n");
    return 0;
}