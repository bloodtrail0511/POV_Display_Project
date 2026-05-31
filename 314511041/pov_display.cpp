#include "pov_display.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

POVDisplay::POVDisplay() : POVDisplay(Config{}) {}

POVDisplay::POVDisplay(const Config& cfg) : cfg_(cfg) {
    spi_buf_size_ = 4 + cfg_.num_leds * 4 + cfg_.end_len;
    frame_size_ = cfg_.degree_resolution * spi_buf_size_;

    frame_.resize(frame_size_);
    lut_x_.resize(cfg_.degree_resolution * cfg_.num_leds);
    lut_y_.resize(cfg_.degree_resolution * cfg_.num_leds);

    initSamplingLut();
    initFrameStructure();
    openDevice();
}

POVDisplay::~POVDisplay() {
    closeDevice();
}

bool POVDisplay::openDevice() {
    closeDevice();
    fd_ = ::open(cfg_.device_path.c_str(), O_WRONLY);
    if (fd_ < 0) {
        perror("POVDisplay: open device failed");
        return false;
    }
    return true;
}

void POVDisplay::closeDevice() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

int POVDisplay::clampInt(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

int POVDisplay::wrapDegree(int deg, int degree_resolution) {
    deg %= degree_resolution;
    if (deg < 0) deg += degree_resolution;
    return deg;
}

void POVDisplay::initSamplingLut() {
    const int side_len = cfg_.canvas_size;
    const int center = side_len / 2;
    const float r_step = static_cast<float>(center) / static_cast<float>(cfg_.half_leds);

    for (int i = 0; i < cfg_.degree_resolution; i++) {
        float angle_A = i * 2.0f * static_cast<float>(M_PI) / cfg_.degree_resolution;
        float angle_B = angle_A + static_cast<float>(M_PI);

        for (int j = 0; j < cfg_.half_leds; j++) {
            float d_A = r_step * j + (r_step * 0.25f);
            int x_A = static_cast<int>(center - d_A * std::sin(angle_A));
            int y_A = static_cast<int>(center + d_A * std::cos(angle_A));

            float d_B = r_step * j + (r_step * 0.75f);
            int x_B = static_cast<int>(center - d_B * std::sin(angle_B));
            int y_B = static_cast<int>(center + d_B * std::cos(angle_B));

            int led_B = ledIndexB(j);
            int led_A = ledIndexA(j);

            lut_x_[lutIndex(i, led_B)] = clampInt(x_B, 0, side_len - 1);
            lut_y_[lutIndex(i, led_B)] = clampInt(y_B, 0, side_len - 1);

            lut_x_[lutIndex(i, led_A)] = clampInt(x_A, 0, side_len - 1);
            lut_y_[lutIndex(i, led_A)] = clampInt(y_A, 0, side_len - 1);
        }
    }
}

void POVDisplay::initFrameStructure() {
    std::memset(frame_.data(), 0, frame_.size());

    for (int deg = 0; deg < cfg_.degree_resolution; deg++) {
        int base_slice = deg * spi_buf_size_;

        frame_[base_slice + 0] = 0x00;
        frame_[base_slice + 1] = 0x00;
        frame_[base_slice + 2] = 0x00;
        frame_[base_slice + 3] = 0x00;

        for (int led = 0; led < cfg_.num_leds; led++) {
            int base = frameOffset(deg, led);
            frame_[base + 0] = static_cast<uint8_t>(0xE0 | clampInt(cfg_.apa102_brightness, 1, 31));
            frame_[base + 1] = 0x00;
            frame_[base + 2] = 0x00;
            frame_[base + 3] = 0x00;
        }

        for (int j = 0; j < cfg_.end_len; j++) {
            frame_[base_slice + spi_buf_size_ - cfg_.end_len + j] = 0xFF;
        }
    }
}

cv::Mat POVDisplay::prepareCanvas(const cv::Mat& input) const {
    if (input.empty()) return cv::Mat();

    cv::Mat bgr;
    if (input.type() == CV_8UC3) {
        bgr = input;
    } else if (input.type() == CV_8UC1) {
        cv::cvtColor(input, bgr, cv::COLOR_GRAY2BGR);
    } else if (input.type() == CV_8UC4) {
        cv::cvtColor(input, bgr, cv::COLOR_BGRA2BGR);
    } else {
        input.convertTo(bgr, CV_8UC3);
    }

    if (!cfg_.auto_fit_canvas &&
        bgr.cols == cfg_.canvas_size &&
        bgr.rows == cfg_.canvas_size) {
        return bgr;
    }

    int side = std::min(bgr.cols, bgr.rows);
    int x0 = (bgr.cols - side) / 2;
    int y0 = (bgr.rows - side) / 2;
    cv::Mat square = bgr(cv::Rect(x0, y0, side, side));

    if (side == cfg_.canvas_size) {
        return square.clone();
    }

    cv::Mat resized;
    cv::resize(square, resized, cv::Size(cfg_.canvas_size, cfg_.canvas_size), 0, 0, cv::INTER_AREA);
    return resized;
}

// uint8_t POVDisplay::scaleColor(uint8_t v) const {
//     int out = static_cast<int>(std::lround(static_cast<double>(v) * cfg_.pixel_brightness_scale));
//     return static_cast<uint8_t>(clampInt(out, 0, 255));
// }

// void POVDisplay::setLedPhysical(int deg, int led, uint8_t r, uint8_t g, uint8_t b) {
//     deg = wrapDegree(deg, cfg_.degree_resolution);
//     if (led < 0 || led >= cfg_.num_leds) return;

//     int base = frameOffset(deg, led);
//     frame_[base + 0] = static_cast<uint8_t>(0xE0 | clampInt(cfg_.apa102_brightness, 1, 31));
//     frame_[base + 1] = scaleColor(b);
//     frame_[base + 2] = scaleColor(g);
//     frame_[base + 3] = scaleColor(r);
// }
// 在 pov_display.cpp 頂部加入這個 Gamma 2.2 查表
// 這是根據公式 V_out = 255 * (V_in / 255)^2.2 算出來的標準 8-bit 表
static const uint8_t gamma8[] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  1,  1,  1,  1,  1,  1,  1,  1,  1,  2,  2,  2,  2,
    2,  2,  2,  3,  3,  3,  3,  3,  4,  4,  4,  4,  5,  5,  5,  5,
    6,  6,  6,  7,  7,  7,  8,  8,  8,  9,  9,  9, 10, 10, 11, 11,
    11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18, 19,
    19, 20, 20, 21, 22, 22, 23, 23, 24, 25, 25, 26, 26, 27, 28, 28,
    29, 30, 30, 31, 32, 33, 33, 34, 35, 36, 36, 37, 38, 39, 40, 40,
    41, 42, 43, 44, 45, 46, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55,
    56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71,
    73, 74, 75, 76, 77, 78, 80, 81, 82, 83, 84, 86, 87, 88, 89, 91,
    92, 93, 95, 96, 97, 99,100,101,103,104,106,107,109,110,111,113,
    114,116,117,119,120,122,123,125,127,128,130,131,133,135,136,138,
    140,141,143,145,147,148,150,152,154,155,157,159,161,163,165,167,
    169,171,173,175,177,179,181,183,185,187,189,191,193,195,198,200,
    202,204,207,209,211,214,216,218,221,223,226,228,231,233,236,238,
    241,244,246,249,251,254,255
};
// 1. 修改原本的 scaleColor，讓它支援獨立的白平衡權重與 Gamma 校正
uint8_t POVDisplay::scaleColor(uint8_t v, double balance_weight) const {
    // 先做白平衡與全局亮度縮放
    double scaled_value = static_cast<double>(v) * cfg_.pixel_brightness_scale * balance_weight;
    int clamped_val = clampInt(static_cast<int>(std::lround(scaled_value)), 0, 255);
    
    // 最後套用 Gamma 查表
    return gamma8[clamped_val];
    // return clamped_val;
}

