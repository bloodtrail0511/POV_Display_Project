#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <signal.h>
#include <array>
#include <vector>
#include <fcntl.h>
#include <errno.h>

#include <pthread.h>
#include <queue>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <opencv2/opencv.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

// 引入封裝好的 POV Display Library
#include "pov_display.hpp"

#define ON_THRESHOLD   8
#define OFF_THRESHOLD  5

#define BOOT_ANIM_MS   2000
#define LOOP_DELAY_US  16000   // 約 60 FPS

#define CANVAS_SIZE 800

// 開機提醒
#define READY_BLINK_UNIT_MS 250
#define READY_BLINK_TOTAL_PHASES 4

static volatile sig_atomic_t g_stop = 0;

#define MAX_PLAYERS 4
struct PlayerKey {
    int player_id;
    int key;
};
// POSIX Mutex 與共用queue
static pthread_mutex_t g_key_mutex = PTHREAD_MUTEX_INITIALIZER;
static std::queue<PlayerKey> g_key_queue;

// 實體按鍵的「上半部」中斷旗標
static volatile sig_atomic_t g_local_btn_flag = 0;
// static bool g_player_active[MAX_PLAYERS] = {true, false, false, false};
static bool g_player_active[MAX_PLAYERS] = {false, false, false, false};

static cv::VideoCapture g_video_cap;
static bool started = false;
static std::vector<cv::Mat> g_video_thumbnails; // 縮圖
static bool g_videos_loaded = false;
static pthread_t g_thumb_tid;
static volatile sig_atomic_t g_thumb_loading = 0; // 0:未開始, 1:下載中, 2:已完成
void* preload_thumbnails_thread(void* arg);

int btn_fd = -1; // for button signal

typedef enum {
    STATE_IDLE,
    STATE_BOOT_ANIM,
    // STATE_SHUTDOWN_ANIM,
    STATE_MAIN_MENU,
    STATE_CLOCK,
    STATE_VIDEO,
    STATE_VIDEO_PLAYING,
    STATE_GAME
} SystemState;

//開機提示
typedef enum {
    ANIM_RUNNING,
    ANIM_DONE
} AnimResult;

typedef struct {
    POVDisplay* display;    // 控制 POV 顯示 driver 的物件指標
    int menu_index;         // 記錄目前選單選到哪一個項目
    int video_index;
    int stable_hall_count;  // 用來數「連續幾次 Hall 週期是穩定的
    SystemState current_state;
    cv::Mat render_canvas;
    volatile sig_atomic_t need_render;
    volatile sig_atomic_t timer_flag;
} AppContext;

static cv::Mat draw_main_menu_canvas(const AppContext* ctx);
// ==========================================================
// 功能函數
// ==========================================================
static long long now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()
    ).count();
}

// ==========================================================
// Pong 遊戲狀態結構
// ==========================================================
struct PongGame {
    int state; // 0: 等待大廳 (Waiting Room), 1: 遊戲中 (Playing)
    bool joined[MAX_PLAYERS];
    float angles[MAX_PLAYERS];
    int scores[MAX_PLAYERS];
    cv::Scalar colors[MAX_PLAYERS];
    
    float ball_x, ball_y;
    float ball_vx, ball_vy;
    int ball_owner;
    cv::Scalar ball_color;
    int serve_delay_timer;
    
    void reset() {
        state = 0;
        for(int i = 0; i < MAX_PLAYERS; i++) {
            joined[i] = false;
            scores[i] = 0;
        }
        ball_owner = -1;
        // 分配 4 種玩家專屬顏色 (BGR 格式)
        colors[0] = cv::Scalar(255, 100, 50);  // P0 (藍)
        colors[1] = cv::Scalar(50, 50, 255);   // P1 (紅)
        colors[2] = cv::Scalar(50, 255, 50);   // P2 (綠)
        colors[3] = cv::Scalar(0, 255, 255);   // P3 (黃)
    }
};

static PongGame g_pong;
static bool g_pong_initialized = false;
static bool g_btn_left_held[MAX_PLAYERS] = {false};
static bool g_btn_right_held[MAX_PLAYERS] = {false};

// ==========================================================
// 按鍵處理
// ==========================================================
static void push_network_key(int key, int player_id)
{
    pthread_mutex_lock(&g_key_mutex);
    g_key_queue.push({player_id, key});
    pthread_mutex_unlock(&g_key_mutex);
}

static int consume_key(int* out_player_id = nullptr) {
    // 1. 下半部 (Bottom-Half)：如果實體按鍵有中斷，去讀取它
    if (g_local_btn_flag) {
        g_local_btn_flag = 0; // 放下旗標
        uint8_t key_code = 0;
        // 把按鍵讀出來塞進 Queue
        if (btn_fd >= 0 && read(btn_fd, &key_code, 1) > 0 && key_code != 0) {
            pthread_mutex_lock(&g_key_mutex);
            g_key_queue.push({0, key_code});
            pthread_mutex_unlock(&g_key_mutex);
        }
    }

    // 2. 從共用佇列拿按鍵
    int result_key = -1;
    pthread_mutex_lock(&g_key_mutex);           // 🔒 上鎖
    if (!g_key_queue.empty()) {
        PlayerKey ev = g_key_queue.front();       
        g_key_queue.pop();
        result_key = ev.key;
        if (out_player_id) *out_player_id = ev.player_id; // 吐出是誰按的
    }
    pthread_mutex_unlock(&g_key_mutex);         // 🔓 解鎖
    return result_key;
}

