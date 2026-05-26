#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <algorithm>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

// app_vid.cpp 是 POV 影片播放功能。
// 它會讀取同資料夾下的 pac_man.gif，把每一幀縮放到圓形 POV 畫布內，
// 再用和 menu/clock 相同的取樣方式投影到虛擬旋轉 LED 螢幕。

#define NUM_SLICES 360
#define LED_NUM 20

#undef MAX
#undef MIN
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct POV_Frame {
    uint8_t data[NUM_SLICES][LED_NUM * 2][3];
} POV_Frame;

int lut_x[NUM_SLICES][LED_NUM * 2];
int lut_y[NUM_SLICES][LED_NUM * 2];

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

            lut_x[i][LED_NUM - j - 1] = MAX(0, MIN(x_B, side_len - 1));
            lut_y[i][LED_NUM - j - 1] = MAX(0, MIN(y_B, side_len - 1));
            lut_x[i][j + LED_NUM] = MAX(0, MIN(x_A, side_len - 1));
            lut_y[i][j + LED_NUM] = MAX(0, MIN(y_A, side_len - 1));
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
    int dot_size = MAX(1, (int)(r_step_sim * 0.2f));

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

cv::Mat normalize_to_bgr(const cv::Mat& src) {
    cv::Mat bgr;
    if (src.channels() == 4) {
        cv::cvtColor(src, bgr, cv::COLOR_BGRA2BGR);
    } else if (src.channels() == 1) {
        cv::cvtColor(src, bgr, cv::COLOR_GRAY2BGR);
    } else {
        bgr = src.clone();
    }
    return bgr;
}

bool load_video_frames(const std::string& path, std::vector<cv::Mat>& frames, double* fps) {
    cv::VideoCapture cap(path);
    if (cap.isOpened()) {
        double detected_fps = cap.get(cv::CAP_PROP_FPS);
        if (detected_fps > 0.0 && detected_fps < 120.0) {
            *fps = detected_fps;
        }

        cv::Mat frame;
        while (cap.read(frame)) {
            if (!frame.empty()) {
                frames.push_back(normalize_to_bgr(frame));
            }
        }
    }

    if (!frames.empty()) {
        return true;
    }

    std::vector<cv::Mat> still_frames;
    if (cv::imreadmulti(path, still_frames, cv::IMREAD_UNCHANGED)) {
        for (const cv::Mat& frame : still_frames) {
            if (!frame.empty()) {
                frames.push_back(normalize_to_bgr(frame));
            }
        }
    }

    if (!frames.empty()) {
        return true;
    }

    cv::Mat fallback = cv::imread(path, cv::IMREAD_UNCHANGED);
    if (!fallback.empty()) {
        frames.push_back(normalize_to_bgr(fallback));
        return true;
    }

    return false;
}

void draw_frame_on_circular_canvas(const cv::Mat& source_frame, cv::Mat& canvas) {
    canvas = cv::Scalar(0, 0, 0);

    int center_x = canvas.cols / 2;
    int center_y = canvas.rows / 2;
    int display_radius = (int)(canvas.cols * 0.44);
    int max_diameter = display_radius * 2;

    double scale = std::min(
        (double)max_diameter / source_frame.cols,
        (double)max_diameter / source_frame.rows
    );
    int frame_w = MAX(1, (int)(source_frame.cols * scale));
    int frame_h = MAX(1, (int)(source_frame.rows * scale));

    cv::Mat resized;
    cv::resize(source_frame, resized, cv::Size(frame_w, frame_h), 0, 0, cv::INTER_AREA);

    int x = center_x - frame_w / 2;
    int y = center_y - frame_h / 2;
    resized.copyTo(canvas(cv::Rect(x, y, frame_w, frame_h)));

    cv::Mat mask = cv::Mat::zeros(canvas.size(), CV_8UC1);
    cv::circle(mask, cv::Point(center_x, center_y), display_radius, cv::Scalar(255), -1, cv::LINE_AA);
    canvas.setTo(cv::Scalar(0, 0, 0), mask == 0);

    cv::circle(canvas, cv::Point(center_x, center_y), display_radius, cv::Scalar(40, 40, 40), 8, cv::LINE_AA);
}

int main() {
    const std::string gif_path = "pac_man.gif";
    const int side_len = 1800;
    init_sampling_lut(side_len);

    std::vector<cv::Mat> frames;
    double fps = 12.0;
    if (!load_video_frames(gif_path, frames, &fps)) {
        printf("Failed to load %s. Please put pac_man.gif in the same folder as app_vid.\n", gif_path.c_str());
        return 1;
    }

    int delay_ms = MAX(1, (int)(1000.0 / fps));
    POV_Frame pov_buffer;
    cv::Mat video_canvas = cv::Mat::zeros(side_len, side_len, CV_8UC3);

    float r_step_sim = 11.0f;
    int sim_window_size = (int)(LED_NUM * r_step_sim * 2) + 40;
    cv::Mat simulation_canvas = cv::Mat::zeros(sim_window_size, sim_window_size, CV_8UC3);

    cv::namedWindow("1. Video Canvas", cv::WINDOW_NORMAL);
    cv::namedWindow("2. POV Simulator", cv::WINDOW_NORMAL);
    cv::resizeWindow("1. Video Canvas", 600, 600);
    cv::resizeWindow("2. POV Simulator", 520, 520);

    printf("Video demo started. Loaded %zu frame(s) from %s.\n", frames.size(), gif_path.c_str());
    printf("Press Down, 's', 'q', or ESC to exit.\n");

    size_t frame_index = 0;
    while (1) {
        draw_frame_on_circular_canvas(frames[frame_index], video_canvas);
        convert_to_pov_buffer(video_canvas, &pov_buffer);
        simulate_pov_display(pov_buffer, simulation_canvas);

        cv::imshow("1. Video Canvas", video_canvas);
        cv::imshow("2. POV Simulator", simulation_canvas);

        int key = cv::waitKeyEx(delay_ms);
        bool down_key = key == 2621440 || key == 84 || key == 65364 || key == 's' || key == 'S';
        if (down_key || key == 'q' || key == 'Q' || key == 27) {
            break;
        }

        frame_index = (frame_index + 1) % frames.size();
    }

    cv::destroyAllWindows();
    return 0;
}
