#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/of.h>
#include <linux/atomic.h>
#include <linux/delay.h>

#define NUM_LEDS 20
#define END_LEN 4
#define SPI_BUF_SIZE (4 + (NUM_LEDS * 4) + END_LEN) // Start(4) + LEDs(160) + End(4) = 168 Bytes

// 驅動狀態與硬體指標
static struct spi_device *pov_spi_device;
static struct hrtimer pov_timer;
static ktime_t timer_interval;

// 用來防止上一筆 SPI 還沒傳完，Timer 又觸發下一筆的保護機制
static atomic_t spi_busy = ATOMIC_INIT(0);

// 將非同步傳輸需要的資料結構宣告為全域變數，並確保記憶體對齊以利 DMA 傳輸
static uint8_t spi_buffer[SPI_BUF_SIZE] ____cacheline_aligned; 
static struct spi_transfer spi_xfer;
static struct spi_message spi_msg;

// 動畫狀態變數 (測試用)
static uint32_t pattern_step = 0;

// --- 1. SPI 傳輸完成的回呼函式 ---
static void spi_complete_callback(void *context) {
    // 當硬體將資料全部推出去後，解除 busy 狀態，允許下一次傳輸
    atomic_set(&spi_busy, 0);
}

// --- 2. 更新畫面資料並發送 ---
static void apa102_update_strip_async(void) {
    int i;
    
    // 如果上一筆還沒傳完，直接放棄這次更新 (Drop frame)，避免核心崩潰
    if (atomic_cmpxchg(&spi_busy, 0, 1)) {
        return; 
    }

    pattern_step++;

    // 填寫 LED 資料 (這裡示範一個簡單的顏色流動效果)
    for (i = 0; i < NUM_LEDS; i++) {
        // 亮度設定：0xE0 | (亮度 1~31)。測試時先設 1，保護供電
        spi_buffer[4 + i*4 + 0] = 0xE0 | 1; 

        // 簡單的流動燈效計算
        if ((i + pattern_step / 10) % 3 == 0) {
            spi_buffer[4 + i*4 + 1] = 0x0F; // B
            spi_buffer[4 + i*4 + 2] = 0x00; // G
            spi_buffer[4 + i*4 + 3] = 0x00; // R
        } else if ((i + pattern_step / 10) % 3 == 1) {
            spi_buffer[4 + i*4 + 1] = 0x00;
            spi_buffer[4 + i*4 + 2] = 0x0F;
            spi_buffer[4 + i*4 + 3] = 0x00;
        } else {
            spi_buffer[4 + i*4 + 1] = 0x00;
            spi_buffer[4 + i*4 + 2] = 0x00;
            spi_buffer[4 + i*4 + 3] = 0x0F;
        }
    }

    // 初始化 message 並綁定 transfer 與 callback
    spi_message_init(&spi_msg);
    spi_message_add_tail(&spi_xfer, &spi_msg);
    spi_msg.complete = spi_complete_callback; 
    
    // 將資料丟進佇列後立刻返回，不會阻塞 (Non-blocking)
    if (spi_async(pov_spi_device, &spi_msg) != 0) {
        // 如果發送失敗，記得解除 busy 狀態
        atomic_set(&spi_busy, 0);
    }
}

static void apa102_off(void) {
    int i;
    
    while (atomic_cmpxchg(&spi_busy, 0, 1)) {
        usleep_range(100, 200); // 睡 100~200 微秒，把 CPU 讓出來
    }

    // 填寫全暗資料... (這段不變)
    for (i = 0; i < NUM_LEDS; i++) {
        spi_buffer[4 + i*4 + 0] = 0xE0 | 0; 
        spi_buffer[4 + i*4 + 1] = 0x00;
        spi_buffer[4 + i*4 + 2] = 0x00;
        spi_buffer[4 + i*4 + 3] = 0x00;
    }

    // 初始化 message 並綁定 transfer
    spi_message_init(&spi_msg);
    spi_message_add_tail(&spi_xfer, &spi_msg);
    
    // ⭐️ 關鍵修改：不要設定 complete callback，並且改用 spi_sync！
    // spi_sync 會卡在這裡，直到硬體把這 300 Bytes 全都傳送完畢才會往下執行
    if (spi_sync(pov_spi_device, &spi_msg) == 0) {
        // 發送成功後解除 busy
        atomic_set(&spi_busy, 0);
    } else {
        atomic_set(&spi_busy, 0);
        pr_err("POV: Failed to send off signal\n");
    }
}

// --- 3. 計時器回呼函式 (138 微秒觸發一次) ---
static enum hrtimer_restart pov_timer_callback(struct hrtimer *timer) {
    // 呼叫非同步發送
    apa102_update_strip_async();

    // 重置計時器，繼續下一次觸發
    hrtimer_forward_now(timer, timer_interval);
    return HRTIMER_RESTART;
}

// --- 4. 設備配對成功時的初始化 (取代原本的 pov_init) ---
static int pov_spi_probe(struct spi_device *spi) {
    int i;

    pr_info("POV: Device Tree matched! Probing SPI device...\n");
    pov_spi_device = spi; // 系統已經幫我們配置好硬體指標了

    // 設定硬體參數
    pov_spi_device->mode = SPI_MODE_0 | SPI_NO_CS;
    pov_spi_device->bits_per_word = 8;
    if (spi_setup(pov_spi_device) != 0) {
        pr_err("POV: SPI setup failed\n");
        return -EINVAL;
    }

    // 預先準備好全域 SPI 緩衝區的不變資料 (Start / End Frames)
    memset(spi_buffer, 0, SPI_BUF_SIZE); 
    for (i = 0; i < 4; i++) spi_buffer[i] = 0x00; // Start Frame
    for (i = 0; i < END_LEN; i++) spi_buffer[SPI_BUF_SIZE - END_LEN + i] = 0xFF; // End Frame
    
    // 設定全域 spi_transfer 結構
    spi_xfer.tx_buf = spi_buffer;
    spi_xfer.len = SPI_BUF_SIZE;

    // 啟動 hrtimer (138 微秒 = 138000 奈秒)
    // timer_interval = ktime_set(0, 138000); 
    timer_interval = ktime_set(0, 10000000); // 5 sec
    hrtimer_init(&pov_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    pov_timer.function = &pov_timer_callback;
    hrtimer_start(&pov_timer, timer_interval, HRTIMER_MODE_REL);

    pr_info("POV: Driver initialized. High-res timer started (138us).\n");
    // apa102_update_strip_async();
    return 0;
}

// --- 5. 設備移除時的清理 ---
static void pov_spi_remove(struct spi_device *spi) {
    // 關閉計時器，確保不再觸發發送
    hrtimer_cancel(&pov_timer);
    apa102_off();
    pr_info("POV: SPI Driver removed. Timer stopped.\n");
}

// --- 6. 宣告 Device Tree 的比對身分證 ---
static const struct of_device_id pov_dt_ids[] = {
    { .compatible = "custom,pov_apa102" },
    { } // 必須以空結構結尾
};
MODULE_DEVICE_TABLE(of, pov_dt_ids);

// --- 7. 註冊 SPI 驅動結構 ---
static struct spi_driver pov_spi_driver = {
    .driver = {
        .name = "pov_apa102",
        .of_match_table = pov_dt_ids,
    },
    .probe = pov_spi_probe,
    .remove = pov_spi_remove,
};

// 取代原本的 module_init / module_exit
module_spi_driver(pov_spi_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("bloodtrail0511");
MODULE_DESCRIPTION("Async SPI Driver for APA102 POV Display");