static bool is_left_key(int key) {
    return key == 'a' || key == 2424832 || key == 81 || key == 65361 || (key & 0xFF) == 81 || (key & 0xFFFF) == 65361;
}

static bool is_right_key(int key) {
    return key == 'd' || key == 2555904 || key == 83 || key == 65363 || (key & 0xFF) == 83 || (key & 0xFFFF) == 65363;
}

static bool is_confirm_key(int key) {
    return key == 'w' || key == 13 || key == 10 || key == 2490368 || key == 82 || key == 65362 || (key & 0xFF) == 13 || (key & 0xFF) == 10 || (key & 0xFFFF) == 65362;
}

static bool is_back_key(int key) {
    return key == 'b' || key == 8 || key == 2621440 || key == 84 || key == 65364 || (key & 0xFF) == 8 || (key & 0xFFFF) == 65364;
}

static bool is_left_key_up(int key) { return key == 'A'; }
static bool is_right_key_up(int key) { return key == 'D'; }
static bool is_confirm_key_up(int key) { return key == 'W'; }
static bool is_back_key_up(int key) { return key == 'B'; }

// ==========================================================
// TCP socket server
// ==========================================================
struct ClientArgs {
    int sock;
    int player_id;
};
void* client_handler(void* arg) {
    ClientArgs* args = (ClientArgs*)arg;
    int client_sock = args->sock;
    int player_id = args->player_id;
    delete args; // 釋放動態記憶體
    
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000; 
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    printf("[NET] Player %d connected! FD: %d\n", player_id, client_sock);
    char buf[16];
    
    while (!g_stop) {
        int n = recv(client_sock, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue; 
            else break; 
        } else if (n == 0) {
            printf("[NET] Player %d disconnected.\n", player_id);
            break;
        }

        for (int i = 0; i < n; i++) {
            push_network_key(buf[i], player_id); // 塞入時標記是誰按的
        }
    }
    
    // 斷線時釋放這個玩家的空位
    g_player_active[player_id] = false; 
    close(client_sock);
    return NULL;
}

// 監聽 8888 Port 的主 Server 執行緒
void* socket_server_thread(void* arg) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(8888);
    bind(server_fd, (struct sockaddr *)&address, sizeof(address));
    listen(server_fd, 5);
    fcntl(server_fd, F_SETFL, O_NONBLOCK); 
    
    printf("[NET] TCP Server listening on port 8888...\n");
    
    while (!g_stop) {
        int client_sock = accept(server_fd, NULL, NULL);
        if (client_sock >= 0) {
            // 尋找 1~3 號空位給新連線的手把
            int slot = -1;
            for (int i = 1; i < MAX_PLAYERS; i++) {
                if (!g_player_active[i]) {
                    g_player_active[i] = true;
                    slot = i;
                    break;
                }
            }
            if (slot != -1) {
                ClientArgs* args = new ClientArgs{client_sock, slot};
                pthread_t client_tid;
                pthread_create(&client_tid, NULL, client_handler, args);
                pthread_detach(client_tid); 
            } else {
                printf("[NET] Server full, rejecting.\n");
                close(client_sock);
            }
        }
        usleep(100000); 
    }
    close(server_fd);
    return NULL;
}


// ==========================================================
// 畫圖
// ==========================================================

static cv::Mat make_black_canvas(POVDisplay* display)
{
    return cv::Mat::zeros(CANVAS_SIZE, CANVAS_SIZE, CV_8UC3);
}

static cv::Mat make_ready_outer_ring_canvas()
{
    cv::Mat canvas = cv::Mat::zeros(CANVAS_SIZE, CANVAS_SIZE, CV_8UC3);

    int center = CANVAS_SIZE / 2;
    int radius = static_cast<int>(CANVAS_SIZE * 0.48);

    // 外圈綠色，代表 ready
    cv::circle(
        canvas,
        cv::Point(center, center),
        radius,
        cv::Scalar(0, 100, 0),
        35,
        cv::LINE_AA
    );

    return canvas;
}

static AnimResult ready_blink_animation(AppContext* ctx)
{
    static bool anim_started = false;
    static bool anim_done = false;
    static long long start_time_ms = 0;

    if (anim_done) {
        return ANIM_DONE;
    }

    if (!anim_started) {
        anim_started = true;
        start_time_ms = now_ms();
        printf("[STARTUP] ready blink animation start\n");
    }

    long long elapsed = now_ms() - start_time_ms;
    int phase = elapsed / READY_BLINK_UNIT_MS;

    if (phase >= READY_BLINK_TOTAL_PHASES) {
        anim_done = true;

        ctx->render_canvas = make_black_canvas(ctx->display);
        ctx->need_render = 1;

        printf("[STARTUP] ready blink animation done\n");
        return ANIM_DONE;
    }

    bool green_on = (phase == 0 || phase == 2);

    if (green_on) {
        ctx->render_canvas = make_ready_outer_ring_canvas();
    } else {
        ctx->render_canvas = make_black_canvas(ctx->display);
    }

    ctx->need_render = 1;

    return ANIM_RUNNING;
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

static void draw_centered_text(cv::Mat& canvas, const std::string& text, cv::Point center, double scale, int thickness, cv::Scalar color) {
    int baseline = 0;
    cv::Size text_size = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, scale, thickness, &baseline);
    cv::Point origin(center.x - text_size.width / 2, center.y + text_size.height / 2);
    cv::putText(canvas, text, origin, cv::FONT_HERSHEY_SIMPLEX, scale, color, thickness, cv::LINE_AA);
}

