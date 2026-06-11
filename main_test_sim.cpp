#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <math.h>
#include <chrono>
#include <array>
#include <string>
#include <vector>
#include <sys/time.h>
#include <time.h>

#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <algorithm>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

#include <opencv2/opencv.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

// ==========================================================
// PC virtual POV display settings
// ==========================================================
#define NUM_SLICES       180
#define HALF_LED_NUM     20
#define TOTAL_LED_NUM    (HALF_LED_NUM * 2)
#define CANVAS_SIZE      800

#define ON_THRESHOLD     3
#define OFF_THRESHOLD    2
#define STABLE_COUNT_MAX 3

#define BOOT_ANIM_MS     2000
#define LOOP_DELAY_US    16000   // 約 60 FPS

#define CONTROLLER_PORT  9000
#define LOCAL_BTN_DEVICE "/dev/my_btn"

#define ENABLE_SOCKET_SERVER 1
#define ENABLE_LOCAL_BUTTON  1

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))

static volatile sig_atomic_t g_stop = 0;

void sigint_handler(int sig)
{
    (void)sig;
    g_stop = 1;
}

static long long now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()
    ).count();
}

// ==========================================================
// POV virtual frame format: 模擬硬體 buffer
// data[slice][led][BGR]
// ==========================================================
struct POV_Frame {
    uint8_t data[NUM_SLICES][TOTAL_LED_NUM][3];
};

class VirtualPOVDisplay {
public:
    VirtualPOVDisplay()
    {
        initSamplingLut(CANVAS_SIZE);

        float r_step_sim = 8.0f;
        sim_window_size_ = static_cast<int>(HALF_LED_NUM * r_step_sim * 2) + 100;
        simulation_canvas_ = cv::Mat::zeros(sim_window_size_, sim_window_size_, CV_8UC3);

        // cv::namedWindow(window_name_, cv::WINDOW_AUTOSIZE);
        // cv::namedWindow(raw_window_name_, cv::WINDOW_AUTOSIZE); // <-- 新增這一行
        // 1. POV 模擬視窗：因為你的 sim_window_size_ 是動態計算的，直接用它來固定大小
        cv::namedWindow(window_name_, cv::WINDOW_NORMAL);
        cv::resizeWindow(window_name_, sim_window_size_, sim_window_size_);

        // 2. 原始方形畫布視窗：固定為 CANVAS_SIZE (800x800)
        cv::namedWindow(raw_window_name_, cv::WINDOW_NORMAL);
        cv::resizeWindow(raw_window_name_, CANVAS_SIZE, CANVAS_SIZE);
        printf("[SIM] Virtual POV device started.\n");
        printf("[SIM] Motor keys: s=spin on, x=spin off, +=hall +1, -=hall -1\n");
        printf("[SIM] Menu keys : a/d or left/right=choose, w/enter=confirm, b=back, q/ESC=quit\n");
    }

    ~VirtualPOVDisplay()
    {
        cv::destroyWindow(window_name_);
    }

    void setStatus(const char* state_name, uint8_t hall_count)
    {
        state_name_ = state_name;
        status_hall_count_ = hall_count;
    }

    bool show(const cv::Mat& input_canvas)
    {
        cv::Mat canvas;

        if (input_canvas.empty()) {
            canvas = cv::Mat::zeros(CANVAS_SIZE, CANVAS_SIZE, CV_8UC3);
        } else {
            // 💡 補上與實體硬體相同的「置中裁切 (Center Crop)」邏輯
            int side = std::min(input_canvas.cols, input_canvas.rows);
            int x0 = (input_canvas.cols - side) / 2;
            int y0 = (input_canvas.rows - side) / 2;
            
            // 擷取出正方形區域
            cv::Mat square = input_canvas(cv::Rect(x0, y0, side, side));

            // 如果大小不是 CANVAS_SIZE (800)，才等比例縮放
            if (square.cols != CANVAS_SIZE) {
                // 使用 INTER_AREA 縮小圖片畫質會比較好
                cv::resize(square, canvas, cv::Size(CANVAS_SIZE, CANVAS_SIZE), 0, 0, cv::INTER_AREA);
            } else {
                canvas = square.clone();
            }
        }

        // 確保顏色通道是 BGR
        if (canvas.channels() == 1) {
            cv::cvtColor(canvas, canvas, cv::COLOR_GRAY2BGR);
        } else if (canvas.channels() == 4) {
            cv::cvtColor(canvas, canvas, cv::COLOR_BGRA2BGR);
        }

        // 下面的底層繪圖邏輯保持不變...
        convertToPovBuffer(canvas, &pov_buffer_);
        simulatePovDisplay(pov_buffer_, simulation_canvas_);
        drawOverlay(simulation_canvas_);
        cv::imshow(window_name_, simulation_canvas_);

        cv::imshow(raw_window_name_, canvas);
        return true;
    }

    bool off()
    {
        cv::Mat black = cv::Mat::zeros(CANVAS_SIZE, CANVAS_SIZE, CV_8UC3);
        return show(black);
    }

    uint8_t readHallCount()
    {
        last_key_ = cv::waitKeyEx(1);
        handleDeviceKey(last_key_);

        // 模擬穩定旋轉時，Hall count 逐漸累加到上限。
        // 模擬停止/轉速失效時，Hall count 逐漸掉到 0。
        if (virtual_motor_on_) {
            if (last_hall_count_ < STABLE_COUNT_MAX) {
                last_hall_count_++;
            }
        } else {
            if (last_hall_count_ > 0) {
                last_hall_count_--;
            }
        }

        return last_hall_count_;
    }

    int consumeKey()
    {
        int key = last_key_;
        last_key_ = -1;
        return key;
    }

    bool shouldQuit() const
    {
        return quit_requested_;
    }

private:
    const char* window_name_ = "POV Virtual Device";
    const char* raw_window_name_ = "Raw Square Canvas"; // <-- 新增這一行

    int lut_x_[NUM_SLICES][TOTAL_LED_NUM];
    int lut_y_[NUM_SLICES][TOTAL_LED_NUM];

    POV_Frame pov_buffer_ = {};
    cv::Mat simulation_canvas_;
    int sim_window_size_ = 0;

