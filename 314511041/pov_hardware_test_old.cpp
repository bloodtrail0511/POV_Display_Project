#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <math.h>

#include <opencv2/opencv.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

// ============================================================
// Hardware / driver format
// ============================================================
#define NUM_LEDS 40
#define HALF_LEDS 20
#define END_LEN 5
#define DEGREE_RESOLUTION 360
#define SPI_BUF_SIZE (4 + (NUM_LEDS * 4) + END_LEN)
#define FRAME_SIZE (DEGREE_RESOLUTION * SPI_BUF_SIZE)

// APA102 global brightness: 1~31. Debug 時建議先低亮度，避免低電壓。
#define APA102_BRIGHTNESS 5
#define PIXEL_BRIGHTNESS_SCALE 0.25 // 圖片 RGB 值的亮度倍率，會把圖片本身的 RGB 數值壓暗。
#define SATURATION_SCALE 2.5 // 圖片的飽和度。
#define VALUE_SCALE 0.55 // 整體明度。
#define WHITE_CUTOFF 255

// ============================================================
// Geometry setting
// ============================================================
// 這裡的意思是：實體每半邊只有 20 顆 LED，
// 但 A/B 兩側半格錯開後，同一個畫面半徑方向可以形成 40 階取樣。
#define VIRTUAL_RADIAL_RES 40

// 若你的實體編號不同，只需要改這兩個 mapping。
// 預設假設：
//   LED 0~19  = B 側，且 B 側實體順序與半徑 j 相反
//   LED 20~39 = A 側，且 A 側實體順序與半徑 j 相同
static inline int led_index_A(int j) {
    return HALF_LEDS + j;       // 20, 21, ... 39
}

static inline int led_index_B(int j) {
    return HALF_LEDS - 1 - j;   // 19, 18, ... 0
}

// ============================================================
// Test mode
// ============================================================
// 0: OpenCV 畫一條 0/180 度紅色直徑線
// 1: OpenCV 畫四條不同顏色直徑線，測角度定位
// 2: OpenCV 畫多條細同心圓，測半徑解析度
// 3: OpenCV 畫 0 度紅線 + 旁邊一條很近的紅線，測解析度是否變細
// 4: OpenCV 畫斜線 X，測整體座標是否正確
// 5: 左上角紅色方形
// 6: 左上角紅色空心方形 + 中心十字線，方便確認方向
// 7: 讀取圖片檔，縮放後顯示
// 8: 刷新率測試：每一幀重新畫移動方塊 → 轉 slice → 寫入 driver，並印出實際 FPS
#define TEST_MODE 1

// TEST_MODE 8 用：目標 user-space 更新率。
// 這不是馬達物理刷新率；馬達物理刷新率 = 1 / Hall 一圈時間。
// 例如 Hall diff = 100ms，物理刷新率約 10Hz。
#define TARGET_UPDATE_FPS 10
#define FPS_PRINT_INTERVAL_SEC 1.0

// OpenCV canvas size. 建議用偶數，中心比較好算。
#define CANVAS_SIZE 800

static uint8_t test_frame[DEGREE_RESOLUTION][SPI_BUF_SIZE];

// ============================================================
// Sampling LUT：沿用 pov_pc_sim_v2 的寫法
// lut_x/lut_y[degree][physical_led_index]
// physical_led_index 0~19 = B 側，20~39 = A 側
// ============================================================
static int lut_x[DEGREE_RESOLUTION][NUM_LEDS];
static int lut_y[DEGREE_RESOLUTION][NUM_LEDS];


static inline int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// 初始化採樣查表：這段刻意跟 pov_pc_sim_v2 的 init_sampling_lut() 保持同一套邏輯。
// 0 度在正下方，角度增加方向為：下 -> 左 -> 上 -> 右。
// A 側：LED 20~39，取 angle_A。
// B 側：LED 0~19，取 angle_B = angle_A + PI，且順序反向存入。
void init_sampling_lut(int side_len) {
    int center = side_len / 2;
    float r_step = (float)center / HALF_LEDS;

    for (int i = 0; i < DEGREE_RESOLUTION; i++) {
        float angle_A = i * 2.0f * (float)M_PI / DEGREE_RESOLUTION;
        float angle_B = angle_A + (float)M_PI;

        for (int j = 0; j < HALF_LEDS; j++) {
            // Strip A：半徑位置 0.25, 1.25, 2.25 ...
            float d_A = r_step * j + (r_step * 0.25f);
            int x_A = center - d_A * sin(angle_A);
            int y_A = center + d_A * cos(angle_A);

            // Strip B：交錯半格，半徑位置 0.75, 1.75, 2.75 ...
            float d_B = r_step * j + (r_step * 0.75f);
            int x_B = center - d_B * sin(angle_B);
            int y_B = center + d_B * cos(angle_B);

            // 這兩行就是你 PC sim 的核心 mapping：
            // B 側反向塞到 0~19，A 側塞到 20~39。
            lut_x[i][HALF_LEDS - j - 1] = clamp_int(x_B, 0, side_len - 1);
            lut_y[i][HALF_LEDS - j - 1] = clamp_int(y_B, 0, side_len - 1);

            lut_x[i][j + HALF_LEDS] = clamp_int(x_A, 0, side_len - 1);
            lut_y[i][j + HALF_LEDS] = clamp_int(y_A, 0, side_len - 1);
        }
    }
}