static cv::Mat load_icon_image(const std::string& path) {
    cv::Mat img = cv::imread(path, cv::IMREAD_UNCHANGED);
    if (!img.empty()) {
        printf("[MENU] loaded icon: %s\n", path.c_str());
    } else {
        printf("[MENU] icon not found: %s\n", path.c_str());
    }
    return img;
}

static void overlay_image(cv::Mat& dst, const cv::Mat& src, cv::Point center, int target_size, double opacity) {
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
                src_bgr = icon.at<cv::Vec3b>(y, x);
            }
            cv::Vec3b& dst_px = dst.at<cv::Vec3b>(dy, dx);
            for (int c = 0; c < 3; c++) {
                dst_px[c] = static_cast<uint8_t>(dst_px[c] * (1.0 - alpha) + src_bgr[c] * alpha);
            }
        }
    }
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

static cv::Mat draw_main_menu_canvas(const AppContext* ctx) {
    static std::vector<cv::Mat> icons = {
        load_icon_image("assets/icon/clock.png"),
        load_icon_image("assets/icon/youtube.png"),
        load_icon_image("assets/icon/game.png")
    };
    cv::Mat canvas = cv::Mat::zeros(CANVAS_SIZE, CANVAS_SIZE, CV_8UC3);
    int cx = CANVAS_SIZE / 2;
    int cy = CANVAS_SIZE / 2;
    // bool blink_on = ((ctx->blink_tick / 18) % 2) == 0;
    bool blink_on = ((now_ms() / 500) % 2) == 0;

    cv::circle(canvas, cv::Point(cx, cy), static_cast<int>(CANVAS_SIZE * 0.50), cv::Scalar(80, 80, 80), 35, cv::LINE_AA);

    std::array<cv::Point, 3> positions = {
        cv::Point(cx - static_cast<int>(CANVAS_SIZE * 0.25), cy + static_cast<int>(CANVAS_SIZE * 0.08)),
        cv::Point(cx,                                      cy + static_cast<int>(CANVAS_SIZE * 0.27)),
        cv::Point(cx + static_cast<int>(CANVAS_SIZE * 0.25), cy + static_cast<int>(CANVAS_SIZE * 0.08))
    };

    for (int i = 0; i < 3; i++) {
        draw_icon_item(canvas, i, positions[i], i == ctx->menu_index, blink_on, icons);
    }
    draw_centered_text(canvas, "MENU", cv::Point(cx, cy - static_cast<int>(CANVAS_SIZE * 0.22)), 4.5, 13, cv::Scalar(220, 220, 220));
    return canvas;
}

static cv::Mat draw_clock_canvas(const AppContext* ctx) {
    // 1. 自己生出畫布大小、中心點與半徑
    cv::Mat canvas = cv::Mat::zeros(CANVAS_SIZE, CANVAS_SIZE, CV_8UC3);
    cv::Point center(CANVAS_SIZE / 2, CANVAS_SIZE / 2);
    int radius = 340; // 配合 Menu 比例的完美半徑

    // 2. 開始畫時鐘外圈與中心基準點
    cv::circle(canvas, center, radius + 40, cv::Scalar(50, 50, 50), 20, cv::LINE_AA);
    cv::circle(canvas, center, 5, cv::Scalar(80, 80, 80), -1, cv::LINE_AA);

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
            int tick_length = 30; 
            
            int outer_r = radius + (tick_length / 2);
            int inner_r = radius - (tick_length / 2);

            int x_outer = center.x + static_cast<int>(outer_r * sin(angle_rad));
            int y_outer = center.y - static_cast<int>(outer_r * cos(angle_rad));
            int x_inner = center.x + static_cast<int>(inner_r * sin(angle_rad));
            int y_inner = center.y - static_cast<int>(inner_r * cos(angle_rad));

            // 畫出淺灰色的粗線段
            cv::line(canvas, cv::Point(x_outer, y_outer), cv::Point(x_inner, y_inner), 
                     cv::Scalar(200, 200, 200), 6, cv::LINE_AA);
        }
    }
    
    // 3. 取得系統時間
    time_t now = time(NULL);
    struct tm local_tm;

#ifdef _WIN32
    localtime_s(&local_tm, &now);
#else
    local_tm = *localtime(&now);