    bool virtual_motor_on_ = false;
    bool quit_requested_ = false;
    uint8_t last_hall_count_ = 0;
    int last_key_ = -1;

    const char* state_name_ = "STATE_IDLE";
    uint8_t status_hall_count_ = 0;

    void handleDeviceKey(int key)
    {
        if (key < 0) return;

        if (key == 'q' || key == 'Q' || key == 27) {
            quit_requested_ = true;
            return;
        }

        if (key == 's' || key == 'S') {
            virtual_motor_on_ = true;
            printf("[SIM] virtual motor ON\n");
            return;
        }

        if (key == 'x' || key == 'X') {
            virtual_motor_on_ = false;
            printf("[SIM] virtual motor OFF\n");
            return;
        }

        if (key == '+' || key == '=') {
            if (last_hall_count_ < STABLE_COUNT_MAX) {
                last_hall_count_++;
            }
            printf("[SIM] hall_count = %u\n", last_hall_count_);
            return;
        }

        if (key == '-' || key == '_') {
            if (last_hall_count_ > 0) {
                last_hall_count_--;
            }
            printf("[SIM] hall_count = %u\n", last_hall_count_);
            return;
        }
    }

    void initSamplingLut(int side_len)
    {
        int center = side_len / 2;
        float r_step = static_cast<float>(center) / HALF_LED_NUM;

        for (int i = 0; i < NUM_SLICES; i++) {
            float angle_A = i * 2.0f * static_cast<float>(M_PI) / NUM_SLICES;
            float angle_B = angle_A + static_cast<float>(M_PI);

            for (int j = 0; j < HALF_LED_NUM; j++) {
                float d_A = r_step * j + (r_step * 0.25f);
                int x_A = static_cast<int>(center - d_A * sin(angle_A));
                int y_A = static_cast<int>(center + d_A * cos(angle_A));

                float d_B = r_step * j + (r_step * 0.75f);
                int x_B = static_cast<int>(center - d_B * sin(angle_B));
                int y_B = static_cast<int>(center + d_B * cos(angle_B));

                lut_x_[i][HALF_LED_NUM - j - 1] = CLAMP(x_B, 0, side_len - 1);
                lut_y_[i][HALF_LED_NUM - j - 1] = CLAMP(y_B, 0, side_len - 1);

                lut_x_[i][j + HALF_LED_NUM] = CLAMP(x_A, 0, side_len - 1);
                lut_y_[i][j + HALF_LED_NUM] = CLAMP(y_A, 0, side_len - 1);
            }
        }
    }

    void convertToPovBuffer(const cv::Mat& canvas, POV_Frame* buffer)
    {
        const uint8_t* raw_pixels = canvas.data;
        int step = static_cast<int>(canvas.step);
        int channels = canvas.channels();

        for (int i = 0; i < NUM_SLICES; i++) {
            for (int j = 0; j < TOTAL_LED_NUM; j++) {
                int offset = lut_y_[i][j] * step + lut_x_[i][j] * channels;
                buffer->data[i][j][0] = raw_pixels[offset + 0];
                buffer->data[i][j][1] = raw_pixels[offset + 1];
                buffer->data[i][j][2] = raw_pixels[offset + 2];
            }
        }
    }

    void simulatePovDisplay(const POV_Frame& buffer, cv::Mat& display_canvas)
    {
        display_canvas = cv::Scalar(0, 0, 0);

        int sim_w = display_canvas.cols;
        int sim_h = display_canvas.rows;
        int sim_center = sim_w / 2;

        float r_step_sim = 8.0f;
        int dot_size = CLAMP(static_cast<int>(r_step_sim * 0.3f), 1, 4);

        for (int i = 0; i < NUM_SLICES; i++) {
            float angle_phys_A = i * 2.0f * static_cast<float>(M_PI) / NUM_SLICES;
            float angle_phys_B = angle_phys_A + static_cast<float>(M_PI);

            for (int j = 0; j < HALF_LED_NUM; j++) {
                float d_A = r_step_sim * j + (r_step_sim * 0.25f);
                int draw_x_A = static_cast<int>(sim_center - d_A * sin(angle_phys_A));
                int draw_y_A = static_cast<int>(sim_center + d_A * cos(angle_phys_A));

                cv::Scalar color_A(
                    buffer.data[i][j + HALF_LED_NUM][0],
                    buffer.data[i][j + HALF_LED_NUM][1],
                    buffer.data[i][j + HALF_LED_NUM][2]
                );
                cv::circle(display_canvas, cv::Point(draw_x_A, draw_y_A), dot_size, color_A, -1, cv::LINE_AA);

                float d_B = r_step_sim * j + (r_step_sim * 0.75f);
                int draw_x_B = static_cast<int>(sim_center - d_B * sin(angle_phys_B));
                int draw_y_B = static_cast<int>(sim_center + d_B * cos(angle_phys_B));

                cv::Scalar color_B(
                    buffer.data[i][HALF_LED_NUM - j - 1][0],
                    buffer.data[i][HALF_LED_NUM - j - 1][1],
                    buffer.data[i][HALF_LED_NUM - j - 1][2]
                );
                cv::circle(display_canvas, cv::Point(draw_x_B, draw_y_B), dot_size, color_B, -1, cv::LINE_AA);
            }
        }

        // 0 度參考點，跟硬體磁鐵位置對應
        cv::circle(display_canvas, cv::Point(sim_center, sim_h - 12), 5, cv::Scalar(0, 0, 255), -1);
    }

    void drawOverlay(cv::Mat& display_canvas)
    {
        char line1[128];
        char line2[180];

        snprintf(line1, sizeof(line1), "state: %s | hall_count: %u | motor: %s",
                 state_name_, status_hall_count_, virtual_motor_on_ ? "ON" : "OFF");
        snprintf(line2, sizeof(line2), "keys: s/x=motor, a/d=menu, w/enter=confirm, b=back, q=quit");

        cv::putText(display_canvas, line1, cv::Point(8, 18),
                    cv::FONT_HERSHEY_SIMPLEX, 0.38, cv::Scalar(200, 200, 200), 1, cv::LINE_AA);
        cv::putText(display_canvas, line2, cv::Point(8, display_canvas.rows - 30),
                    cv::FONT_HERSHEY_SIMPLEX, 0.34, cv::Scalar(160, 160, 160), 1, cv::LINE_AA);
    }
};