cv::Mat preprocess_image_for_pov(const cv::Mat &src) {
    cv::Mat img = src.clone();

    // 1. 先把接近白色的背景壓暗，避免整張圖變成白光
    for (int y = 0; y < img.rows; y++) {
        for (int x = 0; x < img.cols; x++) {
            cv::Vec3b &p = img.at<cv::Vec3b>(y, x);

            int b = p[0];
            int g = p[1];
            int r = p[2];

            if (r > WHITE_CUTOFF && g > WHITE_CUTOFF && b > WHITE_CUTOFF) {
                p = cv::Vec3b(0, 0, 0);
            }
        }
    }

    // 2. 轉 HSV，提高飽和度，降低亮度
    cv::Mat hsv;
    cv::cvtColor(img, hsv, cv::COLOR_BGR2HSV);

    for (int y = 0; y < hsv.rows; y++) {
        for (int x = 0; x < hsv.cols; x++) {
            cv::Vec3b &p = hsv.at<cv::Vec3b>(y, x);

            int h = p[0];
            int s = p[1];
            int v = p[2];

            s = clamp_int((int)(s * SATURATION_SCALE), 0, 255);
            v = clamp_int((int)(v * VALUE_SCALE), 0, 255);

            p[0] = h;
            p[1] = s;
            p[2] = v;
        }
    }

    cv::Mat out;
    cv::cvtColor(hsv, out, cv::COLOR_HSV2BGR);
    return out;
}

static inline int wrap_degree(int deg) {
    deg %= DEGREE_RESOLUTION;
    if (deg < 0) deg += DEGREE_RESOLUTION;
    return deg;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}


void init_frame_structure(void) {
    memset(test_frame, 0, FRAME_SIZE);

    for (int deg = 0; deg < DEGREE_RESOLUTION; deg++) {
        // APA102 start frame
        test_frame[deg][0] = 0x00;
        test_frame[deg][1] = 0x00;
        test_frame[deg][2] = 0x00;
        test_frame[deg][3] = 0x00;

        // LED data: [global brightness][B][G][R]
        for (int led = 0; led < NUM_LEDS; led++) {
            int base = 4 + led * 4;
            test_frame[deg][base + 0] = 0xE0 | APA102_BRIGHTNESS;
            test_frame[deg][base + 1] = 0x00;
            test_frame[deg][base + 2] = 0x00;
            test_frame[deg][base + 3] = 0x00;
        }

        // APA102 end frame
        for (int j = 0; j < END_LEN; j++) {
            test_frame[deg][SPI_BUF_SIZE - END_LEN + j] = 0xFF;
        }
    }
}
static inline uint8_t scale_color(uint8_t v) {
    int out = (int)(v * PIXEL_BRIGHTNESS_SCALE);
    return (uint8_t)clamp_int(out, 0, 255);
}
void set_led_physical(int degree, int led_idx, uint8_t r, uint8_t g, uint8_t b) {
    degree = wrap_degree(degree);
    if (led_idx < 0 || led_idx >= NUM_LEDS) return;

    int base = 4 + led_idx * 4;
    test_frame[degree][base + 0] = 0xE0 | APA102_BRIGHTNESS;
    // test_frame[degree][base + 1] = b;
    // test_frame[degree][base + 2] = g;
    // test_frame[degree][base + 3] = r;
    test_frame[degree][base + 1] = scale_color(b);
    test_frame[degree][base + 2] = scale_color(g);
    test_frame[degree][base + 3] = scale_color(r);
}

// 0 度定義沿用你的 PC sim：0 度在正下方，角度增加方向為 下 -> 左 -> 上 -> 右。
static cv::Point point_from_angle_radius(int center, double angle_rad, double radius) {
    int x = (int)lrint(center - radius * sin(angle_rad));
    int y = (int)lrint(center + radius * cos(angle_rad));
    return cv::Point(x, y);
}