#endif

    // 指針長度跟著半徑走
    int outer_radius = radius + 15;

    double second_value = (double)local_tm.tm_sec;
    double minute_value = (double)local_tm.tm_min + second_value / 60.0;
    double hour_value = (double)(local_tm.tm_hour % 12) + minute_value / 60.0;

    double second_angle = second_value * 6.0;
    double minute_angle = minute_value * 6.0;
    double hour_angle = hour_value * 30.0;

    auto endpoint = [&](double angle_deg, double length_scale) {
        double angle_rad = angle_deg * M_PI / 180.0;
        int x = (int)(center.x + outer_radius * length_scale * sin(angle_rad));
        int y = (int)(center.y - outer_radius * length_scale * cos(angle_rad));
        return cv::Point(x, y);
    };

    cv::Point hour_tip = endpoint(hour_angle, 0.50);    
    cv::Point minute_tip = endpoint(minute_angle, 0.72); 
    cv::Point second_tip = endpoint(second_angle, 0.84); 

    double size_scale = CANVAS_SIZE / 1200.0;
    cv::line(canvas, center, hour_tip, cv::Scalar(255, 255, 255), (int)(20 * size_scale), cv::LINE_AA);
    cv::line(canvas, center, minute_tip, cv::Scalar(220, 220, 220), (int)(12 * size_scale), cv::LINE_AA);
    cv::line(canvas, center, second_tip, cv::Scalar(80, 80, 255), (int)(5 * size_scale), cv::LINE_AA);

    cv::circle(canvas, center, (int)(16 * size_scale), cv::Scalar(230, 230, 230), -1, cv::LINE_AA);
    cv::circle(canvas, center, (int)(7 * size_scale), cv::Scalar(80, 80, 255), -1, cv::LINE_AA);

    // 4. 記得回傳畫好的影像！
    return canvas;
}

SystemState idle_handler(AppContext* ctx)
{
    /*
     * 第一次進入 IDLE 時，先播放 ready blink。
     * animation 還在跑時，不執行 off()，也不判斷 Hall。
     */
    if (ready_blink_animation(ctx) == ANIM_RUNNING) {
        return STATE_IDLE;
    }
    ctx->menu_index = 0;

    /*
     * ready blink 播完後，才是真正的 IDLE。
     */
    ctx->display->off();

    if (!started) {
        if (g_thumb_loading == 0) {
            g_thumb_loading = 1;
            pthread_create(&g_thumb_tid, NULL, preload_thumbnails_thread, NULL);
            pthread_detach(g_thumb_tid);
        }
    }

    if (ctx->stable_hall_count >= ON_THRESHOLD) {
        printf("[FSM] IDLE -> BOOT_ANIM, hall_count = %u\n",
               ctx->stable_hall_count);
        return STATE_BOOT_ANIM;
    }

    return STATE_IDLE;
}

SystemState boot_anim_handler(AppContext* ctx){
    static long long start_time_ms = 0;

    if (!started) {
        started = true;
        start_time_ms = now_ms();
        printf("[FSM] BOOT_ANIM start\n");
    }

    if (ctx->stable_hall_count <= OFF_THRESHOLD) {
        printf("[FSM] BOOT_ANIM interrupted -> IDLE, hall_count = %u\n",
               ctx->stable_hall_count);

        started = false;

        cv::Mat black = make_black_canvas(ctx->display);
        ctx->render_canvas = black;
        ctx->need_render = 1;
        // ctx->display->show(black);

        return STATE_IDLE;
    }

    long long elapsed = now_ms() - start_time_ms;
    double ratio = static_cast<double>(elapsed) / static_cast<double>(BOOT_ANIM_MS);

    if (ratio > 1.0) {
        ratio = 1.0;
    }

    // cv::Mat boot_canvas = make_boot_green_canvas(ratio);
    // cv::Mat boot_canvas = make_boot_sweep_canvas(ctx, ratio);
    cv::Mat boot_canvas = make_boot_shutter_canvas(ctx, ratio);
    ctx->render_canvas = boot_canvas;
    ctx->need_render = 1;
    // ctx->display->show(boot_canvas);

    if (elapsed >= BOOT_ANIM_MS) {
        printf("[FSM] BOOT_ANIM -> MAIN_MENU\n");
        started = false;
        ctx->menu_index = 0;
        // ctx->blink_tick = 0;
        return STATE_MAIN_MENU;
    }


    return STATE_BOOT_ANIM;
}

// SystemState shutdown_anim_handler(AppContext* ctx){


//     return STATE_IDLE;
// }

SystemState main_menu_handler(AppContext* ctx){
    if (ctx->stable_hall_count <= OFF_THRESHOLD) {
        printf("[FSM] MAIN_MENU -> IDLE, hall_count = %u\n", ctx->stable_hall_count);
        return STATE_IDLE;
    }

    // int key = g_last_key;
    // g_last_key = -1; // 讀取後清空
    int key = consume_key();

    const int menu_count = 3;
    if (is_left_key(key)) {
        ctx->menu_index = (ctx->menu_index + menu_count - 1) % menu_count;
        printf("[MENU] selected index = %d\n", ctx->menu_index);
    } else if (is_right_key(key)) {
        ctx->menu_index = (ctx->menu_index + 1) % menu_count;
        printf("[MENU] selected index = %d\n", ctx->menu_index);
    } else if (is_confirm_key(key)) {
        if (ctx->menu_index == 0) return STATE_CLOCK;
        else if (ctx->menu_index == 1) return STATE_VIDEO;
        else return STATE_GAME;
    }

    // ctx->blink_tick++;
    cv::Mat menu_canvas = draw_main_menu_canvas(ctx);
    ctx->render_canvas = menu_canvas;
    ctx->need_render = 1;
    // ctx->display->show(menu_canvas);

    return STATE_MAIN_MENU;
}

