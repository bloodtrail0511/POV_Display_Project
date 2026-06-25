#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/gpio.h>

#define GPIO_DATA_0 23
#define GPIO_DATA_1 24
#define GPIO_CLK    25

static struct hrtimer pov_timer;
// 設定計時週期：138 微秒 (轉換成奈秒就是 138,000 ns)
static ktime_t timer_interval; 
#define TIMER_INTERVAL_NS 138000

static void apa102_send_byte(uint8_t data0, uint8_t data1) {
    // APA102 是 MSB First (最高有效位元先傳)
    for (int i = 7; i >= 0; i--) {
        // 1. 準備 Data 腳的電位 (0 或 1)
        gpio_set_value(GPIO_DATA_0, (data0 >> i) & 0x01);
        gpio_set_value(GPIO_DATA_1, (data1 >> i) & 0x01);
        
        // 2. 拉高 Clock (產生上升緣，此時 APA102 會把 Data 讀進去)
        gpio_set_value(GPIO_CLK, 1);
        
        // (Kernel 操作 GPIO 本身就有微小延遲，通常不需要額外加 ndelay，但加了更保險)
        // ndelay(1); 
        
        // 3. 拉低 Clock，準備傳送下一個 bit
        gpio_set_value(GPIO_CLK, 0);
    }
}

// --- 傳送整包 LED 畫面資料 ---
static void apa102_update_strip(void) {
    int i;
    
    // 假設我們模擬一條只有 4 顆 LED 的燈條
    // uint8_t num_leds = 4;
    
    // 1. 送出 Start Frame (4 個 byte 的 0x00)
    for (i = 0; i < 4; i++) {
        apa102_send_byte(0, 0);
    }
    
    // 2. 送出 LED Frames
    // 格式：0xE0 | 亮度(0-31), Blue, Green, Red
    // 第一顆：全亮，純紅色
    apa102_send_byte(0xE0 | 31, 0xE0 | 31); // 11111111 (0xFF)
    apa102_send_byte(0x00, 0x00);      // B
    apa102_send_byte(0x00, 0xFF);      // G
    apa102_send_byte(0xFF, 0x00);      // R
    
    // 第二顆：半亮，純綠色
    apa102_send_byte(0xE0 | 15, 0xE0 | 15);
    apa102_send_byte(0x00, 0x00);      // B
    apa102_send_byte(0xFF, 0x00);      // G
    apa102_send_byte(0x00, 0xFF);      // R
    
    // 第三顆：微亮，純藍色
    apa102_send_byte(0xE0 | 5, 0xE0 | 5);
    apa102_send_byte(0xFF, 0xFF);      // B
    apa102_send_byte(0x00, 0x00);      // G
    apa102_send_byte(0x00, 0x00);      // R
    
    // 第四顆：全亮，白色
    apa102_send_byte(0xE0 | 31, 0xE0 | 31);
    apa102_send_byte(0xFF, 0x00);      // B
    apa102_send_byte(0xFF, 0xFF);      // G
    apa102_send_byte(0xFF, 0xFF);      // R

    // 3. 送出 End Frame (4 個 byte 的 0xFF)
    for (i = 0; i < 4; i++) {
        apa102_send_byte(0xFF, 0xFF);
    }
    
    pr_info("APA102: SPI frame sent via Bit-banging!\n");
}

// 這個 Callback 函式會在時間到時自動執行
static enum hrtimer_restart pov_timer_callback(struct hrtimer *timer) {
    ktime_t now;

    // --- 這裡放你要做的事 ---
    // 例如：翻轉一根 GPIO 腳位 (Toggle)，或者呼叫 apa102_update_strip() 丟一排資料
    // gpio_set_value(TEST_GPIO, !gpio_get_value(TEST_GPIO));
    // ------------------------
    apa102_update_strip();

    // 重新設定下一次觸發的時間 (現在時間 + 間隔)
    now = ktime_get();
    hrtimer_forward(timer, now, timer_interval);

    // 回傳 HRTIMER_RESTART 告訴系統「我要一直重複執行」
    // 如果回傳 HRTIMER_NORESTART，計時器響一次就停了
    return HRTIMER_RESTART; 
}


static int __init pov_display_driver_init(void) {
    pr_info("APA102: Initializing GPIO...\n");
    // 請求 GPIO 資源並設為輸出，預設為低電位 (0)
    if( (gpio_is_valid(GPIO_DATA_0) == false) || 
        (gpio_is_valid(GPIO_DATA_1) == false) ||
        (gpio_is_valid(GPIO_CLK   ) == false)){ 
      pr_err("GPIO is not valid\n"); 
      return -1;
    }

    if (gpio_request(GPIO_DATA_0, "apa102_data_0")) return -1;
    if (gpio_request(GPIO_DATA_1, "apa102_data_1")) {
        gpio_free(GPIO_DATA_0);
        return -1;
    }
    if (gpio_request(GPIO_CLK, "apa102_clk")) {
        gpio_free(GPIO_DATA_0);
        gpio_free(GPIO_DATA_1);
        return -1;
    }

    // 設定 GPIO 為輸出模式，並預設為低電位 (0)
    gpio_direction_output(GPIO_DATA_0, 0);
    gpio_direction_output(GPIO_DATA_1, 0);
    gpio_direction_output(GPIO_CLK, 0);


    pr_info("Timer: Initializing timer...\n");

    // 將時間轉換為 ktime_t 格式
    timer_interval = ktime_set(0, TIMER_INTERVAL_NS);

    // 初始化 hrtimer (CLOCK_MONOTONIC 代表使用系統單調時間，不受人為調時影響)
    // HRTIMER_MODE_REL 相對時間喚醒
    hrtimer_init(&pov_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);

    // 綁定我們寫好的 callback 函式
    pov_timer.function = &pov_timer_callback;

    // 啟動計時器！
    hrtimer_start(&pov_timer, timer_interval, HRTIMER_MODE_REL);



    return 0;
}


static void __exit pov_display_driver_exit(void) {
    gpio_set_value(GPIO_DATA_0, 0);
    gpio_set_value(GPIO_DATA_1, 0);
    gpio_set_value(GPIO_CLK, 0);
    gpio_free(GPIO_DATA_0);
    gpio_free(GPIO_DATA_1);
    gpio_free(GPIO_CLK);
    pr_info("pov_display: Driver removed.\n");
}

module_init(pov_display_driver_init); 
module_exit(pov_display_driver_exit); 
  
MODULE_LICENSE("GPL"); 
MODULE_AUTHOR("bloodtrail0511 <beckham051188@gmail.com>"); 
MODULE_DESCRIPTION("A simple 7 seg driver - GPIO Driver"); 
MODULE_VERSION("1.0");