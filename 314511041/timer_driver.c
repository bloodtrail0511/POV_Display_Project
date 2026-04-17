#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/gpio.h>

static struct hrtimer pov_timer;
// 設定計時週期：138 微秒 (轉換成奈秒就是 138,000 ns)
static ktime_t timer_interval; 
#define TIMER_INTERVAL_NS 138000

static uint8_t gpio_status;
#define GPIO 26

// 這個 Callback 函式會在時間到時自動執行
static enum hrtimer_restart pov_timer_callback(struct hrtimer *timer) {
    ktime_t now;

    // --- 這裡放你要做的事 ---
    // 例如：翻轉一根 GPIO 腳位 (Toggle)，或者呼叫 apa102_update_strip() 丟一排資料
    // gpio_set_value(TEST_GPIO, !gpio_get_value(TEST_GPIO));
    // ------------------------
    gpio_status = !gpio_status;
    gpio_set_value(GPIO, gpio_status);

    // 重新設定下一次觸發的時間 (現在時間 + 間隔)
    now = ktime_get();
    hrtimer_forward(timer, now, timer_interval);

    // 回傳 HRTIMER_RESTART 告訴系統「我要一直重複執行」
    // 如果回傳 HRTIMER_NORESTART，計時器響一次就停了
    return HRTIMER_RESTART; 
}

static int __init timer_driver_init(void) 
{
    pr_info("Timer: Initializing timer driver...\n");

    // 將時間轉換為 ktime_t 格式
    timer_interval = ktime_set(0, TIMER_INTERVAL_NS);

    // 初始化 hrtimer (CLOCK_MONOTONIC 代表使用系統單調時間，不受人為調時影響)
    // HRTIMER_MODE_REL 相對時間喚醒
    hrtimer_init(&pov_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);

    // 綁定我們寫好的 callback 函式
    pov_timer.function = &pov_timer_callback;

    // 啟動計時器！
    hrtimer_start(&pov_timer, timer_interval, HRTIMER_MODE_REL);


    if( (gpio_is_valid(GPIO) == false)){ 
      pr_err("GPIO is not valid\n"); 
      return -1;
    }

    if (gpio_request(GPIO, "timer")) return -1;
    gpio_status = 0;
    gpio_direction_output(GPIO, gpio_status);

    return 0;
}


static void __exit timer_driver_exit(void) 
{
    gpio_set_value(GPIO, 0);
    gpio_free(GPIO);
    pr_info("Timer: Driver removed.\n");
}

module_init(timer_driver_init); 
module_exit(timer_driver_exit); 
  
MODULE_LICENSE("GPL"); 
MODULE_AUTHOR("bloodtrail0511 <beckham051188@gmail.com>"); 
MODULE_DESCRIPTION("A simple 7 seg driver - GPIO Driver"); 
MODULE_VERSION("1.0");