SystemState clock_handler(AppContext* ctx){
    if (ctx->stable_hall_count <= OFF_THRESHOLD) {
        printf("[FSM] MAIN_MENU -> IDLE, hall_count = %u\n", ctx->stable_hall_count);
        return STATE_IDLE;
    }

    // int key = g_last_key;
    // g_last_key = -1; // 讀取後清空
    int key = consume_key();
    if(is_back_key(key)) return STATE_MAIN_MENU;

    cv::Mat clock_canvas = draw_clock_canvas(ctx);
    ctx->render_canvas = clock_canvas;
    ctx->need_render = 1;

    return STATE_CLOCK;
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

void* preload_thumbnails_thread(void* arg) {
    if (g_videos_loaded) return NULL;
    
    printf("[ASYNC VIDEO] Background thumbnail loading started...\n");
    std::vector<cv::String> video_files;
    cv::glob("assets/video/*.mp4", video_files, false);
    
    if (!video_files.empty()) {
        std::sort(video_files.begin(), video_files.end());
        for (const auto& path : video_files) {
            // 如果主程式準備結束，提早退出執行緒
            if (g_stop) break; 
            
            cv::Mat thumb = get_video_thumbnail(path);
            if (!thumb.empty()) {
                // 🔒 注意：因為 g_video_thumbnails 會在 main 執行緒被讀取，
                // 這裡安全起見用已經存在的 g_key_mutex 或是獨立 mutex 保護
                pthread_mutex_lock(&g_key_mutex);
                g_video_thumbnails.push_back(thumb);
                pthread_mutex_unlock(&g_key_mutex);
            }
            // 微小延遲，避免搶佔過多 CPU 資源影響開機動畫
            usleep(10000); 
        }
    }
    
    g_videos_loaded = true;
    g_thumb_loading = 2; // 標記完成
    printf("[ASYNC VIDEO] All thumbnails preloaded in background! Total: %zu\n", g_video_thumbnails.size());
    return NULL;
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

SystemState video_handler(AppContext* ctx){
    if (ctx->stable_hall_count <= OFF_THRESHOLD) return STATE_IDLE;

    // int key = g_last_key;
    // g_last_key = -1; // 讀取後清空
    int key = consume_key();
    
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

    cv::Mat video_canvas = draw_video_menu_canvas(ctx);
    ctx->render_canvas = video_canvas;
    ctx->need_render = 1;

    return STATE_VIDEO;
}

SystemState video_playing_handler(AppContext* ctx){
    static long long last_frame_time_ms = 0;
    static cv::Mat current_frame_cache;

    // 1. 硬體防護：停轉就關閉影片並關機
    if (ctx->stable_hall_count <= OFF_THRESHOLD) {
        g_video_cap.release();
        current_frame_cache.release();
        last_frame_time_ms = 0; // 離開時重置時間
        return STATE_IDLE;
    }

    // 2. 按鍵偵測：按下 B 鍵或 Enter 鍵退出播放
    // int key = g_last_key;
    // g_last_key = -1; // 讀取後清空
    int key = consume_key();
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
    ctx->render_canvas = current_frame_cache;
    ctx->need_render = 1;

    return STATE_VIDEO_PLAYING;
}

static cv::Mat draw_game_canvas(const AppContext* ctx) {
    cv::Mat canvas = cv::Mat::zeros(CANVAS_SIZE, CANVAS_SIZE, CV_8UC3);

    // 1. 定義場地與尺寸參數
    float center_x = CANVAS_SIZE / 2.0f;
    float center_y = CANVAS_SIZE / 2.0f;
    int r_step = CANVAS_SIZE / 2 / 10;
    int arena_radius = center_x - r_step;
    int ball_radius = r_step * 0.8;
    float paddle_size = 25.0;

    // 計算目前有幾個人加入
    int active_joined_count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (g_pong.joined[i]) active_joined_count++;
    }

    // ==========================================
    // 2. 物理引擎運算區塊 (從 Handler 搬過來的)
    // ==========================================
    if (g_pong.state == 1) {
        if (g_pong.serve_delay_timer > 0) {
            g_pong.serve_delay_timer--;
            if (g_pong.serve_delay_timer == 0) {
                // // 發球
                // float serve_angle = (rand() % 360) * M_PI / 180.0;
                // float serve_speed = 9.0;
                // g_pong.ball_vx = serve_speed * cos(serve_angle);
                // g_pong.ball_vy = serve_speed * sin(serve_angle);
                // 發球：隨機發到某一個已加入玩家的板子正中間
                std::vector<int> joined_players;

                for (int i = 0; i < MAX_PLAYERS; i++) {
                    if (g_pong.joined[i]) {
                        joined_players.push_back(i);
                    }
                }

                if (!joined_players.empty()) {
                    int target_player = joined_players[rand() % joined_players.size()];

                    // 板子的正中間角度
                    float serve_angle_deg = g_pong.angles[target_player];
                    float serve_angle = serve_angle_deg * M_PI / 180.0f;

                    float serve_speed = 9.0f;

                    // 往該板子的方向發球
                    g_pong.ball_vx = serve_speed * cos(serve_angle);
                    g_pong.ball_vy = serve_speed * sin(serve_angle);

                    // 避免一發出去馬上因為 ball_owner 判斷造成不能接
                    g_pong.ball_owner = -1;

                    printf("[PONG] Serve to player %d center, angle = %.1f\n",
                        target_player, serve_angle_deg);
                }
            }
        } else {
            g_pong.ball_x += g_pong.ball_vx;
            g_pong.ball_y += g_pong.ball_vy;
        }

        float dx = g_pong.ball_x - center_x;
        float dy = g_pong.ball_y - center_y;
        float distance = sqrt(dx*dx + dy*dy);

        // 如果球撞到邊界
        if (distance + ball_radius > arena_radius) {
            float ball_angle_rad = atan2(dy, dx);
            float ball_angle_deg = ball_angle_rad * 180.0 / M_PI;
            if (ball_angle_deg < 0) ball_angle_deg += 360.0;

            auto get_angle_diff = [](float a1, float a2) {
                float diff = fmod(fabs(a1 - a2), 360.0);
                return diff > 180.0 ? 360.0 - diff : diff;
            };

            float hit_tolerance = paddle_size + 5.0;
            bool hit_someone = false;
            int hit_player_id = -1;

            // 檢查是誰擋下了球
            for(int i = 0; i < MAX_PLAYERS; i++) {
                if (g_pong.joined[i]) {
                    float diff = get_angle_diff(ball_angle_deg, g_pong.angles[i]);
                    if (diff <= hit_tolerance && g_pong.ball_owner != i) {
                        hit_someone = true;
                        hit_player_id = i;
                        break;
                    }
                }
            }

            if (hit_someone) {
                // 成功反彈、加速與染色
                g_pong.ball_owner = hit_player_id;
                g_pong.ball_color = g_pong.colors[hit_player_id];
                
                float overlap = (distance + ball_radius) - arena_radius;
                g_pong.ball_x -= (dx / distance) * overlap;
                g_pong.ball_y -= (dy / distance) * overlap;

                float nx = dx / distance;
                float ny = dy / distance;

                // 1. 標準反射
                float dot_product = (g_pong.ball_vx * nx) + (g_pong.ball_vy * ny);
                g_pong.ball_vx -= (2 * dot_product * nx);
                g_pong.ball_vy -= (2 * dot_product * ny);

                // 2. 加入切線方向，避免球每次反彈都穿過圓心
                float tx = -ny;
                float ty = nx;

                // 取得這個玩家板子的移動方向
                float paddle_v = 0.0f;
                if (g_btn_left_held[hit_player_id]) {
                    paddle_v = -1.0f;
                } else if (g_btn_right_held[hit_player_id]) {
                    paddle_v = 1.0f;
                }

                // 切球強度：越大越不容易穿過圓心
                float english_strength = 3.0f;
                g_pong.ball_vx += tx * paddle_v * english_strength;
                g_pong.ball_vy += ty * paddle_v * english_strength;

                // 3. 即使板子沒動，也給一點隨機切線偏移
                float random_tangent = ((rand() % 100) / 100.0f - 0.5f) * 2.5f;
                g_pong.ball_vx += tx * random_tangent;
                g_pong.ball_vy += ty * random_tangent;

                // 4. 加速
                g_pong.ball_vx *= 1.05f;
                g_pong.ball_vy *= 1.05f;

                // 5. 限制最高速度，避免越來越快
                float max_speed = r_step * 1.5f;
                float speed = sqrt(g_pong.ball_vx * g_pong.ball_vx + g_pong.ball_vy * g_pong.ball_vy);
                if (speed > max_speed) {
                    g_pong.ball_vx *= max_speed / speed;
                    g_pong.ball_vy *= max_speed / speed;
                }
            } else {
                // 漏接得分，回到中心準備重新發球
                if (g_pong.ball_owner != -1) {
                    g_pong.scores[g_pong.ball_owner]++;
                }
                g_pong.ball_x = center_x;
                g_pong.ball_y = center_y;
                g_pong.ball_owner = -1;
                g_pong.ball_color = cv::Scalar(255, 255, 255);
                g_pong.serve_delay_timer = 60;
                g_pong.ball_vx = 0;
                g_pong.ball_vy = 0;
            }
        }
    }

    // ==========================================
    // 3. 畫面繪圖區塊
    // ==========================================
    cv::circle(canvas, cv::Point(center_x, center_y), arena_radius, cv::Scalar(30, 30, 30), 40, cv::LINE_AA);

    // 畫出所有已加入玩家的板子
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (g_pong.joined[i]) {
            cv::ellipse(canvas, cv::Point(center_x, center_y), cv::Size(arena_radius, arena_radius),
                        0, g_pong.angles[i] - paddle_size, g_pong.angles[i] + paddle_size, 
                        g_pong.colors[i], 40, cv::LINE_AA);
        }
    }

    if (g_pong.state == 0) {
        // 大廳文字提示
        draw_centered_text(canvas, "PONG", cv::Point(center_x, center_y - 80), 3.0, 5, cv::Scalar(255, 255, 255));
        
        char buf[64];
        snprintf(buf, sizeof(buf), "JOINED: %d", active_joined_count);
        draw_centered_text(canvas, buf, cv::Point(center_x, center_y), 1.5, 2, cv::Scalar(200, 200, 200));

        // if (active_joined_count >= 2) {
        //     draw_centered_text(canvas, "PRESS CONFIRM", cv::Point(center_x, center_y + 80), 1.2, 2, cv::Scalar(0, 255, 0));
        // } else {
        //     draw_centered_text(canvas, "WAITING PLAYERS...", cv::Point(center_x, center_y + 80), 1.2, 2, cv::Scalar(100, 100, 100));
        // }
    } else {
        // 畫出球
        cv::circle(canvas, cv::Point(g_pong.ball_x, g_pong.ball_y), ball_radius, g_pong.ball_color, -1, cv::LINE_AA);
    }

    return canvas;
}

