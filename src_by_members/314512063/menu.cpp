#include <stdint.h>
#include <stdio.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cmath>
#include <string>

#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/opencv.hpp>

#include "app_clock_v1.hpp"
#include "app_pong_sim.hpp"
#include "app_vid.hpp"

// menu.cpp 是整個 POV 模擬程式的入口選單。
// 功能流程：
// 1. 先用 OpenCV 畫出選單畫面。
// 2. 把選單畫面轉成 POV_Frame，模擬旋轉 LED 顯示效果。
// 3. 讀取鍵盤輸入，左右切換選項，上鍵確認，下鍵返回主選單。
// 4. 選到 CLK/VID/GAME 時，透過對應的 .hpp 入口函式啟動外部功能程式。
//    功能程式結束後會重新建立 menu 視窗，讓使用者回到主選單。

#define NUM_SLICES 360
#define LED_NUM 20

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct POV_Frame {
    uint8_t data[NUM_SLICES][LED_NUM * 2][3];
} POV_Frame;

// lut_x/lut_y 是 POV 取樣查表。
// 每個角度切片、每顆 LED 要從原始 2D 畫布的哪個像素取樣，都先算好存在這裡。
int lut_x[NUM_SLICES][LED_NUM * 2];
int lut_y[NUM_SLICES][LED_NUM * 2];

// 選單目前的畫面狀態。
// MainMenu：一般選單畫面。
// ClockSelected / VideoSelected / GameSelected：保留給需要先顯示「已選取」提示的流程。
// 目前確認選項後會直接呼叫 app_clock_v1.hpp / app_vid.hpp / app_pong_sim.hpp 的功能入口。
enum class MenuState {
    MainMenu,
    ClockSelected,
    VideoSelected,
    GameSelected
};

struct MenuContext {
    MenuState state = MenuState::MainMenu; // 畫面目前顯示的狀態
    int selected_index = 0;                // 目前游標指向的選項 index，0=CLK、1=VID、2=GAME
    int blink_tick = 0;                    // 控制選取項目閃爍的計數器

    // 選單文字標籤。
    // 若未來要新增 menu 功能，除了擴充陣列大小，也要同步修改 label_positions、
    // move_selection() 的選項數量、confirm_selection() 的對應動作，以及新增功能的 .hpp。
    std::array<std::string, 3> labels = {"CLK", "VID", "GAME"};
};

// 建立 POV 取樣查表。
// menu、clock、video、pong 都使用同樣的取樣概念：先畫完整 2D 圖，再轉成旋轉 LED 的資料格式。
void init_sampling_lut(int side_len) {
    int center = side_len / 2;
    float r_step = (float)center / LED_NUM;

    for (int i = 0; i < NUM_SLICES; i++) {
        float angle_A = (float)(i * 2.0 * M_PI / NUM_SLICES);
        float angle_B = angle_A + (float)M_PI;

        for (int j = 0; j < LED_NUM; j++) {
            float d_A = r_step * j + (r_step * 0.25f);
            int x_A = (int)(center - d_A * sin(angle_A));
            int y_A = (int)(center + d_A * cos(angle_A));

            float d_B = r_step * j + (r_step * 0.75f);
            int x_B = (int)(center - d_B * sin(angle_B));
            int y_B = (int)(center + d_B * cos(angle_B));

            lut_x[i][LED_NUM - j - 1] = std::max(0, std::min(x_B, side_len - 1));
            lut_y[i][LED_NUM - j - 1] = std::max(0, std::min(y_B, side_len - 1));
            lut_x[i][j + LED_NUM] = std::max(0, std::min(x_A, side_len - 1));
            lut_y[i][j + LED_NUM] = std::max(0, std::min(y_A, side_len - 1));
        }
    }
}

// 將 OpenCV Mat 畫布轉成 POV_Frame。
// 後續 simulate_pov_display() 會再把這份資料畫成 POV 模擬畫面。
void convert_to_pov_buffer(const cv::Mat& frame_cropped, POV_Frame* buffer) {
    const uint8_t* raw_pixels = frame_cropped.data;
    int step = (int)frame_cropped.step;
    int channels = frame_cropped.channels();

    for (int i = 0; i < NUM_SLICES; i++) {
        for (int j = 0; j < LED_NUM * 2; j++) {
            int offset = lut_y[i][j] * step + lut_x[i][j] * channels;
            buffer->data[i][j][0] = raw_pixels[offset + 0];
            buffer->data[i][j][1] = raw_pixels[offset + 1];
            buffer->data[i][j][2] = raw_pixels[offset + 2];
        }
    }
}

