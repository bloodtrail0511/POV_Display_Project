#include <stdio.h>      // printf(), snprintf()
#include <stdint.h>     // uint8_t：固定 8-bit 的無號整數，適合存放影像 RGB/BGR 顏色值
#include <math.h>       // sin(), cos(), M_PI 等數學函式
#include <time.h>       // time(), localtime_s()/localtime()：取得目前系統時間

#include <opencv2/opencv.hpp>   // OpenCV 主要功能
#include <opencv2/highgui.hpp>  // 視窗顯示：namedWindow(), imshow(), waitKey()
#include <opencv2/imgproc.hpp>  // 影像繪圖：circle(), line(), putText()

// POV 模擬時，把一圈切成 360 個角度切片。
// 可以理解成旋轉手臂每轉一圈，取樣 360 個角度位置。
#define NUM_SLICES 360

// 單側 LED 數量。
// 程式中會模擬一條穿過圓心的 LED 臂，因此實際取樣點數為 LED_NUM * 2。
#define LED_NUM 24

// 避免 Windows 標頭或其他函式庫已經定義 MAX/MIN，先取消後重新定義。
#undef MAX
#undef MIN
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// POV_Frame 用來儲存一整圈 POV 顯示所需的顏色資料。
// data[角度切片][LED 位置][BGR 顏色通道]
// 注意：OpenCV 預設顏色順序是 BGR，不是 RGB。
typedef struct POV_Frame {
    uint8_t data[NUM_SLICES][LED_NUM * 2][3];
} POV_Frame;

// 查表陣列 Lookup Table, LUT。
// lut_x[i][j], lut_y[i][j] 代表：
// 第 i 個角度切片、第 j 顆 LED，應該從原始方形畫布的哪一個像素座標取樣。
int lut_x[NUM_SLICES][LED_NUM * 2];
int lut_y[NUM_SLICES][LED_NUM * 2];

// 建立取樣查表。
// 這個函式會預先計算每個旋轉角度、每顆 LED 對應到原始畫布上的座標。
// 好處是主迴圈中不需要反覆做大量三角函式運算，可以提高效率。
void init_sampling_lut(int side_len) {
    int center = side_len / 2;                 // 方形畫布中心點
    float r_step = (float)center / LED_NUM;    // 每顆 LED 在半徑方向上的間距

    for (int i = 0; i < NUM_SLICES; i++) {
        // angle_A 是目前切片的角度。
        // angle_B 是反方向，也就是 angle_A + 180 度。
        // 因為這裡模擬的是一條穿過圓心的 LED 臂，所以要同時處理 A、B 兩側。
        float angle_A = (float)(i * 2.0 * M_PI / NUM_SLICES);
        float angle_B = angle_A + (float)M_PI;

        for (int j = 0; j < LED_NUM; j++) {
            // A 側 LED 距離圓心的位置。
            // 加上 0.25 個間距是為了讓取樣點不要剛好落在格線或中心上。
            float d_A = r_step * j + (r_step * 0.25f);
            int x_A = (int)(center - d_A * sin(angle_A));
            int y_A = (int)(center + d_A * cos(angle_A));

            // B 側 LED 距離圓心的位置。
            // 加上 0.75 個間距，讓 A/B 兩側取樣位置略微錯開，模擬更連續的 LED 分佈。
            float d_B = r_step * j + (r_step * 0.75f);
            int x_B = (int)(center - d_B * sin(angle_B));
            int y_B = (int)(center + d_B * cos(angle_B));

            // B 側資料反向存入前半段，讓 LED index 順序對應實體 LED 排列。
            // MAX/MIN 用來限制座標不能超出畫布範圍。
            lut_x[i][LED_NUM - j - 1] = MAX(0, MIN(x_B, side_len - 1));
            lut_y[i][LED_NUM - j - 1] = MAX(0, MIN(y_B, side_len - 1));

            // A 側資料存入後半段。
            lut_x[i][j + LED_NUM] = MAX(0, MIN(x_A, side_len - 1));
            lut_y[i][j + LED_NUM] = MAX(0, MIN(y_A, side_len - 1));
        }
    }
}

// 將原始方形畫布轉換成 POV 顯示緩衝區。
// frame_cropped：完整的 2D 時鐘畫面。
// buffer：轉換後的 POV 資料，格式是「角度切片 × LED 位置 × 顏色」。
void convert_to_pov_buffer(const cv::Mat& frame_cropped, POV_Frame* buffer) {
    const uint8_t* raw_pixels = frame_cropped.data; // 直接取得 OpenCV Mat 的原始像素資料
    int step = (int)frame_cropped.step;             // 每一列影像佔用的 byte 數
    int channels = frame_cropped.channels();        // 顏色通道數，CV_8UC3 通常為 3，也就是 BGR

    for (int i = 0; i < NUM_SLICES; i++) {
        for (int j = 0; j < LED_NUM * 2; j++) {
            // 透過 LUT 找出目前角度與 LED 位置應該取樣的原始影像座標。
            // offset 是該像素在 raw_pixels 一維陣列中的 byte 位置。
            int offset = lut_y[i][j] * step + lut_x[i][j] * channels;

            // 複製 B、G、R 三個通道到 POV buffer。
            buffer->data[i][j][0] = raw_pixels[offset + 0];
            buffer->data[i][j][1] = raw_pixels[offset + 1];
            buffer->data[i][j][2] = raw_pixels[offset + 2];
        }
    }
}