typedef enum {
    STATE_IDLE,
    STATE_BOOT_ANIM,
    STATE_SHUTDOWN_ANIM,
    STATE_MAIN_MENU,
    STATE_CLOCK,
    STATE_VIDEO,
    STATE_VIDEO_PLAYING,
    STATE_GAME,
    STATE_COUNT
} SystemState;

static const char* stateName(SystemState state)
{
    switch (state) {
    case STATE_IDLE: return "STATE_IDLE";
    case STATE_BOOT_ANIM: return "STATE_BOOT_ANIM";
    case STATE_SHUTDOWN_ANIM: return "STATE_SHUTDOWN_ANIM";
    case STATE_MAIN_MENU: return "STATE_MAIN_MENU";
    case STATE_CLOCK: return "STATE_CLOCK";
    case STATE_VIDEO: return "STATE_VIDEO";
    case STATE_VIDEO_PLAYING: return "STATE_VIDEO_PLAYING";
    case STATE_GAME: return "STATE_GAME";
    default: return "STATE_UNKNOWN";
    }
}

typedef struct {
    VirtualPOVDisplay* display;
    int menu_index;
    int video_index;
    int blink_tick;
    uint8_t stable_hall_count;
    SystemState current_state;
} AppContext;

static bool is_left_key(int key)
{
    // 補上常見的 65361 (Linux), 2424832 (舊版), 81 (Q), 以及 0x51 或特殊編碼的遮罩值
    return key == 'a' || key == 'A' || 
           key == 2424832 || key == 81 || key == 65361 || 
           (key & 0xFF) == 81 || (key & 0xFFFF) == 65361;
}

static bool is_right_key(int key)
{
    // 補上常見的 65363 (Linux), 2555904 (舊版), 83 (S), 以及 0x53 或特殊編碼的遮罩值
    return key == 'd' || key == 'D' || 
           key == 2555904 || key == 83 || key == 65363 || 
           (key & 0xFF) == 83 || (key & 0xFFFF) == 65363;
}

static bool is_confirm_key(int key)
{
    // 補上 65362 (Linux 向上方向鍵), 13/10 (Enter), 2490368 (舊版)
    return key == 'w' || key == 'W' || key == 13 || key == 10 || 
           key == 2490368 || key == 82 || key == 65362 || 
           (key & 0xFF) == 13 || (key & 0xFF) == 10 || (key & 0xFFFF) == 65362;
}

static bool is_back_key(int key)
{
    // 補上 65364 (Linux 向下方向鍵), 8 (Backspace), 2621440 (舊版)
    return key == 'b' || key == 'B' || key == 8 || 
           key == 2621440 || key == 84 || key == 65364 || 
           (key & 0xFF) == 8 || (key & 0xFFFF) == 65364;
}
static void draw_centered_text(cv::Mat& canvas, const std::string& text, cv::Point center, double scale, int thickness, cv::Scalar color)
{
    int baseline = 0;
    cv::Size text_size = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, scale, thickness, &baseline);
    cv::Point origin(center.x - text_size.width / 2, center.y + text_size.height / 2);
    cv::putText(canvas, text, origin, cv::FONT_HERSHEY_SIMPLEX, scale, color, thickness, cv::LINE_AA);
}

static cv::Mat load_icon_image(const std::string& path)
{
    cv::Mat img = cv::imread(path, cv::IMREAD_UNCHANGED);
    if (!img.empty()) {
        printf("[MENU] loaded icon: %s\n", path.c_str());
    } else {
        printf("[MENU] icon not found, using fallback: %s\n", path.c_str());
    }
    return img;
}

static void overlay_image(cv::Mat& dst, const cv::Mat& src, cv::Point center, int target_size, double opacity)
{
    if (src.empty()) return;

    cv::Mat icon;
    cv::resize(src, icon, cv::Size(target_size, target_size), 0, 0, cv::INTER_AREA);

    int x0 = center.x - target_size / 2;
    int y0 = center.y - target_size / 2;

    for (int y = 0; y < icon.rows; y++) {
        int dy = y0 + y;
        if (dy < 0 || dy >= dst.rows) continue;

        for (int x = 0; x < icon.cols; x++) {
            int dx = x0 + x;
            if (dx < 0 || dx >= dst.cols) continue;

            cv::Vec3b src_bgr;
            double alpha = opacity;

            if (icon.channels() == 4) {
                cv::Vec4b px = icon.at<cv::Vec4b>(y, x);
                src_bgr = cv::Vec3b(px[0], px[1], px[2]);
                alpha *= static_cast<double>(px[3]) / 255.0;
            } else {
                cv::Vec3b px = icon.at<cv::Vec3b>(y, x);
                src_bgr = px;
            }

            cv::Vec3b& dst_px = dst.at<cv::Vec3b>(dy, dx);
            for (int c = 0; c < 3; c++) {
                dst_px[c] = static_cast<uint8_t>(dst_px[c] * (1.0 - alpha) + src_bgr[c] * alpha);
            }
        }
    }
}