// 在 setLedPhysical 中加入徑向亮度補償與白平衡
void POVDisplay::setLedPhysical(int deg, int led, uint8_t r, uint8_t g, uint8_t b) {
    deg = wrapDegree(deg, cfg_.degree_resolution);
    if (led < 0 || led >= cfg_.num_leds) return;

    // 1. 計算該顆 LED 距離圓心的「物理半徑比例」(0.0 ~ 1.0)
    // 根據你們硬體的 mapping：
    // A 側 (led 20~39): 20 在圓心, 39 在最外
    // B 側 (led 0~19): 19 在圓心, 0 在最外
    int radius_index = 0;
    if (led >= cfg_.half_leds) {
        radius_index = led - cfg_.half_leds;       // A 側距離: 0 ~ 19
    } else {
        radius_index = cfg_.half_leds - 1 - led;   // B 側距離: 0 ~ 19
    }
    
    // 算出比例 (0.0 代表最靠近圓心，1.0 代表最外圍)
    double radius_ratio = static_cast<double>(radius_index) / static_cast<double>(cfg_.half_leds - 1); 

    // 2. 設定徑向補償參數 (Radial Weight)
    // CENTER_DIM_FACTOR 決定最中心的 LED 要保留多少亮度 (例如 0.2 代表只剩 20% 亮度)
    // 這是一個線性插值：圓心亮度為 20%，最外圍為 100%
    const double CENTER_DIM_FACTOR = 0.20; 
    double radial_weight = CENTER_DIM_FACTOR + (1.0 - CENTER_DIM_FACTOR) * radius_ratio;

    // 3. 原本的白平衡參數
    // cfg_.r_balance = 1.00;
    // cfg_.g_balance = 0.85; 
    // cfg_.b_balance = 0.70; 

    int base = frameOffset(deg, led);
    
    // 4. 將白平衡與徑向權重一起乘進去
    frame_[base + 1] = scaleColor(b, cfg_.b_balance * radial_weight);
    frame_[base + 2] = scaleColor(g, cfg_.g_balance * radial_weight);
    frame_[base + 3] = scaleColor(r, cfg_.r_balance * radial_weight);
    
    frame_[base + 0] = static_cast<uint8_t>(0xE0 | clampInt(cfg_.apa102_brightness, 1, 31));
}