// 使用 POV_Frame 重建視覺效果，模擬真實旋轉 LED 看到的結果。
void simulate_pov_display(const POV_Frame& buffer, cv::Mat& display_canvas) {
    display_canvas = cv::Scalar(0, 0, 0);

    int sim_w = display_canvas.cols;
    int sim_h = display_canvas.rows;
    int sim_center = sim_w / 2;

    float r_step_sim = 11.0f;
    int dot_size = std::max(1, (int)(r_step_sim * 0.2f));

    for (int i = 0; i < NUM_SLICES; i++) {
        float angle_phys_A = (float)(i * 2.0 * M_PI / NUM_SLICES);
        float angle_phys_B = angle_phys_A + (float)M_PI;

        for (int j = 0; j < LED_NUM; j++) {
            float d_A = r_step_sim * j + (r_step_sim * 0.25f);
            int draw_x_A = (int)(sim_center - d_A * sin(angle_phys_A));
            int draw_y_A = (int)(sim_center + d_A * cos(angle_phys_A));

            cv::Scalar color_A(
                buffer.data[i][j + LED_NUM][0],
                buffer.data[i][j + LED_NUM][1],
                buffer.data[i][j + LED_NUM][2]
            );
            cv::circle(display_canvas, cv::Point(draw_x_A, draw_y_A), dot_size, color_A, -1, cv::LINE_8);

            float d_B = r_step_sim * j + (r_step_sim * 0.75f);
            int draw_x_B = (int)(sim_center - d_B * sin(angle_phys_B));
            int draw_y_B = (int)(sim_center + d_B * cos(angle_phys_B));

            cv::Scalar color_B(
                buffer.data[i][LED_NUM - j - 1][0],
                buffer.data[i][LED_NUM - j - 1][1],
                buffer.data[i][LED_NUM - j - 1][2]
            );
            cv::circle(display_canvas, cv::Point(draw_x_B, draw_y_B), dot_size, color_B, -1, cv::LINE_8);
        }
    }

    cv::circle(display_canvas, cv::Point(sim_center, sim_h - 10), 5, cv::Scalar(0, 0, 255), -1);
}

// 用 OpenCV 內建字型畫置中文字。
// 目前主要保留作為一般文字工具，選單大字則使用下方的 5x7 方塊字。
void draw_centered_text(
    cv::Mat& canvas,
    const std::string& text,
    cv::Point center,
    int font_face,
    double font_scale,
    int thickness,
    cv::Scalar color
) {
    int baseline = 0;
    cv::Size text_size = cv::getTextSize(text, font_face, font_scale, thickness, &baseline);
    cv::Point origin(center.x - text_size.width / 2, center.y + text_size.height / 2);
    cv::putText(canvas, text, origin, font_face, font_scale, color, thickness, cv::LINE_AA);
}

// 5x7 方塊字型表。
// 選單標籤只支援這裡列出的英文字母；新增新標籤時若出現新字母，要在這裡補 pattern。
std::array<std::string, 7> glyph_pattern(char ch) {
    switch (ch) {
    case 'A':
        return {"01110", "10001", "10001", "11111", "10001", "10001", "10001"};
    case 'C':
        return {"01111", "10000", "10000", "10000", "10000", "10000", "01111"};
    case 'D':
        return {"11110", "10001", "10001", "10001", "10001", "10001", "11110"};
    case 'E':
        return {"11111", "10000", "10000", "11110", "10000", "10000", "11111"};
    case 'G':
        return {"01111", "10000", "10000", "10111", "10001", "10001", "01111"};
    case 'I':
        return {"11111", "00100", "00100", "00100", "00100", "00100", "11111"};
    case 'K':
        return {"10001", "10010", "10100", "11000", "10100", "10010", "10001"};
    case 'L':
        return {"10000", "10000", "10000", "10000", "10000", "10000", "11111"};
    case 'M':
        return {"10001", "11011", "10101", "10101", "10001", "10001", "10001"};
    case 'N':
        return {"10001", "11001", "10101", "10011", "10001", "10001", "10001"};
    case 'S':
        return {"01111", "10000", "10000", "01110", "00001", "00001", "11110"};
    case 'T':
        return {"11111", "00100", "00100", "00100", "00100", "00100", "00100"};
    case 'U':
        return {"10001", "10001", "10001", "10001", "10001", "10001", "01110"};
    case 'V':
        return {"10001", "10001", "10001", "10001", "01010", "01010", "00100"};
    default:
        return {"00000", "00000", "00000", "00000", "00000", "00000", "00000"};
    }
}