SystemState game_handler(AppContext* ctx) {
    // 1. 硬體防護：風扇停轉就強制離開
    if (ctx->stable_hall_count <= OFF_THRESHOLD) {
        g_pong_initialized = false;
        return STATE_IDLE;
    }

    if (!g_pong_initialized) {
        g_pong.reset();
        g_pong_initialized = true;
        // 清空所有人的按鍵狀態
        memset(g_btn_left_held, 0, sizeof(g_btn_left_held));
        memset(g_btn_right_held, 0, sizeof(g_btn_right_held));
        printf("[PONG] 進入遊戲等待大廳...\n");
    }

    // 2. 自動同步連線狀態
    int active_joined_count = 0;
    for(int i = 0; i < MAX_PLAYERS; i++) {
        if (g_player_active[i] && !g_pong.joined[i]) {
            g_pong.angles[i] = i * 90.0f; // 給新加入的玩家一個預設位置
        }
        g_pong.joined[i] = g_player_active[i]; 
        if (g_pong.joined[i]) active_joined_count++;
    }
    
    if (g_pong.state == 1 && active_joined_count < 2) {
        printf("[PONG] 玩家不足 2 人，退回大廳！\n");
        g_pong.state = 0; 
    }

    // ==========================================
    // 3. 輸入處理層 (只更新狀態，不負責移動)
    // ==========================================
    int player_id;
    int key;
    while ((key = consume_key(&player_id)) != -1) {
        if (is_back_key(key)) { 
            g_pong_initialized = false;
            return STATE_MAIN_MENU;
        }

        // player 0 是 server，只允許返回，不允許控制遊戲
        if (player_id == 0) {continue;}

        // 更新這個玩家的「按住/放開」狀態
        if (is_left_key(key))  g_btn_left_held[player_id] = true;
        if (is_right_key(key)) g_btn_right_held[player_id] = true;
        if (is_left_key_up(key))  g_btn_left_held[player_id] = false;
        if (is_right_key_up(key)) g_btn_right_held[player_id] = false;

        // 大廳專屬邏輯：開始遊戲
        if (g_pong.state == 0) {
            if (is_confirm_key(key) && active_joined_count >= 2) {
                printf("[PONG] 遊戲開始！(%d 人)\n", active_joined_count);
                g_pong.state = 1;
                g_pong.ball_x = CANVAS_SIZE / 2.0f;
                g_pong.ball_y = CANVAS_SIZE / 2.0f;
                g_pong.serve_delay_timer = 60; 
                g_pong.ball_owner = -1;
                g_pong.ball_color = cv::Scalar(255, 255, 255);
            }
        }
    }

    // ==========================================
    // 4. 運動與物理層 (每秒執行 60 次)
    // ==========================================
    // 💡 平滑移動邏輯：只要狀態是 true，每一幀都勻速移動！
    float smooth_speed = 3.5f; // 每一幀轉 3.5 度 (你可以調整這個速度)
    
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (g_pong.joined[i]) {
            if (g_btn_left_held[i])  g_pong.angles[i] -= smooth_speed;
            if (g_btn_right_held[i]) g_pong.angles[i] += smooth_speed;
            g_pong.angles[i] = fmod(g_pong.angles[i] + 360.0, 360.0);
        }
    }

    // 呼叫包含球體物理碰撞與畫圖的函式
    ctx->render_canvas = draw_game_canvas(ctx);
    ctx->need_render = 1;

    return STATE_GAME;
}