static void draw_clock_fallback(cv::Mat& canvas, cv::Point center, int radius) {
    canvas = cv::Scalar(0, 0, 0); // 清空畫布為黑色

    // 畫時鐘外圈與中心基準點
    cv::circle(canvas, cv::Point(center.x, center.y), radius + 40, cv::Scalar(50, 50, 50), 20, cv::LINE_AA);
    cv::circle(canvas, cv::Point(center.x, center.y), 5, cv::Scalar(80, 80, 80), -1, cv::LINE_AA);

    for (int hour = 1; hour <= 12; hour++) {
        // 一個小時對應 30 度
        double angle_deg = hour * 30.0;
        double angle_rad = angle_deg * M_PI / 180.0;

        // 判斷：如果是 3, 6, 9, 12 就畫數字；否則畫線段
        if (hour % 3 == 0) {
            // 計算數字的 X, Y 座標
            int x = center.x + static_cast<int>((radius - 30) * sin(angle_rad));
            int y = center.y - static_cast<int>((radius - 30) * cos(angle_rad));

            char label[3];
            snprintf(label, sizeof(label), "%d", hour);

            // 畫出大數字 (Scale = 3.5, Thickness = 7)
            draw_centered_text(canvas, label, cv::Point(x, y), 4.0, 9, cv::Scalar(255, 255, 255));
        } else {
            // 畫指向圓心的刻度線段
            int tick_length = 30; // 刻度線的總長度
            
            // 讓線段的中心點剛好落在 number_radius 上，這樣視覺上會和數字對齊
            int outer_r = radius + (tick_length / 2);
            int inner_r = radius - (tick_length / 2);

            // 計算線段外側端點
            int x_outer = center.x + static_cast<int>(outer_r * sin(angle_rad));
            int y_outer = center.y - static_cast<int>(outer_r * cos(angle_rad));

            // 計算線段內側端點
            int x_inner = center.x + static_cast<int>(inner_r * sin(angle_rad));
            int y_inner = center.y - static_cast<int>(inner_r * cos(angle_rad));

            // 畫出淺灰色的粗線段 (Thickness = 6)
            cv::line(canvas, cv::Point(x_outer, y_outer), cv::Point(x_inner, y_inner), 
                     cv::Scalar(200, 200, 200), 6, cv::LINE_AA);
        }
    }
    time_t now = time(NULL);
    struct tm local_tm;

    // Windows 使用 localtime_s；Linux/macOS 使用 localtime。
    // 這樣寫可以讓程式在不同作業系統上都能編譯。
#ifdef _WIN32
    localtime_s(&local_tm, &now);
#else
    local_tm = *localtime(&now);
#endif

    // int outer_radius = (int)(canvas.cols * 0.39);
    int outer_radius = radius + 15;

    // 秒、分、時都使用浮點數，讓指針可以平滑地反映時間。
    // 例如 3:30 時，時針不會停在 3，而是介於 3 和 4 之間。
    double second_value = (double)local_tm.tm_sec;
    double minute_value = (double)local_tm.tm_min + second_value / 60.0;
    double hour_value = (double)(local_tm.tm_hour % 12) + minute_value / 60.0;

    // 角度換算：秒針與分針每單位 6 度，時針每小時 30 度。
    double second_angle = second_value * 6.0;
    double minute_angle = minute_value * 6.0;
    double hour_angle = hour_value * 30.0;

    // lambda 函式：將角度與長度比例轉換成指針末端座標。
    auto endpoint = [&](double angle_deg, double length_scale) {
        double angle_rad = angle_deg * M_PI / 180.0;
        int x = (int)(center.x + outer_radius * length_scale * sin(angle_rad));
        int y = (int)(center.y - outer_radius * length_scale * cos(angle_rad));
        return cv::Point(x, y);
    };

    // cv::Point center(center_x, center_y);
    cv::Point hour_tip = endpoint(hour_angle, 0.50);    // 時針較短
    cv::Point minute_tip = endpoint(minute_angle, 0.72); // 分針中等長度
    cv::Point second_tip = endpoint(second_angle, 0.84); // 秒針較長

    // 依序畫出時針、分針、秒針。
    double size_scale = canvas.cols / 1200.0;
    cv::line(canvas, center, hour_tip, cv::Scalar(255, 255, 255), (int)(20 * size_scale), cv::LINE_AA);
    cv::line(canvas, center, minute_tip, cv::Scalar(220, 220, 220), (int)(12 * size_scale), cv::LINE_AA);
    cv::line(canvas, center, second_tip, cv::Scalar(80, 80, 255), (int)(5 * size_scale), cv::LINE_AA);

    // 畫中心圓點，蓋住三根指針交會處，使畫面較整潔。
    cv::circle(canvas, center, (int)(16 * size_scale), cv::Scalar(230, 230, 230), -1, cv::LINE_AA);
    cv::circle(canvas, center, (int)(7 * size_scale), cv::Scalar(80, 80, 255), -1, cv::LINE_AA);
}


static cv::Mat get_video_thumbnail(const std::string& video_path) {
    cv::VideoCapture cap(video_path);
    
    if (!cap.isOpened()) {
        printf("[VIDEO] Failed to open video: %s\n", video_path.c_str());
        return cv::Mat(); // 回傳空影像
    }

    // 💡 小技巧：不要抓第 0 幀，因為很多影片開頭前幾幀全黑的
    // 我們跳到第 30 幀 (大約 1 秒處) 來當縮圖
    cap.set(cv::CAP_PROP_POS_FRAMES, 30); 

    cv::Mat frame;
    cap >> frame; // 讀取這一幀
    cap.release(); // 讀完馬上關閉釋放資源

    if (frame.empty()) {
        printf("[VIDEO] Failed to read frame from: %s\n", video_path.c_str());
        return cv::Mat();
    }

    // 💡 進階處理：把長方形影片「置中裁切」成完美的正方形
    int min_dim = std::min(frame.cols, frame.rows);
    cv::Rect crop_region(
        (frame.cols - min_dim) / 2, 
        (frame.rows - min_dim) / 2, 
        min_dim, min_dim
    );
    cv::Mat square_frame = frame(crop_region);

    printf("[VIDEO] Successfully extracted thumbnail from: %s\n", video_path.c_str());
    return square_frame;
}

// 💡 全域的縮圖快取與標記
static std::vector<cv::Mat> g_video_thumbnails;
static bool g_videos_loaded = false;

// 💡 確保影片已載入的共用函式
static void ensure_videos_loaded() {
    if (g_videos_loaded) return; // 已經載入過就直接 Return，保證不卡頓
    
    printf("[VIDEO] Scanning video files in 'assets/video/'...\n");
    std::vector<cv::String> video_files;
    cv::glob("assets/video/*.mp4", video_files, false);
    
    if (!video_files.empty()) {
        std::sort(video_files.begin(), video_files.end()); // 確保照字母排序
        for (const auto& path : video_files) {
            cv::Mat thumb = get_video_thumbnail(path);
            if (!thumb.empty()) {
                g_video_thumbnails.push_back(thumb);
            }
        }
    }
    
    g_videos_loaded = true;
    printf("[VIDEO] All thumbnails loaded! Total: %zu videos.\n", g_video_thumbnails.size());
}