void draw_test_canvas(cv::Mat &canvas, int argc, char** argv) {
    canvas = cv::Mat::zeros(CANVAS_SIZE, CANVAS_SIZE, CV_8UC3);

    const int center = CANVAS_SIZE / 2;
    const int radius = center - 8;

#if TEST_MODE == 0
    // 一條紅色直徑線：OpenCV 先畫圖，再由 LUT 取樣成 slice。
    // 如果 A/B 半格交錯正確，0 度方向同一半邊會由 slice 0 與 slice 180 補出更密的點。
    cv::line(canvas,
             cv::Point(center, center - radius),
             cv::Point(center, center + radius),
             cv::Scalar(0, 0, 255), 3, cv::LINE_AA);

#elif TEST_MODE == 1
    // 四條直徑線：紅 0/180、綠 90/270、藍 45/225、黃 135/315
    cv::line(canvas, cv::Point(center, center - radius), cv::Point(center, center + radius), cv::Scalar(0, 0, 255), 3, cv::LINE_AA);
    cv::line(canvas, cv::Point(center - radius, center), cv::Point(center + radius, center), cv::Scalar(0, 255, 0), 3, cv::LINE_AA);

    cv::Point p45a = point_from_angle_radius(center, 45.0  * M_PI / 180.0, radius);
    cv::Point p45b = point_from_angle_radius(center, 225.0 * M_PI / 180.0, radius);
    cv::line(canvas, p45a, p45b, cv::Scalar(255, 0, 0), 3, cv::LINE_AA);

    cv::Point p135a = point_from_angle_radius(center, 135.0 * M_PI / 180.0, radius);
    cv::Point p135b = point_from_angle_radius(center, 315.0 * M_PI / 180.0, radius);
    cv::line(canvas, p135a, p135b, cv::Scalar(0, 255, 255), 3, cv::LINE_AA);

#elif TEST_MODE == 2
    // 多條細同心圓：用來看半徑方向的點是不是變密。
    for (int r = 20; r < radius; r += 20) {
        cv::circle(canvas, cv::Point(center, center), r, cv::Scalar(0, 0, 255), 2, cv::LINE_AA);
    }

#elif TEST_MODE == 3
    // 兩條距離很近的直線：測解析度與是否糊成一條。
    cv::line(canvas, cv::Point(center - 4, center - radius), cv::Point(center - 4, center + radius), cv::Scalar(0, 0, 255), 2, cv::LINE_AA);
    cv::line(canvas, cv::Point(center + 4, center - radius), cv::Point(center + 4, center + radius), cv::Scalar(0, 0, 255), 2, cv::LINE_AA);

#elif TEST_MODE == 4
    // X 形斜線，測整體座標方向有沒有左右/上下顛倒。
    cv::line(canvas, cv::Point(center - radius, center - radius), cv::Point(center + radius, center + radius), cv::Scalar(0, 0, 255), 3, cv::LINE_AA);
    cv::line(canvas, cv::Point(center + radius, center - radius), cv::Point(center - radius, center + radius), cv::Scalar(0, 255, 0), 3, cv::LINE_AA);
#elif TEST_MODE == 5
    // 左上角實心方形。位置刻意不要靠太邊，避免超出圓形掃描範圍。
    cv::rectangle(canvas,
                  cv::Point(CANVAS_SIZE * 0.20, CANVAS_SIZE * 0.20),
                  cv::Point(CANVAS_SIZE * 0.38, CANVAS_SIZE * 0.38),
                  cv::Scalar(0, 0, 255),
                  -1,
                  cv::LINE_AA);

#elif TEST_MODE == 6
    // 左上角空心方形 + 中心十字，方便確認方位有沒有旋轉/鏡像。
    cv::rectangle(canvas,
                  cv::Point(CANVAS_SIZE * 0.20, CANVAS_SIZE * 0.20),
                  cv::Point(CANVAS_SIZE * 0.42, CANVAS_SIZE * 0.42),
                  cv::Scalar(0, 0, 255),
                  4,
                  cv::LINE_AA);

    cv::line(canvas, cv::Point(center - 100, center), cv::Point(center + 100, center), cv::Scalar(0, 255, 255), 2, cv::LINE_AA);
    cv::line(canvas, cv::Point(center, center - 100), cv::Point(center, center + 100), cv::Scalar(0, 255, 255), 2, cv::LINE_AA);

#elif TEST_MODE == 7
    if (argc < 2) {
        fprintf(stderr, "TEST_MODE=7 需要圖片路徑，例如：sudo ./pov_hardware_test image.png\n");
        return;
    }

    cv::Mat img = cv::imread(argv[1], cv::IMREAD_COLOR);
    if (img.empty()) {
        fprintf(stderr, "無法讀取圖片：%s\n", argv[1]);
        return;
    }
    img = preprocess_image_for_pov(img);

    // 等比例縮放到正方形中間，黑邊補齊。
    double scale = std::min((double)CANVAS_SIZE / img.cols, (double)CANVAS_SIZE / img.rows);
    int new_w = (int)(img.cols * scale);
    int new_h = (int)(img.rows * scale);

    cv::Mat resized;
    cv::resize(img, resized, cv::Size(new_w, new_h), 0, 0, cv::INTER_AREA);

    int x0 = (CANVAS_SIZE - new_w) / 2;
    int y0 = (CANVAS_SIZE - new_h) / 2;
    resized.copyTo(canvas(cv::Rect(x0, y0, new_w, new_h)));

#elif TEST_MODE == 8
    // 靜態初始化用；真正動態畫面會在 run_refresh_rate_test() 裡每幀重畫。
    cv::rectangle(canvas,
                  cv::Point(CANVAS_SIZE * 0.20, CANVAS_SIZE * 0.20),
                  cv::Point(CANVAS_SIZE * 0.38, CANVAS_SIZE * 0.38),
                  cv::Scalar(0, 0, 255),
                  -1,
                  cv::LINE_AA);
#else
#error "Unknown TEST_MODE"
#endif

    // 中心點標記，方便確認畫布中心。
    cv::circle(canvas, cv::Point(center, center), 3, cv::Scalar(255, 255, 255), -1, cv::LINE_AA);
}

