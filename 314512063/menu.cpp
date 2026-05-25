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

#define NUM_SLICES 360
#define LED_NUM 20

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct POV_Frame {
    uint8_t data[NUM_SLICES][LED_NUM * 2][3];
} POV_Frame;

int lut_x[NUM_SLICES][LED_NUM * 2];
int lut_y[NUM_SLICES][LED_NUM * 2];

enum class MenuState {
    MainMenu,
    ClockSelected,
    VideoSelected,
    GameSelected
};

struct MenuContext {
    MenuState state = MenuState::MainMenu;
    int selected_index = 0;
    int blink_tick = 0;
    std::array<std::string, 3> labels = {"CLK", "VID", "GAME"};
};

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

void draw_selected_message(cv::Mat& canvas, const std::string& label) {
    int center_x = canvas.cols / 2;
    int center_y = canvas.rows / 2;

    draw_block_text(canvas, label, cv::Point(center_x, center_y + 230), 34, cv::Scalar(0, 0, 0));
    draw_block_text(canvas, "SELECTED", cv::Point(center_x, center_y + 335), 15, cv::Scalar(0, 0, 0));
}

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

void move_selection(MenuContext& menu, int delta) {
    menu.selected_index = (menu.selected_index + delta + 3) % 3;
}

void run_clock_app() {
    cv::destroyAllWindows();
#ifdef _WIN32
    int result = std::system("app_clock_v1.exe");
#else
    int result = std::system("./app_clock_v1");
#endif
    if (result != 0) {
        printf("Failed to run clock app. Please build app_clock_v1 first.\n");
    }
    cv::namedWindow("1. Menu Canvas", cv::WINDOW_NORMAL);
    cv::namedWindow("2. POV Simulator", cv::WINDOW_NORMAL);
    cv::resizeWindow("1. Menu Canvas", 600, 600);
    cv::resizeWindow("2. POV Simulator", 520, 520);
}

void confirm_selection(MenuContext& menu) {
    if (menu.selected_index == 0) {
        run_clock_app();
        menu.state = MenuState::MainMenu;
    } else if (menu.selected_index == 1) {
        menu.state = MenuState::VideoSelected;
    } else {
        menu.state = MenuState::GameSelected;
    }
}

bool handle_key(MenuContext& menu, int key) {
    if (key < 0) {
        return true;
    }

    const bool left_key = key == 2424832 || key == 81;
    const bool up_key = key == 2490368 || key == 82;
    const bool right_key = key == 2555904 || key == 83;
    const bool down_key = key == 2621440 || key == 84;

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
    int side_len = 1800;
    init_sampling_lut(side_len);

    MenuContext menu;
    POV_Frame pov_buffer;
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