static cv::Mat draw_video_menu_canvas(const AppContext* ctx)
{
    cv::Mat canvas = cv::Mat::zeros(CANVAS_SIZE, CANVAS_SIZE, CV_8UC3);
    int cx = CANVAS_SIZE / 2;
    int cy = CANVAS_SIZE / 2;

    // 💡 呼叫共用函式，取得影片數量
    ensure_videos_loaded();
    int num_vids = g_video_thumbnails.size();

    // 防呆：如果沒影片，畫出 NO VIDEOS 提示
    if (num_vids == 0) {
        cv::circle(canvas, cv::Point(cx, cy), static_cast<int>(CANVAS_SIZE * 0.50), cv::Scalar(80, 80, 80), 35, cv::LINE_AA);
        draw_centered_text(canvas, "NO VIDEOS", cv::Point(cx, cy), 3.0, 7, cv::Scalar(100, 100, 100));
        draw_centered_text(canvas, "VIDEOS", cv::Point(cx, cy - 200), 3.0, 8, cv::Scalar(200, 200, 200));
        return canvas; 
    }

    int v_idx = ctx->video_index % num_vids; 
    int prev_idx = (v_idx - 1 + num_vids) % num_vids;
    int next_idx = (v_idx + 1) % num_vids;

    // 畫左右縮圖
    overlay_image(canvas, g_video_thumbnails[prev_idx], cv::Point(cx - 360, cy), 180, 0.3);
    overlay_image(canvas, g_video_thumbnails[next_idx], cv::Point(cx + 360, cy), 180, 0.3);

    // 畫中間選中的縮圖
    bool blink_on = ((now_ms() / 500) % 2) == 0;
    // overlay_image(canvas, g_video_thumbnails[v_idx], cv::Point(cx, cy), 280, blink_on ? 1.0 : 0.6);
    overlay_image(canvas, g_video_thumbnails[v_idx], cv::Point(cx, cy), 380, 1.0);

    // 畫深灰色外圈遮罩
    cv::circle(canvas, cv::Point(cx, cy), static_cast<int>(CANVAS_SIZE * 0.50), cv::Scalar(80, 80, 80), 35, cv::LINE_AA);


    int float_offset = static_cast<int>(15.0 * sin(now_ms() / 300.0));
    int left_arrow_x = cx - 235 - float_offset;
    int right_arrow_x = cx + 235 + float_offset;
    draw_centered_text(canvas, "<", cv::Point(left_arrow_x, cy), 2.5, 5, cv::Scalar(255, 255, 255));
    draw_centered_text(canvas, ">", cv::Point(right_arrow_x, cy), 2.5, 5, cv::Scalar(255, 255, 255));

    return canvas;
}

static void draw_gamepad_fallback(cv::Mat& canvas, cv::Point center, int radius, cv::Scalar color)
{
    int w = static_cast<int>(radius * 1.9);
    int h = static_cast<int>(radius * 1.05);
    cv::Rect body(center.x - w / 2, center.y - h / 2, w, h);
    cv::ellipse(canvas, cv::Point(center.x - w / 4, center.y), cv::Size(radius * 6 / 10, radius * 5 / 10), 0, 0, 360, color, 10, cv::LINE_AA);
    cv::ellipse(canvas, cv::Point(center.x + w / 4, center.y), cv::Size(radius * 6 / 10, radius * 5 / 10), 0, 0, 360, color, 10, cv::LINE_AA);
    cv::rectangle(canvas, body, color, 10, cv::LINE_AA);

    cv::line(canvas, cv::Point(center.x - radius, center.y), cv::Point(center.x - radius / 2, center.y), color, 8, cv::LINE_AA);
    cv::line(canvas, cv::Point(center.x - radius * 3 / 4, center.y - radius / 4),
             cv::Point(center.x - radius * 3 / 4, center.y + radius / 4), color, 8, cv::LINE_AA);

    cv::circle(canvas, cv::Point(center.x + radius / 2, center.y - radius / 5), 9, color, -1, cv::LINE_AA);
    cv::circle(canvas, cv::Point(center.x + radius * 4 / 5, center.y + radius / 6), 9, color, -1, cv::LINE_AA);
}

static void draw_icon_item(cv::Mat& canvas, int index, cv::Point center, bool selected,
                           bool blink_on, const std::vector<cv::Mat>& icons)
{
    // 1. 設定圖案大小
    int icon_size = selected ? 300 : 180;

    // 2. 計算透明度，讓選中的圖案自己閃爍
    double opacity = 0.30; // 未選中時的預設透明度 (比較暗)
    
    if (selected) {
        // 選中時，根據 blink_on 在 1.0 (全亮) 與 0.5 (半暗) 之間切換來製造閃爍感
        opacity = blink_on ? 1.0 : 0.5; 
    }

    // 3. 疊加圖片 (同時把之前的 fallback 也清掉了)
    if (!icons[index].empty()) {
        overlay_image(canvas, icons[index], center, icon_size, opacity);
    }
}

static cv::Mat draw_main_menu_canvas(const AppContext* ctx)
{
    static std::vector<cv::Mat> icons = {
        load_icon_image("assets/icon/clock.png"),
        load_icon_image("assets/icon/youtube.png"),
        load_icon_image("assets/icon/game.png")
    };

    cv::Mat canvas = cv::Mat::zeros(CANVAS_SIZE, CANVAS_SIZE, CV_8UC3);

    int cx = CANVAS_SIZE / 2;
    int cy = CANVAS_SIZE / 2;
    // bool blink_on = ((ctx->blink_tick / 13) % 2) == 0;
    bool blink_on = ((now_ms() / 500) % 2) == 0;

    // 外圈：保留一點 POV 環形介面的感覺。
    cv::circle(canvas, cv::Point(cx, cy), static_cast<int>(CANVAS_SIZE * 0.50), cv::Scalar(80, 80, 80), 35, cv::LINE_AA);

    // icon 位置參照你同學 menu.cpp 的三角配置，但改成 icon。
    std::array<cv::Point, 3> positions = {
        cv::Point(cx - static_cast<int>(CANVAS_SIZE * 0.25), cy + static_cast<int>(CANVAS_SIZE * 0.08)),
        cv::Point(cx,                                      cy + static_cast<int>(CANVAS_SIZE * 0.27)),
        cv::Point(cx + static_cast<int>(CANVAS_SIZE * 0.25), cy + static_cast<int>(CANVAS_SIZE * 0.08))
    };

    for (int i = 0; i < 3; i++) {
        draw_icon_item(canvas, i, positions[i], i == ctx->menu_index, blink_on, icons);
    }

    // 這不是選項文字，只是操作提示；實際硬體若覺得太糊可以拿掉。
    draw_centered_text(canvas, "MENU",
                   cv::Point(cx, cy - static_cast<int>(CANVAS_SIZE * 0.22)),
                   4.5, 13, cv::Scalar(220, 220, 220));

    return canvas;
}