cv::Vec3b sample_bgr_nearest(const cv::Mat &img, int x, int y) {
    x = clamp_int(x, 0, img.cols - 1);
    y = clamp_int(y, 0, img.rows - 1);
    return img.at<cv::Vec3b>(y, x);
}

void convert_canvas_to_hardware_frame(const cv::Mat &canvas) {
    init_frame_structure();

    // 使用 pov_pc_sim_v2 同款 LUT 轉換：
    // 先由 init_sampling_lut() 決定每個 degree / LED 對應 canvas 哪個 pixel，
    // 再把該 pixel 的 BGR 塞進硬體 frame。
    for (int deg = 0; deg < DEGREE_RESOLUTION; deg++) {
        for (int led = 0; led < NUM_LEDS; led++) {
            int x = lut_x[deg][led];
            int y = lut_y[deg][led];

            cv::Vec3b c = sample_bgr_nearest(canvas, x, y);  // OpenCV: B, G, R
            set_led_physical(deg, led, c[2], c[1], c[0]);    // hardware helper: R, G, B
        }
    }
}

void save_debug_images(const cv::Mat &canvas) {
    cv::imwrite("/tmp/pov_test_canvas.png", canvas);

    // 這張 preview 也用同一份 LUT 反畫回去，方便確認「LUT 取樣後」資料長相。
    cv::Mat sim = cv::Mat::zeros(CANVAS_SIZE, CANVAS_SIZE, CV_8UC3);

    for (int deg = 0; deg < DEGREE_RESOLUTION; deg++) {
        for (int led = 0; led < NUM_LEDS; led++) {
            int x = lut_x[deg][led];
            int y = lut_y[deg][led];
            int base = 4 + led * 4;

            cv::Scalar color(
                test_frame[deg][base + 1],  // B
                test_frame[deg][base + 2],  // G
                test_frame[deg][base + 3]   // R
            );
            cv::circle(sim, cv::Point(x, y), 2, color, -1, cv::LINE_AA);
        }
    }

    cv::imwrite("/tmp/pov_test_sampled_preview.png", sim);
}