// 計算一段 5x7 方塊字在指定 cell_size 下的總寬高。
cv::Size block_text_size(const std::string& text, int cell_size) {
    int char_gap = cell_size;
    int width = 0;
    for (size_t i = 0; i < text.size(); i++) {
        width += 5 * cell_size;
        if (i + 1 < text.size()) {
            width += char_gap;
        }
    }
    return cv::Size(width, 7 * cell_size);
}

// 實際把 5x7 方塊字畫到 OpenCV 畫布上。
void draw_block_text(
    cv::Mat& canvas,
    const std::string& text,
    cv::Point center,
    int cell_size,
    cv::Scalar color
) {
    cv::Size total = block_text_size(text, cell_size);
    int char_gap = cell_size;
    int pixel_gap = std::max(2, cell_size / 8);
    int x = center.x - total.width / 2;
    int y = center.y - total.height / 2;

    for (char ch : text) {
        std::array<std::string, 7> glyph = glyph_pattern(ch);
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (glyph[row][col] == '1') {
                    cv::Rect block(
                        x + col * cell_size,
                        y + row * cell_size,
                        cell_size - pixel_gap,
                        cell_size - pixel_gap
                    );
                    cv::rectangle(canvas, block, color, cv::FILLED, cv::LINE_8);
                }
            }
        }
        x += 5 * cell_size + char_gap;
    }
}

// 畫出單一 menu 選項。
// selected=true 且 blink_on=true 時，會用黑底白字做閃爍效果。
void draw_menu_label(
    cv::Mat& canvas,
    const std::string& text,
    cv::Point center,
    bool selected,
    bool blink_on
) {
    const int cell_size = (text.size() >= 4) ? 18 : 22;
    cv::Size text_size = block_text_size(text, cell_size);
    cv::Rect box(
        center.x - text_size.width / 2 - cell_size,
        center.y - text_size.height / 2 - cell_size,
        text_size.width + cell_size * 2,
        text_size.height + cell_size * 2
    );

    if (selected && blink_on) {
        cv::rectangle(canvas, box, cv::Scalar(0, 0, 0), cv::FILLED, cv::LINE_AA);
        draw_block_text(canvas, text, center, cell_size, cv::Scalar(255, 255, 255));
    } else {
        draw_block_text(canvas, text, center, cell_size, cv::Scalar(0, 0, 0));
    }
}

// 顯示某個功能被選取的提示文字。
// 目前確認選項後會直接啟動功能程式；這個函式保留給未來需要中間提示畫面的流程。
void draw_selected_message(cv::Mat& canvas, const std::string& label) {
    int center_x = canvas.cols / 2;
    int center_y = canvas.rows / 2;

    draw_block_text(canvas, label, cv::Point(center_x, center_y + 230), 34, cv::Scalar(0, 0, 0));
    draw_block_text(canvas, "SELECTED", cv::Point(center_x, center_y + 335), 15, cv::Scalar(0, 0, 0));
}

// 產生完整 menu 的 2D 原始畫面。
// 若未來新增選項，label_positions 的位置數量也要跟著調整。
void draw_menu_canvas(cv::Mat& canvas, const MenuContext& menu) {
    canvas = cv::Scalar(255, 255, 255);

    int center_x = canvas.cols / 2;
    int center_y = canvas.rows / 2;
    int outer_radius = (int)(canvas.cols * 0.44);
    bool blink_on = ((menu.blink_tick / 18) % 2) == 0;

    cv::circle(canvas, cv::Point(center_x, center_y), outer_radius, cv::Scalar(0, 0, 0), 10, cv::LINE_AA);

    draw_block_text(canvas, "MENU", cv::Point(center_x, center_y - (int)(canvas.rows * 0.25)), 30, cv::Scalar(0, 0, 0));

    std::array<cv::Point, 3> label_positions = {
        cv::Point(center_x - (int)(canvas.cols * 0.25), center_y + (int)(canvas.rows * 0.13)),
        cv::Point(center_x, center_y + (int)(canvas.rows * 0.32)),
        cv::Point(center_x + (int)(canvas.cols * 0.25), center_y + (int)(canvas.rows * 0.13))
    };

    for (int i = 0; i < 3; i++) {
        draw_menu_label(canvas, menu.labels[i], label_positions[i], i == menu.selected_index, blink_on);
    }

    if (menu.state == MenuState::ClockSelected) {
        draw_selected_message(canvas, "CLK");
    } else if (menu.state == MenuState::VideoSelected) {
        draw_selected_message(canvas, "VID");
    } else if (menu.state == MenuState::GameSelected) {
        draw_selected_message(canvas, "GAME");
    }
}