static cv::Mat make_boot_green_canvas(double ratio)
{
    int center = CANVAS_SIZE / 2;
    ratio = CLAMP(ratio, 0.0, 1.0);

    cv::Mat canvas = cv::Mat::zeros(CANVAS_SIZE, CANVAS_SIZE, CV_8UC3);
    int min_radius = 8;
    int max_radius = center;
    int radius = min_radius + static_cast<int>((max_radius - min_radius) * ratio);

    cv::circle(canvas,
               cv::Point(center, center),
               radius,
               cv::Scalar(0, 255, 0),
               -1,
               cv::LINE_AA);

    return canvas;
}

static cv::Mat make_boot_sweep_canvas(const AppContext* ctx, double ratio)
{
    int center = CANVAS_SIZE / 2;
    int max_radius = CANVAS_SIZE; // 確保能涵蓋整個畫面

    // 1. 先偷偷畫出目標畫面 (Main Menu)
    cv::Mat target_canvas = draw_main_menu_canvas(ctx);

    // 2. 準備一張黑底畫布，和一張用來「挖洞」的單通道遮罩
    cv::Mat canvas = cv::Mat::zeros(CANVAS_SIZE, CANVAS_SIZE, CV_8UC3);
    cv::Mat mask = cv::Mat::zeros(CANVAS_SIZE, CANVAS_SIZE, CV_8UC1);

    // 3. 在遮罩上畫出「扇形 (Pie Slice)」
    // OpenCV 的 ellipse 函式，當 thickness = -1 時可以畫出實心扇形
    // 角度設定：從 -90 度 (12點鐘方向) 開始，畫出 ratio * 360 度的扇形
    double sweep_angle = ratio * 360.0;
    cv::ellipse(mask, cv::Point(center, center), cv::Size(max_radius, max_radius),
                -90, 0, sweep_angle, cv::Scalar(255), -1, cv::LINE_AA);

    // 4. 用這張扇形遮罩，把 Main Menu 貼到全黑的畫布上
    target_canvas.copyTo(canvas, mask);

    // 5. (加分點綴) 畫出那條正在掃描的「雷達前導線」
    // 計算前導線的 X, Y 座標 (要轉回弧度制)
    double rad = (-90 + sweep_angle) * M_PI / 180.0;
    int line_x = center + static_cast<int>(max_radius * cos(rad));
    int line_y = center + static_cast<int>(max_radius * sin(rad));
    
    // 畫出掃描線 (如果你喜歡別的顏色可以改)
    cv::line(canvas, cv::Point(center, center), cv::Point(line_x, line_y), 
             cv::Scalar(200, 200, 200), 10, cv::LINE_AA);

    return canvas;
}

static cv::Mat make_boot_shutter_canvas(const AppContext* ctx, double ratio)
{
    int center = CANVAS_SIZE / 2;

    // 1. 取得目標畫面 (Main Menu)
    cv::Mat target_canvas = draw_main_menu_canvas(ctx);

    // 2. 準備黑底畫布與遮罩
    cv::Mat canvas = cv::Mat::zeros(CANVAS_SIZE, CANVAS_SIZE, CV_8UC3);
    cv::Mat mask = cv::Mat::zeros(CANVAS_SIZE, CANVAS_SIZE, CV_8UC1);

    // 3. 計算目前的圓孔半徑 (加入 Ease-Out 讓放大有「煞車」的平滑感)
    double ease_ratio = 1.0 - pow(1.0 - ratio, 3); 
    // 最大半徑要乘上 0.7 左右，確保對角線也能完全展開
    // int max_radius = static_cast<int>(CANVAS_SIZE * 0.75); 
    int max_radius = static_cast<int>(CANVAS_SIZE * 0.5); 
    int current_radius = static_cast<int>(max_radius * ease_ratio);

    // 4. 在遮罩上畫出實心白圓 (挖洞)
    cv::circle(mask, cv::Point(center, center), current_radius, cv::Scalar(255), -1, cv::LINE_AA);

    // 5. 將 Main Menu 透過圓孔貼上去
    target_canvas.copyTo(canvas, mask);

    // 6. (加分點綴) 畫出圓孔邊緣的光圈環
    // 只有在還沒完全打開的時候才畫出邊緣，讓它有實體快門金屬環的感覺
    if (ratio < 0.95 && current_radius > 0) {
        // 畫外圈金屬環
        cv::circle(canvas, cv::Point(center, center), current_radius, cv::Scalar(200, 200, 200), 6, cv::LINE_AA);
        
        // 💡 加入保護機制：確保半徑夠大才畫內圈，絕對不讓它變成負數！
        if (current_radius >= 6) {
            cv::circle(canvas, cv::Point(center, center), current_radius - 6, cv::Scalar(100, 100, 100), 2, cv::LINE_AA);
        }
    }

    return canvas;
}

SystemState idle_handler(AppContext* ctx)
{
    ctx->display->off();

    if (ctx->stable_hall_count >= ON_THRESHOLD) {
        printf("[FSM] IDLE -> BOOT_ANIM, hall_count = %u\n", ctx->stable_hall_count);
        return STATE_BOOT_ANIM;
    }

    return STATE_IDLE;
}

