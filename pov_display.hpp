#ifndef POV_DISPLAY_HPP
#define POV_DISPLAY_HPP

#include <opencv2/opencv.hpp>
#include <cstdint>
#include <string>
#include <vector>
#include <sys/ioctl.h>

class POVDisplay {
public:
    struct Config {
        std::string device_path = "/dev/pov_display";
        std::string magnet_path = "/dev/mag_sensor";
        int num_leds = 40;
        int half_leds = 20;
        int degree_resolution = 180;
        int end_len = 5;  // must match kernel driver
        int canvas_size = 800;
        int apa102_brightness = 5;       // 1~31
        double pixel_brightness_scale = 0.25;
        bool auto_fit_canvas = true;
        double r_balance = 1.00;
        double g_balance = 0.75;
        double b_balance = 0.70;
    };

    POVDisplay();
    explicit POVDisplay(const Config& cfg);
    ~POVDisplay();

    POVDisplay(const POVDisplay&) = delete;
    POVDisplay& operator=(const POVDisplay&) = delete;

    bool isOpen() const { return display_fd_ >= 0; }
    bool isDisplayOpen() const { return display_fd_ >= 0; }
    bool isMagnetOpen() const { return mag_fd_ >= 0; }

    bool openDevice();          // open both
    bool openDisplayDevice();
    bool openMagnetDevice();
    
    void closeDevice();
    void closeDisplayDevice();
    void closeMagnetDevice();

    bool show(const cv::Mat& bgr_canvas);     // convert + write
    bool convert(const cv::Mat& bgr_canvas);  // convert only
    bool flush();                             // write only
    bool saveSampledPreview(const std::string& path);

    uint8_t readHallCount();
    bool off();

    size_t frameSize() const { return frame_.size(); }

private:
    Config cfg_;
    // int fd_ = -1;
    int display_fd_ = -1;
    int mag_fd_ = -1;
    uint8_t last_hall_count_ = 0;
    int spi_buf_size_ = 0;
    int frame_size_ = 0;

    std::vector<uint8_t> frame_;
    std::vector<int> lut_x_;
    std::vector<int> lut_y_;
    cv::Mat last_canvas_;

    static int clampInt(int v, int lo, int hi);
    static int wrapDegree(int deg, int degree_resolution);

    int lutIndex(int deg, int led) const { return deg * cfg_.num_leds + led; }
    int frameOffset(int deg, int led) const { return deg * spi_buf_size_ + 4 + led * 4; }

    int ledIndexA(int j) const { return cfg_.half_leds + j; }
    int ledIndexB(int j) const { return cfg_.half_leds - 1 - j; }

    void initSamplingLut();
    void initFrameStructure();
    cv::Mat prepareCanvas(const cv::Mat& input) const;
    // uint8_t scaleColor(uint8_t v) const;
    uint8_t scaleColor(uint8_t v, double balance_weight) const;
    void setLedPhysical(int deg, int led, uint8_t r, uint8_t g, uint8_t b);
};

#endif