// 左右鍵切換選單項目。
// 目前共有 3 個選項，所以用 +3 和 %3 讓 index 在 0、1、2 之間循環。
// 若新增第 4 個選項，這裡的 3 要改成新的選項數量。
void move_selection(MenuContext& menu, int delta) {
    menu.selected_index = (menu.selected_index + delta + 3) % 3;
}

// ===== 功能連結總入口 =====
// 按下確認鍵時會進到這裡。
// 未來若要新增 menu 功能，通常要改三個地方：
// 1. MenuContext::labels：新增畫面上的文字。
// 2. draw_menu_canvas()：新增選項位置。
// 3. 新增對應功能的 .hpp，並在 confirm_selection() 把 selected_index 對應到該入口函式。
void confirm_selection(MenuContext& menu) {
    if (menu.selected_index == 0) {
        run_clock_app();
        menu.state = MenuState::MainMenu;
    } else if (menu.selected_index == 1) {
        run_video_app();
        menu.state = MenuState::MainMenu;
    } else {
        run_pong_app();
        menu.state = MenuState::MainMenu;
    }
}

// 處理鍵盤輸入。
// OpenCV 在 Windows/Linux 不同後端會回傳不同方向鍵代碼，所以這裡同時支援多組 key code。
// A/D/W/S 是備用鍵，方便在方向鍵沒有被 OpenCV 視窗正確接收時操作。
bool handle_key(MenuContext& menu, int key) {
    if (key < 0) {
        return true;
    }

    const bool left_key = key == 2424832 || key == 81 || key == 65361 || key == 'a' || key == 'A';
    const bool up_key = key == 2490368 || key == 82 || key == 65362 || key == 'w' || key == 'W';
    const bool right_key = key == 2555904 || key == 83 || key == 65363 || key == 'd' || key == 'D';
    const bool down_key = key == 2621440 || key == 84 || key == 65364 || key == 's' || key == 'S';

    if (key == 'q' || key == 'Q' || key == 27) {
        return false;
    }

    if (left_key) {
        move_selection(menu, -1);
        return true;
    }

    if (right_key) {
        move_selection(menu, 1);
        return true;
    }

    if (up_key) {
        confirm_selection(menu);
        return true;
    }

    if (down_key) {
        menu.state = MenuState::MainMenu;
        return true;
    }

    return true;
}

int main() {
    // menu 原始畫布大小。畫布越大，轉成 POV 後取樣越細緻。
    int side_len = 1800;

    // 先建立取樣查表，之後每一幀都重複使用。
    init_sampling_lut(side_len);

    MenuContext menu;
    POV_Frame pov_buffer;

    // menu_canvas 是完整的 2D 選單畫面。
    // simulation_canvas 是轉成 POV_Frame 後重建出的 POV 模擬畫面。
    cv::Mat menu_canvas = cv::Mat::zeros(side_len, side_len, CV_8UC3);

    float r_step_sim = 11.0f;
    int sim_window_size = (int)(LED_NUM * r_step_sim * 2) + 40;
    cv::Mat simulation_canvas = cv::Mat::zeros(sim_window_size, sim_window_size, CV_8UC3);

    cv::namedWindow("1. Menu Canvas", cv::WINDOW_NORMAL);
    cv::namedWindow("2. POV Simulator", cv::WINDOW_NORMAL);
    cv::resizeWindow("1. Menu Canvas", 600, 600);
    cv::resizeWindow("2. POV Simulator", 520, 520);

    printf("Menu started. Left/Right: choose, Up: confirm, Down: back, Q/ESC: exit.\n");

    bool running = true;
    while (running) {
        menu.blink_tick++;

        // 每一幀的流程：
        // 1. 畫出 2D menu。
        // 2. 轉成 POV buffer。
        // 3. 產生 POV 模擬畫面。
        // 4. 顯示視窗並讀取鍵盤。
        draw_menu_canvas(menu_canvas, menu);
        convert_to_pov_buffer(menu_canvas, &pov_buffer);
        simulate_pov_display(pov_buffer, simulation_canvas);

        cv::imshow("1. Menu Canvas", menu_canvas);
        cv::imshow("2. POV Simulator", simulation_canvas);

        int key = cv::waitKeyEx(16);
        running = handle_key(menu, key);
    }

    cv::destroyAllWindows();
    return 0;
}