typedef SystemState (*StateHandler)(AppContext* ctx);

StateHandler state_table[] = {
    idle_handler,
    boot_anim_handler,
    // shutdown_anim_handler,
    main_menu_handler,
    clock_handler,
    video_handler,
    video_playing_handler,
    game_handler
};


void sigint_handler(int sig) {
    g_stop = 1;
}

// void button_signal_handler(int sig) {
//     if (btn_fd < 0) return;

//     uint8_t key_code = 0;
//     ssize_t n = read(btn_fd, &key_code, 1); // 把按鈕讀出來
    
//     if (n > 0 && key_code != 0) {
//         g_last_key = key_code; // 把讀到的按鍵交給主迴圈的狀態機
//         // printf("C++ Received Button: %c\n", key_code); // 測試用，可印出來看看
//     }
// }
void button_signal_handler(int sig) {
    if (btn_fd < 0) return;
    g_local_btn_flag = 1; // Top-Half: 只立旗標通知主迴圈，光速離開
}

void timer_handler(int sig, siginfo_t *si, void *context)
{
    (void)sig;
    (void)context;

    AppContext* ctx = (AppContext*)si->si_value.sival_ptr;
    if (!ctx || !ctx->display) return;

    if (g_stop) {
        return;
    }

    // 1. 立起旗標，通知 Main 迴圈
    ctx->timer_flag = 1;

    // 2. 在這裡統一精準計時，取代原本 main_menu 裡的 ctx->blink_tick++
    // ctx->blink_tick++;
}