SystemState boot_anim_handler(AppContext* ctx)
{
    static bool started = false;
    static long long start_time_ms = 0;

    if (!started) {
        started = true;
        start_time_ms = now_ms();
        printf("[FSM] BOOT_ANIM start\n");
    }

    if (ctx->stable_hall_count <= OFF_THRESHOLD) {
        printf("[FSM] BOOT_ANIM interrupted -> SHUTDOWN_ANIM, hall_count = %u\n",
               ctx->stable_hall_count);
        started = false;
        return STATE_SHUTDOWN_ANIM;
    }

    long long elapsed = now_ms() - start_time_ms;
    double ratio = static_cast<double>(elapsed) / static_cast<double>(BOOT_ANIM_MS);

    // ctx->display->show(make_boot_green_canvas(ratio));
    // ctx->display->show(make_boot_sweep_canvas(ctx, ratio));
    ctx->display->show(make_boot_shutter_canvas(ctx, ratio));

    if (elapsed >= BOOT_ANIM_MS) {
        printf("[FSM] BOOT_ANIM -> MAIN_MENU\n");
        started = false;
        ctx->menu_index = 0;
        ctx->blink_tick = 0;
        return STATE_MAIN_MENU;
    }

    return STATE_BOOT_ANIM;
}

SystemState shutdown_anim_handler(AppContext* ctx)
{
    printf("[FSM] SHUTDOWN_ANIM -> IDLE\n");
    ctx->display->off();
    return STATE_IDLE;
}

SystemState main_menu_handler(AppContext* ctx)
{
    if (ctx->stable_hall_count <= OFF_THRESHOLD) {
        printf("[FSM] MAIN_MENU -> SHUTDOWN_ANIM, hall_count = %u\n",
               ctx->stable_hall_count);
        return STATE_SHUTDOWN_ANIM;
    }

    int key = ctx->display->consumeKey();
    const int menu_count = 3;

    if (is_left_key(key)) {
        ctx->menu_index = (ctx->menu_index + menu_count - 1) % menu_count;
        printf("[MENU] selected index = %d\n", ctx->menu_index);
    } else if (is_right_key(key)) {
        ctx->menu_index = (ctx->menu_index + 1) % menu_count;
        printf("[MENU] selected index = %d\n", ctx->menu_index);
    } else if (is_confirm_key(key)) {
        if (ctx->menu_index == 0) {
            printf("[MENU] confirm CLOCK\n");
            return STATE_CLOCK;
        } else if (ctx->menu_index == 1) {
            printf("[MENU] confirm VIDEO\n");
            return STATE_VIDEO;
        } else {
            printf("[MENU] confirm GAME\n");
            return STATE_GAME;
        }
    }

    ctx->blink_tick++;
    ctx->display->show(draw_main_menu_canvas(ctx));

    return STATE_MAIN_MENU;
}

SystemState clock_handler(AppContext* ctx)
{
    if (ctx->stable_hall_count <= OFF_THRESHOLD) return STATE_SHUTDOWN_ANIM;
    if (is_back_key(ctx->display->consumeKey())) return STATE_MAIN_MENU;
    cv::Mat canvas = cv::Mat::zeros(CANVAS_SIZE, CANVAS_SIZE, CV_8UC3);
    cv::Point c(CANVAS_SIZE / 2, CANVAS_SIZE / 2);
    draw_clock_fallback(canvas, c, 340);
    ctx->display->show(canvas);
    return STATE_CLOCK;
}

static cv::VideoCapture g_video_cap;
SystemState video_handler(AppContext* ctx)
{
    if (ctx->stable_hall_count <= OFF_THRESHOLD) return STATE_SHUTDOWN_ANIM;

    int key = ctx->display->consumeKey();
    
    // 💡 確保影片已載入，並取得真實的影片數量
    ensure_videos_loaded();
    int num_videos = g_video_thumbnails.size();

    // 不管有沒有影片，都可以按 B 返回主選單
    if (is_back_key(key)) {
        return STATE_MAIN_MENU; 
    } 
    
    // 只有在有影片的時候，才處理左右切換邏輯
    if (num_videos > 0) {
        if (is_left_key(key)) {
            ctx->video_index = (ctx->video_index - 1 + num_videos) % num_videos;
            printf("[VIDEO] selected video = %d\n", ctx->video_index);
        } else if (is_right_key(key)) {
            ctx->video_index = (ctx->video_index + 1) % num_videos;
            printf("[VIDEO] selected video = %d\n", ctx->video_index);
        } else if (is_confirm_key(key)) {
            printf("[VIDEO] confirm play video %d\n", ctx->video_index);

            // 重新掃描一次路徑，確保抓到對應的檔案
            std::vector<cv::String> video_files;
            cv::glob("assets/video/*.mp4", video_files, false);
            std::sort(video_files.begin(), video_files.end());
            
            if (ctx->video_index < video_files.size()) {
                g_video_cap.open(video_files[ctx->video_index]); // 開啟影片
                if (g_video_cap.isOpened()) {
                    return STATE_VIDEO_PLAYING; // 切換到播放狀態！
                }
            }
        }
    }

    ctx->display->show(draw_video_menu_canvas(ctx));

    return STATE_VIDEO;
}

