#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/gpio.h>
#include <linux/io.h>     // 引入 ioremap 和 iowrite32 的標頭檔

#define GPIO_DATA_0 23
#define GPIO_DATA_1 24
#define GPIO_CLK    25

// --- Pi 4 (BCM2711) 專用的實體位址與偏移量 ---
#define BCM2711_GPIO_BASE 0xFE200000 // GPIO 控制器的實體記憶體起點
#define GPSET0_OFFSET     0x1C          // set register 的偏移量
#define GPCLR0_OFFSET     0x28          // clear register 的偏移量

// 儲存映射後的虛擬記憶體指標
static void __iomem *gpio_base;

static struct hrtimer pov_timer;
static ktime_t timer_interval;

// --- 核心技術：MMIO 極速 Bit-banging ---
static void apa102_send_byte_mmio(uint8_t data0, uint8_t data1) {
    int i;
    uint32_t set_mask, clr_mask;

    for (i = 7; i >= 0; i--) {
        set_mask = 0;
        clr_mask = 0;
        
        // 1. 準備 Data 0 的遮罩
        if ((data0 >> i) & 0x01) set_mask |= (1 << GPIO_DATA_0);
        else                     clr_mask |= (1 << GPIO_DATA_0);
        
        // 2. 準備 Data 1 的遮罩
        if ((data1 >> i) & 0x01) set_mask |= (1 << GPIO_DATA_1);
        else                     clr_mask |= (1 << GPIO_DATA_1);
        
        // 3. 瞬間寫入暫存器 (一次同時設定兩根腳位，速度極快！)
        if (set_mask) iowrite32(set_mask, gpio_base + GPSET0_OFFSET);
        if (clr_mask) iowrite32(clr_mask, gpio_base + GPCLR0_OFFSET);
        
        // 4. Clock 產生上升緣與下降緣 (直接對應暫存器寫入 1)
        iowrite32((1 << GPIO_CLK), gpio_base + GPSET0_OFFSET); // CLK High
        iowrite32((1 << GPIO_CLK), gpio_base + GPCLR0_OFFSET); // CLK Low
    }
}

// --- 傳送整包 LED 畫面資料 ---
static void apa102_update_strip(void) {
    int i;
    // 1. Start Frame
    for (i = 0; i < 4; i++) apa102_send_byte_mmio(0, 0);
    
    // 2. LED Frame (測試用：全部亮純藍色)
    // 實際專題中，這裡會變成讀取你轉換好的極座標陣列
    apa102_send_byte_mmio(0xE0 | 31, 0xE0 | 31);
    apa102_send_byte_mmio(0xFF, 0xFF); // B
    apa102_send_byte_mmio(0x00, 0x00); // G
    apa102_send_byte_mmio(0x00, 0x00); // R
    
    // 3. End Frame
    for (i = 0; i < 4; i++) apa102_send_byte_mmio(0xFF, 0xFF);
}

// --- 計時器回呼函式 ---
static enum hrtimer_restart pov_timer_callback(struct hrtimer *timer) {
    ktime_t now;

    // 現在這個函數執行時間不到 1 微秒，絕對不會讓系統死機！
    apa102_update_strip();

    now = ktime_get();
    hrtimer_forward(timer, now, timer_interval);
    return HRTIMER_RESTART;
}

static int __init pov_init(void) {
    pr_info("POV: Initializing MMIO driver...\n");

    // 1. 保留原本的 gpio_request，這能告訴 Kernel 設定好這幾根腳的預設功能
    if (gpio_request_one(GPIO_DATA_0, GPIOF_OUT_INIT_LOW, "apa_d0")) return -1;
    if (gpio_request_one(GPIO_DATA_1, GPIOF_OUT_INIT_LOW, "apa_d1")) goto err_d0;
    if (gpio_request_one(GPIO_CLK, GPIOF_OUT_INIT_LOW, "apa_clk"))   goto err_d1;

    // 2. ⭐️ 核心魔法：映射 GPIO 實體記憶體
    gpio_base = ioremap(BCM2711_GPIO_BASE, 0x100);
    if (!gpio_base) {
        pr_err("POV: Failed to ioremap GPIO base!\n");
        goto err_clk;
    }

    // 3. 啟動 hrtimer
    timer_interval = ktime_set(0, 138000); // 138 微秒
    hrtimer_init(&pov_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    pov_timer.function = &pov_timer_callback;
    hrtimer_start(&pov_timer, timer_interval, HRTIMER_MODE_REL);

    pr_info("POV: MMIO Timer started!\n");
    return 0;

err_clk: gpio_free(GPIO_CLK);
err_d1:  gpio_free(GPIO_DATA_1);
err_d0:  gpio_free(GPIO_DATA_0);
    return -1;
}

static void __exit pov_exit(void) {
    hrtimer_cancel(&pov_timer);
    
    if (gpio_base) iounmap(gpio_base); // 解除記憶體映射
    
    gpio_free(GPIO_CLK);
    gpio_free(GPIO_DATA_1);
    gpio_free(GPIO_DATA_0);
    pr_info("POV: MMIO Driver removed.\n");
}

module_init(pov_init);
module_exit(pov_exit);
MODULE_LICENSE("GPL");