bool POVDisplay::convert(const cv::Mat& bgr_canvas) {
    cv::Mat canvas = prepareCanvas(bgr_canvas);
    if (canvas.empty()) return false;

    last_canvas_ = canvas;
    initFrameStructure();

    for (int deg = 0; deg < cfg_.degree_resolution; deg++) {
        for (int led = 0; led < cfg_.num_leds; led++) {
            int x = lut_x_[lutIndex(deg, led)];
            int y = lut_y_[lutIndex(deg, led)];
            const cv::Vec3b& c = canvas.at<cv::Vec3b>(y, x); // OpenCV BGR
            setLedPhysical(deg, led, c[2], c[1], c[0]);      // helper RGB
        }
    }
    return true;
}

bool POVDisplay::flush() {
    if (fd_ < 0 && !openDevice()) return false;

    ssize_t written = ::write(fd_, frame_.data(), frame_.size());
    if (written != static_cast<ssize_t>(frame_.size())) {
        perror("POVDisplay: write failed or wrong length");
        return false;
    }
    return true;
}

bool POVDisplay::show(const cv::Mat& bgr_canvas) {
    if (!convert(bgr_canvas)) return false;
    return flush();
}

bool POVDisplay::saveSampledPreview(const std::string& path) {
    if (last_canvas_.empty()) return false;

    cv::Mat sim = cv::Mat::zeros(cfg_.canvas_size, cfg_.canvas_size, CV_8UC3);
    for (int deg = 0; deg < cfg_.degree_resolution; deg++) {
        for (int led = 0; led < cfg_.num_leds; led++) {
            int x = lut_x_[lutIndex(deg, led)];
            int y = lut_y_[lutIndex(deg, led)];
            int base = frameOffset(deg, led);
            cv::Scalar color(frame_[base + 1], frame_[base + 2], frame_[base + 3]);
            cv::circle(sim, cv::Point(x, y), 2, color, -1, cv::LINE_AA);
        }
    }
    return cv::imwrite(path, sim);
}