// 根據 POV buffer 重建視覺化結果，用 OpenCV 畫出模擬的旋轉顯示效果。
// display_canvas 是輸出的模擬畫布，也就是視窗「2. POV Simulator」看到的畫面。
void simulate_pov_display(const POV_Frame& buffer, cv::Mat& display_canvas) {
    display_canvas = cv::Scalar(0, 0, 0); // 每次重畫前先清成黑色背景

    int sim_w = display_canvas.cols;
    int sim_h = display_canvas.rows;
    int sim_center = sim_w / 2;

    // 模擬視窗中每顆 LED 的半徑間距。
    // 這裡不一定等於原始畫布的 r_step，只是控制視覺化大小。
    float r_step_sim = 11.0f;
    int dot_size = MAX(1, (int)(r_step_sim * 0.2f));

    for (int i = 0; i < NUM_SLICES; i++) {
        // A/B 兩側的實體角度。
        float angle_phys_A = (float)(i * 2.0 * M_PI / NUM_SLICES);
        float angle_phys_B = angle_phys_A + (float)M_PI;

        for (int j = 0; j < LED_NUM; j++) {
            // 繪製 A 側 LED 點。
            float d_A = r_step_sim * j + (r_step_sim * 0.25f);
            int draw_x_A = (int)(sim_center - d_A * sin(angle_phys_A));
            int draw_y_A = (int)(sim_center + d_A * cos(angle_phys_A));

            // A 側資料存在 buffer 的後半段。
            cv::Scalar color_A(
                buffer.data[i][j + LED_NUM][0],
                buffer.data[i][j + LED_NUM][1],
                buffer.data[i][j + LED_NUM][2]
            );
            cv::circle(display_canvas, cv::Point(draw_x_A, draw_y_A), dot_size, color_A, -1, cv::LINE_8);

            // 繪製 B 側 LED 點。
            float d_B = r_step_sim * j + (r_step_sim * 0.75f);
            int draw_x_B = (int)(sim_center - d_B * sin(angle_phys_B));
            int draw_y_B = (int)(sim_center + d_B * cos(angle_phys_B));

            // B 側資料存在 buffer 的前半段，並且 index 是反向排列。
            cv::Scalar color_B(
                buffer.data[i][LED_NUM - j - 1][0],
                buffer.data[i][LED_NUM - j - 1][1],
                buffer.data[i][LED_NUM - j - 1][2]
            );
            cv::circle(display_canvas, cv::Point(draw_x_B, draw_y_B), dot_size, color_B, -1, cv::LINE_8);
        }
    }

    // 畫出底部紅點，作為視覺上的角度參考點。
    // 在真實 POV 裝置中，這類參考通常可對應 Hall sensor 的 0 度位置。
    cv::circle(display_canvas, cv::Point(sim_center, sim_h - 10), 5, cv::Scalar(0, 0, 255), -1);
}

// 繪製時鐘的固定背景：黑底、外圈、中心點，以及 1 到 12 的數字。
void draw_clock_numbers(cv::Mat& canvas) {
    canvas = cv::Scalar(0, 0, 0); // 清空畫布為黑色

    int center_x = canvas.cols / 2;
    int center_y = canvas.rows / 2;
    int number_radius = (int)(canvas.cols * 0.39); // 數字所在圓周的半徑

    // 畫時鐘外圈與中心基準點。
    cv::circle(canvas, cv::Point(center_x, center_y), number_radius + 55, cv::Scalar(35, 35, 35), 6, cv::LINE_AA);
    cv::circle(canvas, cv::Point(center_x, center_y), 6, cv::Scalar(80, 80, 80), -1, cv::LINE_AA);

    for (int hour = 1; hour <= 12; hour++) {
        // 一個小時對應 30 度，12 小時剛好 360 度。
        // 這裡採用時鐘座標：12 點在上方，3 點在右方。
        double angle_deg = hour * 30.0;
        double angle_rad = angle_deg * M_PI / 180.0;

        // sin 控制 x，cos 控制 y。
        // y 使用減號是因為影像座標的 y 軸向下為正。
        int x = (int)(center_x + number_radius * sin(angle_rad));
        int y = (int)(center_y - number_radius * cos(angle_rad));

        char label[3];
        snprintf(label, sizeof(label), "%d", hour); // 將 hour 轉成字串

        // 10、11、12 是兩位數，因此字體稍微縮小，避免太寬。
        double font_scale = (hour >= 10) ? 2.1 : 2.5;
        int thickness = 6;
        int baseline = 0;

        // 取得文字尺寸，方便把文字中心對齊到計算出的座標。
        cv::Size text_size = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX, font_scale, thickness, &baseline);

        cv::Point text_origin(
            x - text_size.width / 2,
            y + text_size.height / 2
        );

        // 先畫一層深色粗字，作為外框或陰影，增加數字在 POV 模擬中的辨識度。
        cv::putText(
            canvas,
            label,
            text_origin,
            cv::FONT_HERSHEY_SIMPLEX,
            font_scale,
            cv::Scalar(30, 30, 30),
            thickness + 4,
            cv::LINE_AA
        );

        // 再畫白色數字本體。
        cv::putText(
            canvas,
            label,
            text_origin,
            cv::FONT_HERSHEY_SIMPLEX,
            font_scale,
            cv::Scalar(255, 255, 255),
            thickness,
            cv::LINE_AA
        );
    }
}