SystemState video_playing_handler(AppContext* ctx)
{
    static long long last_frame_time_ms = 0;
    static cv::Mat current_frame_cache;

    // 1. 硬體防護：停轉就關閉影片並關機
    if (ctx->stable_hall_count <= OFF_THRESHOLD) {
        g_video_cap.release();
        current_frame_cache.release();
        last_frame_time_ms = 0; // 離開時重置時間
        return STATE_SHUTDOWN_ANIM;
    }

    // 2. 按鍵偵測：按下 B 鍵或 Enter 鍵退出播放
    int key = ctx->display->consumeKey();
    if (is_back_key(key)) {
        g_video_cap.release();
        current_frame_cache.release(); 
        last_frame_time_ms = 0; // 離開時重置時間
        return STATE_VIDEO; 
    }

    long long now = now_ms();
    
    // 取得這部影片的原始 FPS
    double fps = g_video_cap.get(cv::CAP_PROP_FPS);
    if (fps <= 0) fps = 30.0;
    long long frame_interval_ms = static_cast<long long>(1000.0 / fps);

    // 3. 判斷時間：時間到了，才去讀取「下一幀」
    if (current_frame_cache.empty() || (now - last_frame_time_ms >= frame_interval_ms)) {
        cv::Mat frame;
        g_video_cap >> frame;

        // 4. 自動從頭循環播放 (Loop)
        if (frame.empty()) {
            g_video_cap.set(cv::CAP_PROP_POS_FRAMES, 0);
            g_video_cap >> frame;
            if (frame.empty()) {
                g_video_cap.release();
                current_frame_cache.release();
                last_frame_time_ms = 0;
                return STATE_VIDEO;
            }
            // 影片重播時，也重新對齊時間基準
            last_frame_time_ms = now;
        }

        // 5. 存入快取
        current_frame_cache = frame.clone(); 
        
        // 💡 6. 核心修正：完美消除時間飄移 (Time Drift)
        if (last_frame_time_ms == 0) {
            last_frame_time_ms = now; // 第一次播放，基準設為現在
        } else {
            // 只加上「理想間隔時間」，把上次延遲的扣打留給下一次！
            last_frame_time_ms += frame_interval_ms; 
            
            // 防呆機制：如果你的電腦突然卡頓了 1 秒鐘，避免它為了「還債」而瘋狂快轉
            if (now - last_frame_time_ms > frame_interval_ms * 2) {
                last_frame_time_ms = now;
            }
        }
    }

    // 7. 將目前的畫面丟給 POV 顯示
    ctx->display->show(current_frame_cache);

    return STATE_VIDEO_PLAYING;
}

SystemState game_handler(AppContext* ctx)
{
    if (ctx->stable_hall_count <= OFF_THRESHOLD) return STATE_SHUTDOWN_ANIM;
    if (is_back_key(ctx->display->consumeKey())) return STATE_MAIN_MENU;
    cv::Mat canvas = cv::Mat::zeros(CANVAS_SIZE, CANVAS_SIZE, CV_8UC3);
    cv::Point c(CANVAS_SIZE / 2, CANVAS_SIZE / 2);
    draw_gamepad_fallback(canvas, c, 140, cv::Scalar(80, 255, 80));
    ctx->display->show(canvas);
    return STATE_GAME;
}

typedef SystemState (*StateHandler)(AppContext* ctx);

StateHandler state_table[STATE_COUNT] = {
    idle_handler,
    boot_anim_handler,
    shutdown_anim_handler,
    main_menu_handler,
    clock_handler,
    video_handler,
    video_playing_handler,
    game_handler
};

// void timer_handler(int sig, siginfo_t *si, void *context)
// {
//     (void)sig;
//     (void)context;

//     AppContext* ctx = (AppContext*)si->si_value.sival_ptr;
//     if (!ctx || !ctx->display) return;

//     if (g_stop || ctx->display->shouldQuit()) {
//         return;
//     }

//     // 防呆檢查狀態機邊界
//     if (ctx->current_state < 0 || ctx->current_state >= STATE_COUNT) {
//         ctx->current_state = STATE_IDLE;
//     }

//     // 執行當前狀態的邏輯，並取得下一狀態（內部會將要畫的 Mat 給 ctx->render_canvas）
//     // ctx->current_state = state_table[ctx->current_state](ctx);
// }

int main()
{
    // ctrl + c
    signal(SIGINT, sigint_handler);

    VirtualPOVDisplay display;

    AppContext ctx = {};
    ctx.display = &display;
    ctx.menu_index = 0;
    ctx.blink_tick = 0;
    ctx.stable_hall_count = 0;
    ctx.current_state = STATE_IDLE; // 初始化狀態
    SystemState current_state = STATE_IDLE; // 初始化狀態

    printf("[SIM] FSM + icon menu test started.\n");
    printf("[SIM] Put icons here if you want external images:\n");
    printf("      assets/icon/clock.png\n");
    printf("      assets/icon/youtube.png\n");
    printf("      assets/icon/gamepad.png\n");
    printf("[SIM] ON_THRESHOLD = %d, OFF_THRESHOLD = %d\n", ON_THRESHOLD, OFF_THRESHOLD);

    //--------------------- POSIX Timer (60Hz) -------------------------//
    // struct sigaction sa;
    // struct sigevent sev;
    // timer_t timerid;
    // struct itimerspec its;

    // memset(&sa, 0, sizeof(sa));
    // sa.sa_flags = SA_SIGINFO;             // 開啟三參數模式以接收附加資料
    // sa.sa_sigaction = timer_handler;      // 綁定新的三參數 handler
    // sigaction(SIGALRM, &sa, NULL);

    // memset(&sev, 0, sizeof(sev));
    // sev.sigev_notify = SIGEV_SIGNAL;
    // sev.sigev_signo = SIGALRM;
    // sev.sigev_value.sival_ptr = &ctx;     // 將 ctx 指標綁定到 timer 中！

    // if (timer_create(CLOCK_REALTIME, &sev, &timerid) == -1) {
    //     perror("timer_create");
    //     return 1;
    // }

    // // 16667000 奈秒 = 16.667 毫秒 (約 60Hz)
    // its.it_value.tv_sec = 0;
    // its.it_value.tv_nsec = 16666667; 
    // its.it_interval.tv_sec = 0;
    // its.it_interval.tv_nsec = 16666667;

    // if (timer_settime(timerid, 0, &its, NULL) == -1) {
    //     perror("timer_settime");
    //     return 1;
    // }
    //------------------------------------------------------------------//

    while (!g_stop && !display.shouldQuit()) {
        ctx.stable_hall_count = display.readHallCount();

        if (current_state < 0 || current_state >= STATE_COUNT) {
            current_state = STATE_IDLE;
        }

        display.setStatus(stateName(current_state), ctx.stable_hall_count);
        current_state = state_table[current_state](&ctx);

        usleep(LOOP_DELAY_US);
    }

    printf("[SIM] closing virtual device...\n");
    
    // 結束前刪除計時器以免繼續觸發
    // timer_delete(timerid);

    display.off();
    usleep(50000);
    cv::destroyAllWindows();

    return 0;
}