void draw_refresh_rate_canvas(cv::Mat &canvas, int frame_idx) {
    canvas = cv::Mat::zeros(CANVAS_SIZE, CANVAS_SIZE, CV_8UC3);

    const int center = CANVAS_SIZE / 2;
    const int margin = CANVAS_SIZE / 8;
    const int box = CANVAS_SIZE / 8;
    const int travel = CANVAS_SIZE - 2 * margin - box;

    // 讓方塊在上方左右來回移動。若更新率不夠，會明顯跳動。
    int period_frames = TARGET_UPDATE_FPS * 2;
    if (period_frames < 2) period_frames = 2;

    int phase = frame_idx % (2 * period_frames);
    double t = (phase < period_frames)
        ? (double)phase / (double)period_frames
        : (double)(2 * period_frames - phase) / (double)period_frames;

    int x = margin + (int)lrint(travel * t);
    int y = margin;

    // 每幀顏色循環，方便看出 driver 是否真的換到新 frame。
    cv::Scalar color;
    switch ((frame_idx / 10) % 3) {
        case 0: color = cv::Scalar(0, 0, 255); break;   // red
        case 1: color = cv::Scalar(0, 255, 0); break;   // green
        default: color = cv::Scalar(255, 0, 0); break;  // blue
    }

    cv::rectangle(canvas, cv::Point(x, y), cv::Point(x + box, y + box), color, -1, cv::LINE_AA);

    // 中心十字作為固定參考點，判斷畫面是否被整體拖影或偏移。
    cv::line(canvas, cv::Point(center - 70, center), cv::Point(center + 70, center), cv::Scalar(0, 255, 255), 2, cv::LINE_AA);
    cv::line(canvas, cv::Point(center, center - 70), cv::Point(center, center + 70), cv::Scalar(0, 255, 255), 2, cv::LINE_AA);

    // 右下角閃爍小點：每幀切換，低刷新率時會很明顯。
    if (frame_idx % 2 == 0) {
        cv::circle(canvas, cv::Point(CANVAS_SIZE - margin, CANVAS_SIZE - margin), 24, cv::Scalar(255, 255, 255), -1, cv::LINE_AA);
    }
}

int run_refresh_rate_test(const char *device_path) {
    int fd = open(device_path, O_WRONLY);
    if (fd < 0) {
        perror("Cannot open /dev/pov_display");
        return -1;
    }

    printf("Refresh-rate test mode started. TARGET_UPDATE_FPS = %d\\n", TARGET_UPDATE_FPS);
    printf("This measures user-space frame update rate: OpenCV draw -> slice conversion -> write().\\n");
    printf("Physical POV refresh rate still depends on motor/Hall period. Example: 100ms per round = 10Hz.\\n");

    init_sampling_lut(CANVAS_SIZE);

    cv::Mat canvas;
    int frame_idx = 0;

    double last_print = now_sec();
    double last_frame_start = now_sec();
    int frames_since_print = 0;

    while (1) {
        double frame_start = now_sec();

        draw_refresh_rate_canvas(canvas, frame_idx);
        convert_canvas_to_hardware_frame(canvas);

        ssize_t bytes_written = write(fd, test_frame, FRAME_SIZE);
        if (bytes_written != FRAME_SIZE) {
            perror("write failed or wrong length");
            close(fd);
            return -1;
        }

        frame_idx++;
        frames_since_print++;

        double now = now_sec();
        if (now - last_print >= FPS_PRINT_INTERVAL_SEC) {
            double fps = frames_since_print / (now - last_print);
            printf("actual update FPS = %.2f, frame = %d, frame interval ~= %.2f ms\\n",
                   fps, frame_idx, 1000.0 / fps);
            frames_since_print = 0;
            last_print = now;
        }

        // 控制目標更新率。若 draw+convert+write 已經超過目標時間，就不 sleep。
        double target_dt = 1.0 / (double)TARGET_UPDATE_FPS;
        double elapsed = now_sec() - frame_start;
        if (elapsed < target_dt) {
            usleep((useconds_t)((target_dt - elapsed) * 1000000.0));
        }

        last_frame_start = frame_start;
    }

    close(fd);
    return 0;
}


int main(int argc, char** argv) {
    const char *device_path = "/dev/pov_display";

#if TEST_MODE == 8
    return run_refresh_rate_test(device_path);
#endif

    init_sampling_lut(CANVAS_SIZE);

    cv::Mat canvas;
    draw_test_canvas(canvas, argc, argv);
    convert_canvas_to_hardware_frame(canvas);
    // save_debug_images(canvas);

    printf("OpenCV test canvas generated.\n");
    // printf("Saved: /tmp/pov_test_canvas.png\n");
    // printf("Saved: /tmp/pov_test_sampled_preview.png\n");
    printf("Writing frame to %s ...\n", device_path);

    int fd = open(device_path, O_WRONLY);
    if (fd < 0) {
        perror("Cannot open /dev/pov_display");
        return -1;
    }

    // 靜態圖案：重複寫入同一份 frame，讓 driver 在 Hall sync 時有資料可 swap。
    // 若你想只寫一次，也可以把 while 改成單次 write。
    while (1) {
        ssize_t bytes_written = write(fd, test_frame, FRAME_SIZE);
        if (bytes_written != FRAME_SIZE) {
            perror("write failed or wrong length");
            break;
        }
        usleep(100000); // 100 ms，不需要 60fps；這是靜態測試圖。
    }

    close(fd);
    return 0;
}