// 根據目前系統時間繪製時針、分針、秒針。
void draw_clock_hands(cv::Mat& canvas) {
    // 取得目前時間。
    time_t now = time(NULL);
    struct tm local_tm;

    // Windows 使用 localtime_s；Linux/macOS 使用 localtime。
    // 這樣寫可以讓程式在不同作業系統上都能編譯。
#ifdef _WIN32
    localtime_s(&local_tm, &now);
#else
    local_tm = *localtime(&now);
#endif

    int center_x = canvas.cols / 2;
    int center_y = canvas.rows / 2;
    int outer_radius = (int)(canvas.cols * 0.39);

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
        int x = (int)(center_x + outer_radius * length_scale * sin(angle_rad));
        int y = (int)(center_y - outer_radius * length_scale * cos(angle_rad));
        return cv::Point(x, y);
    };

    cv::Point center(center_x, center_y);
    cv::Point hour_tip = endpoint(hour_angle, 0.50);    // 時針較短
    cv::Point minute_tip = endpoint(minute_angle, 0.72); // 分針中等長度
    cv::Point second_tip = endpoint(second_angle, 0.84); // 秒針較長

    // 依序畫出時針、分針、秒針。
    cv::line(canvas, center, hour_tip, cv::Scalar(255, 255, 255), 18, cv::LINE_AA);
    cv::line(canvas, center, minute_tip, cv::Scalar(220, 220, 220), 10, cv::LINE_AA);
    cv::line(canvas, center, second_tip, cv::Scalar(80, 80, 255), 4, cv::LINE_AA);

    // 畫中心圓點，蓋住三根指針交會處，使畫面較整潔。
    cv::circle(canvas, center, 14, cv::Scalar(230, 230, 230), -1, cv::LINE_AA);
    cv::circle(canvas, center, 6, cv::Scalar(80, 80, 255), -1, cv::LINE_AA);
}

int main() {
    // 原始時鐘畫布大小。畫布越大，取樣後的 POV 畫面越細緻。
    int side_len = 1200;

    // 程式開始時先建立取樣查表。
    // 因為 side_len 固定，所以 LUT 只需要建立一次。
    init_sampling_lut(side_len);

    // 儲存轉換後的 POV 顯示資料。
    POV_Frame pov_buffer;

    // game_canvas 是完整的 2D 時鐘畫面，也就是視窗「1. Clock Canvas」。
    cv::Mat game_canvas = cv::Mat::zeros(side_len, side_len, CV_8UC3);

    // simulation_canvas 是重建後的 POV 模擬畫面，也就是視窗「2. POV Simulator」。
    float r_step_sim = 11.0f;
    int sim_window_size = (int)(LED_NUM * r_step_sim * 2) + 40;
    cv::Mat simulation_canvas = cv::Mat::zeros(sim_window_size, sim_window_size, CV_8UC3);

    // 建立兩個 OpenCV 視窗。
    cv::namedWindow("1. Clock Canvas", cv::WINDOW_AUTOSIZE);
    cv::namedWindow("2. POV Simulator", cv::WINDOW_AUTOSIZE);

    printf("Clock demo started. Press 'q' or ESC to exit.\n");

    // 主迴圈：不斷更新時間、轉換成 POV buffer、顯示模擬結果。
    while (1) {
        // 先畫固定的時鐘背景與數字。
        draw_clock_numbers(game_canvas);

        // 再根據目前時間畫出時針、分針、秒針。
        draw_clock_hands(game_canvas);

        // 將完整時鐘影像轉換成 POV 裝置可以使用的角度/LED buffer。
        convert_to_pov_buffer(game_canvas, &pov_buffer);

        // 將 POV buffer 畫回螢幕，用來模擬旋轉 LED 的顯示效果。
        simulate_pov_display(pov_buffer, simulation_canvas);

        // 顯示原始時鐘畫布與 POV 模擬畫布。
        cv::imshow("1. Clock Canvas", game_canvas);
        cv::imshow("2. POV Simulator", simulation_canvas);

        // waitKey(16) 約等於每 16 ms 更新一次，接近 60 FPS。
        // 若按 q 或 ESC，就結束程式。
        int key = cv::waitKey(16);
        if (key == 'q' || key == 27) {
            break;
        }
    }

    // 關閉所有 OpenCV 視窗。
    cv::destroyAllWindows();
    return 0;
}