int main(){
    // 安裝driver
    // 加上 2>/dev/null 可以把「模組已存在」的報錯訊息隱藏起來，畫面比較乾淨
    system("sudo insmod magnet_driver.ko 2>/dev/null");
    system("sudo insmod pov_display_driver_v3.ko 2>/dev/null");
    system("sudo insmod my_btn_driver_v2.ko 2>/dev/null");
    
    // 註冊 Signal Handler
    signal(SIGINT, sigint_handler);
    
    POVDisplay::Config cfg;
    cfg.device_path = "/dev/pov_display";
    cfg.pixel_brightness_scale = 1.0;
    cfg.apa102_brightness = 15;
    cfg.auto_fit_canvas = false;
    POVDisplay display(cfg);
    
    AppContext ctx = {};
    ctx.display = &display;
    ctx.menu_index = 0;
    ctx.video_index = 0;
    ctx.stable_hall_count = 0;
    ctx.current_state = STATE_IDLE;
    // SystemState current_state = STATE_IDLE;

    //--------------------- POSIX Timer (60Hz) -------------------------//
    struct sigaction sa;
    struct sigevent sev;
    timer_t timerid;
    struct itimerspec its;

    memset(&sa, 0, sizeof(sa));
    sa.sa_flags = SA_SIGINFO;             // 開啟三參數模式以接收附加資料
    sa.sa_sigaction = timer_handler;      // 綁定新的三參數 handler
    sigaction(SIGALRM, &sa, NULL);

    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGALRM;
    sev.sigev_value.sival_ptr = &ctx;     // 將 ctx 指標綁定到 timer 中！

    btn_fd = open("/dev/my_btn", O_RDWR);
    if (btn_fd < 0) {
        perror("Failed to open /dev/my_btn");
    } else {
        // 2. 註冊 SIGIO 訊號的 Handler
        signal(SIGIO, button_signal_handler);
        
        // 3. 告訴 Driver：「我是這支程式 (PID)，請把訊號發給我」
        fcntl(btn_fd, F_SETOWN, getpid());
        
        // 4. 取得目前的 flag，並加上 FASYNC (非同步通知)
        int flags = fcntl(btn_fd, F_GETFL);
        fcntl(btn_fd, F_SETFL, flags | FASYNC);
        
        printf("[BTN] Hardware buttons ready!\n");
    }

    if (timer_create(CLOCK_REALTIME, &sev, &timerid) == -1) {
        perror("timer_create");
        return 1;
    }

    // 16667000 奈秒 = 16.667 毫秒 (約 60Hz)
    its.it_value.tv_sec = 0;
    its.it_value.tv_nsec = 16666667; 
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 16666667;

    if (timer_settime(timerid, 0, &its, NULL) == -1) {
        perror("timer_settime");
        return 1;
    }
    //------------------------------------------------------------------//
    
    pthread_t net_tid;
    pthread_create(&net_tid, NULL, socket_server_thread, NULL);

    while (!g_stop)
    {
        // // 1. 抓取按鍵事件 (需要 KeyReceiver 視窗才能運作)
        // int k = cv::waitKeyEx(1);
        // if (k > 0) {
        //     g_last_key = k;
        // }

        // 2. 當 Timer (60Hz) 觸發時，才去執行狀態機與畫圖
        if (ctx.timer_flag) {
            ctx.timer_flag = 0; // 放下旗標

            // 讀取硬體 Hall 數值
            ctx.stable_hall_count = ctx.display->readHallCount();

            // 執行當前狀態的邏輯 (這會把畫布存進 render_canvas，並設定 need_render)
            ctx.current_state = state_table[ctx.current_state](&ctx);
        }

        // 3. 如果狀態機有產出新的畫布，就更新到 POV 硬體
        if (ctx.need_render) {
            ctx.need_render = 0;
            ctx.display->show(ctx.render_canvas); 
        }

        // 用微小的 sleep 讓出 CPU
        usleep(1000); 
    }
    
    printf("\n收到 Ctrl+C，正在關閉 POV 顯示...\n");

    // 關閉 device fd，這一步很重要
    display.closeDevice();
    timer_delete(timerid);
    if (btn_fd >= 0) {close(btn_fd);}
    pthread_join(net_tid, NULL);

    printf("正在解除 kernel modules...\n");

    // 先不要把錯誤訊息藏掉，方便 debug
    system("sudo rmmod pov_display_driver_v3 2>/dev/null");
    system("sudo rmmod magnet_driver 2>/dev/null");
    system("sudo rmmod my_btn_driver 2>/dev/null");

    return 0;
}