#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/of.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/math64.h>
#include <linux/spinlock.h>
#include <linux/ioctl.h>

#define NUM_LEDS 40
#define END_LEN 5
#define DEGREE_RESOLUTION 180                       // 一個圓切成360分
#define SPI_BUF_SIZE (4 + (NUM_LEDS * 4) + END_LEN) // Start(4) + LEDs(80) + End(4) = 88 Bytes
#define POV_IOC_MAGIC 'p'
#define POV_IOC_OFF   _IO(POV_IOC_MAGIC, 0)

// 驅動狀態與硬體指標
static struct spi_device *pov_spi_device;
static struct hrtimer pov_timer;
static ktime_t timer_interval;

// ============================================================
// Time-based angle estimation
// ============================================================

// 30 rps 時一圈約 33.333 ms
#define INITIAL_PERIOD_NS 33333333LL

// 依你的目標轉速設定合理範圍
// 20 rps = 50 ms, 40 rps = 25 ms
#define MIN_PERIOD_NS 20000000LL   // 20 ms
#define MAX_PERIOD_NS 100000000LL   // 100 ms

// 週期低通濾波：越大越穩，但反應越慢
// new_period = old * 0.8 + measured * 0.2
#define PERIOD_FILTER_OLD 8
#define PERIOD_FILTER_NEW 2

// 如果整張圖固定偏轉，可用這個微調 slice
#define ANGLE_OFFSET_SLICE 0

static ktime_t last_hall_time;
static s64 current_period_ns = INITIAL_PERIOD_NS;
static bool hall_synced = false;

// 用來防止上一筆 SPI 還沒傳完，Timer 又觸發下一筆的保護機制
static atomic_t spi_busy = ATOMIC_INIT(0);

// 將非同步傳輸需要的資料結構宣告為全域變數，並確保記憶體對齊以利 DMA 傳輸
static uint8_t frame_A[DEGREE_RESOLUTION][SPI_BUF_SIZE] ____cacheline_aligned;
static uint8_t frame_B[DEGREE_RESOLUTION][SPI_BUF_SIZE] ____cacheline_aligned;
static struct spi_transfer spi_xfer;
static struct spi_message spi_msg;

// character device
dev_t pov_dev_num = 0;
static struct class *pov_class;
static struct cdev pov_cdev;

// 宣告外部函式 (來自 magnet_driver.c)
extern void register_mag_callback(void (*callback_func)(ktime_t, s64));
extern void unregister_mag_callback(void);

// --- 雙緩衝指標與 Spinlock 保護機制 ---
static DEFINE_SPINLOCK(frame_lock);
static uint8_t (*active_frame)[SPI_BUF_SIZE] = frame_A;
static uint8_t (*back_frame)[SPI_BUF_SIZE] = frame_B;

static bool new_frame_ready = false; 
static bool is_writing = false;      // 記錄 User Space 是否正在執行 copy_from_user

// 控制邏輯變數
// static atomic_t current_slice = ATOMIC_INIT(0); // 馬達目前轉到第幾度，改為 atomic 讓 ISR 和 Timer 安全共用


static int get_current_slice_by_time(void)
{
    ktime_t now;
    s64 elapsed_ns;
    s64 period_ns;
    int slice;
    unsigned long flags;

    spin_lock_irqsave(&frame_lock, flags);
    period_ns = current_period_ns;
    now = ktime_get();
    elapsed_ns = ktime_to_ns(ktime_sub(now, last_hall_time));
    spin_unlock_irqrestore(&frame_lock, flags);

    if (period_ns <= 0) {
        return 0;
    }

    /*
     * elapsed_ns 正常情況下會小於 period_ns。
     * 如果因為剛啟動、漏 Hall 或 timer 延遲導致超過一圈，
     * 就用 Linux kernel 提供的 div64_s64 算商，再扣掉整圈數。
     */
    if (elapsed_ns >= period_ns) {
        s64 q = div64_s64(elapsed_ns, period_ns);
        elapsed_ns -= q * period_ns;
    }

    if (elapsed_ns < 0) {
        elapsed_ns = 0;
    }

    slice = div64_s64(elapsed_ns * DEGREE_RESOLUTION, period_ns);

    if (slice < 0) {
        slice = 0;
    }

    if (slice >= DEGREE_RESOLUTION) {
        slice = DEGREE_RESOLUTION - 1;
    }

    slice = (slice + ANGLE_OFFSET_SLICE) % DEGREE_RESOLUTION;

    return slice;
}
// --- 1. SPI 傳輸完成的回呼函式 ---
static void spi_complete_callback(void *context)
{
    atomic_set(&spi_busy, 0);
}

// --- 2. 更新畫面資料並發送 ---
// static void apa102_update_strip_async(void)
// {
//     int slice;

//     if (atomic_cmpxchg(&spi_busy, 0, 1))
//     {
//         return;
//     }

//     slice = atomic_read(&current_slice);

//     // 直接把 SPI 發送指標指向 active_frame 對應的 Slice
//     spin_lock(&frame_lock);
//     spi_xfer.tx_buf = active_frame[slice];
//     spin_unlock(&frame_lock);
//     spi_xfer.len = SPI_BUF_SIZE;

//     spi_message_init(&spi_msg);
//     spi_message_add_tail(&spi_xfer, &spi_msg);
//     spi_msg.complete = spi_complete_callback;

//     if (spi_async(pov_spi_device, &spi_msg) != 0)
//     {
//         atomic_set(&spi_busy, 0);
//     }

//     // 角度 +1。若馬達降速導致 Timer 跑超過 360，強制歸零避免記憶體越界
//     // 真正的精準 0 度歸零與 Swap 交由霍爾感測器 ISR 處理
//     slice++;
//     if (slice >= DEGREE_RESOLUTION) {
//         slice = 0; 
//     }
//     atomic_set(&current_slice, slice);
// }
static void apa102_update_strip_async(void)
{
    int slice;

    if (atomic_cmpxchg(&spi_busy, 0, 1))
    {
        return;
    }

    // 不再使用 current_slice++，改成根據 Hall 時間與目前時間估算角度
    slice = get_current_slice_by_time();

    // 直接把 SPI 發送指標指向 active_frame 對應的 Slice
    spin_lock(&frame_lock);
    spi_xfer.tx_buf = active_frame[slice];
    spin_unlock(&frame_lock);

    spi_xfer.len = SPI_BUF_SIZE;

    spi_message_init(&spi_msg);
    spi_message_add_tail(&spi_xfer, &spi_msg);
    spi_msg.complete = spi_complete_callback;

    if (spi_async(pov_spi_device, &spi_msg) != 0)
    {
        atomic_set(&spi_busy, 0);
    }
}


static void apa102_off(void)
{
    int i, j;

    while (atomic_cmpxchg(&spi_busy, 0, 1))
    {
        usleep_range(100, 200);
    }

    // 填寫全暗資料
    for (i = 0; i < NUM_LEDS; i++)
    {
        for (j = 0; j < DEGREE_RESOLUTION; j++)
        {
            frame_A[j][4 + i * 4 + 0] = 0xE0 | 0;
            frame_A[j][4 + i * 4 + 1] = 0x00;
            frame_A[j][4 + i * 4 + 2] = 0x00;
            frame_A[j][4 + i * 4 + 3] = 0x00;
            frame_B[j][4 + i * 4 + 0] = 0xE0 | 0;
            frame_B[j][4 + i * 4 + 1] = 0x00;
            frame_B[j][4 + i * 4 + 2] = 0x00;
            frame_B[j][4 + i * 4 + 3] = 0x00;
        }
    }

    spi_xfer.tx_buf = frame_A[0]; 
    spi_xfer.len = SPI_BUF_SIZE;
    spi_message_init(&spi_msg);
    spi_message_add_tail(&spi_xfer, &spi_msg);

    if (spi_sync(pov_spi_device, &spi_msg) == 0)
    {
        atomic_set(&spi_busy, 0);
    }
    else
    {
        atomic_set(&spi_busy, 0);
        pr_err("POV: Failed to send off signal\n");
    }
}

static ssize_t pov_write(struct file *filp, const char __user *buf, size_t len, loff_t *off)
{
    unsigned long flags;
    uint8_t (*target_frame)[SPI_BUF_SIZE];

    if (len != (DEGREE_RESOLUTION * SPI_BUF_SIZE))
        return -EINVAL;

    // 1. 上鎖：標記正在寫入，並安全獲取當下的 back_frame 指標
    spin_lock_irqsave(&frame_lock, flags);
    is_writing = true;
    target_frame = back_frame;
    spin_unlock_irqrestore(&frame_lock, flags);

    // 2. 解鎖狀態下執行 copy_from_user (允許休眠)
    if (copy_from_user(target_frame, buf, len))
    {
        // 寫入失敗，復原寫入狀態
        spin_lock_irqsave(&frame_lock, flags);
        is_writing = false;
        spin_unlock_irqrestore(&frame_lock, flags);
        return -EFAULT;
    }

    // 3. 上鎖：寫入完成，升起新畫面旗標，解除寫入狀態
    spin_lock_irqsave(&frame_lock, flags);
    new_frame_ready = true;
    is_writing = false;
    spin_unlock_irqrestore(&frame_lock, flags);

    return len;
}

static long pov_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    switch (cmd) {
    case POV_IOC_OFF:
        apa102_off();
        return 0;

    default:
        return -ENOTTY;
    }
}

static struct file_operations fops =
{
    .owner = THIS_MODULE,
    .write = pov_write,
    .unlocked_ioctl = pov_ioctl,
};

// --- 4. 計時器回呼函式 ---
static enum hrtimer_restart pov_timer_callback(struct hrtimer *timer)
{
    ktime_t current_interval;
    unsigned long flags;

    apa102_update_strip_async();

    // === 臨時測試代碼：當一輪 360 度跑完時，自動在 Timer 裡進行雙緩衝交換 ===
    // if (atomic_read(&current_slice) == 0 && new_frame_ready && !is_writing) {
    //     spin_lock_irqsave(&frame_lock, flags);
    //     if (new_frame_ready && !is_writing) {
    //         uint8_t (*temp)[SPI_BUF_SIZE] = active_frame;
    //         active_frame = back_frame;
    //         back_frame = temp;
    //         new_frame_ready = false;
    //     }
    //     spin_unlock_irqrestore(&frame_lock, flags);
    // }
    // ===================================================================

    spin_lock_irqsave(&frame_lock, flags);
    current_interval = timer_interval;
    spin_unlock_irqrestore(&frame_lock, flags);

    hrtimer_forward_now(timer, current_interval);
    return HRTIMER_RESTART;
}

// 2. 要掛載到磁力驅動上的同步校正函式
// static void pov_hall_sync(ktime_t current_time, s64 delta_ns)
// {
//     u64 slice_ns;
//     unsigned long flags;

//     // 計算新頻率並更新 Timer
//     slice_ns = div_u64((u64)delta_ns, DEGREE_RESOLUTION);
    
//     // 經過磁鐵，絕對 0 度物理對齊！
//     atomic_set(&current_slice, 0);
    
//     // 雙緩衝切換 (Ping-Pong Swap)
//     spin_lock_irqsave(&frame_lock, flags);
//     timer_interval = ns_to_ktime(slice_ns);
//     if (new_frame_ready && !is_writing) {
//         uint8_t (*temp)[SPI_BUF_SIZE] = active_frame;
//         active_frame = back_frame;
//         back_frame = temp;
//         new_frame_ready = false;
//     }
//     spin_unlock_irqrestore(&frame_lock, flags);
// }
static void pov_hall_sync(ktime_t current_time, s64 delta_ns)
{
    u64 slice_ns;
    unsigned long flags;

    spin_lock_irqsave(&frame_lock, flags);

    // 記錄這次 Hall 通過時間，作為物理 0 度基準
    last_hall_time = current_time;
    hall_synced = true;

    // 只接受合理週期
    if (delta_ns >= MIN_PERIOD_NS && delta_ns <= MAX_PERIOD_NS) {
        // 對週期做低通濾波，避免每圈速度微小抖動造成畫面角度抖動
        current_period_ns =
            div_s64(current_period_ns * PERIOD_FILTER_OLD +
                    delta_ns * PERIOD_FILTER_NEW,
                    PERIOD_FILTER_OLD + PERIOD_FILTER_NEW);
    }

    // timer 只是負責定期刷新，不再代表真實角度
    // 這裡使用濾波後的 period 來決定大約每個 slice 刷一次
    slice_ns = div_u64((u64)current_period_ns, DEGREE_RESOLUTION);
    timer_interval = ns_to_ktime(slice_ns);

    // 雙緩衝切換
    if (new_frame_ready && !is_writing) {
        uint8_t (*temp)[SPI_BUF_SIZE] = active_frame;
        active_frame = back_frame;
        back_frame = temp;
        new_frame_ready = false;
    }

    spin_unlock_irqrestore(&frame_lock, flags);
}

// --- 5. 設備配對成功時的初始化 ---
static int pov_spi_probe(struct spi_device *spi)
{
    int i, j;
    struct device *dev_ret;

    pr_info("POV: Device Tree matched! Probing SPI device...\n");
    pov_spi_device = spi;

    pov_spi_device->mode = SPI_MODE_0 | SPI_NO_CS;
    pov_spi_device->bits_per_word = 8;
    if (spi_setup(pov_spi_device) != 0)
    {
        pr_err("POV: SPI setup failed\n");
        return -EINVAL;
    }

    // 初始化緩衝區 Start/End Frames
    memset(frame_A, 0, SPI_BUF_SIZE * DEGREE_RESOLUTION);
    memset(frame_B, 0, SPI_BUF_SIZE * DEGREE_RESOLUTION);
    for (i = 0; i < DEGREE_RESOLUTION; i++)
    {
        for (j = 0; j < END_LEN; j++) {
            frame_A[i][SPI_BUF_SIZE - END_LEN + j] = 0xFF;
            frame_B[i][SPI_BUF_SIZE - END_LEN + j] = 0xFF;
        }
    }

    apa102_off();

    // 註冊 character device
    if ((alloc_chrdev_region(&pov_dev_num, 0, 1, "pov_dev")) < 0)
    {
        pr_err("Cannot allocate major number\n");
        return -1;
    }

    cdev_init(&pov_cdev, &fops);
    if ((cdev_add(&pov_cdev, pov_dev_num, 1)) < 0)
    {
        pr_err("Cannot add the device to the system\n");
        unregister_chrdev_region(pov_dev_num, 1);
        return -1;
    }

    pov_class = class_create(THIS_MODULE, "pov_class");
    if (IS_ERR(pov_class))
    {
        pr_err("Cannot create the struct class\n");
        cdev_del(&pov_cdev);
        unregister_chrdev_region(pov_dev_num, 1);
        return PTR_ERR(pov_class);
    }

    dev_ret = device_create(pov_class, NULL, pov_dev_num, NULL, "pov_display");
    if (IS_ERR(dev_ret))
    {
        pr_err("Cannot create the Device\n");
        class_destroy(pov_class);
        cdev_del(&pov_cdev);
        unregister_chrdev_region(pov_dev_num, 1);
        return PTR_ERR(dev_ret);
    }
    
    // 把 POV 的同步邏輯，掛載到 Magnet 驅動上
    register_mag_callback(pov_hall_sync);

    // 啟動 hrtimer (預設 10ms，轉速進來後會被 ISR 自動校正)
    // timer_interval = ktime_set(0, 10000000); // 10ms
    timer_interval = ktime_set(0, 1200000); // 5rps
    hrtimer_init(&pov_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    pov_timer.function = &pov_timer_callback;
    hrtimer_start(&pov_timer, timer_interval, HRTIMER_MODE_REL);

    pr_info("POV: Driver initialized successfully.\n");
    return 0;
}

// --- 6. 設備移除時的清理 ---
static void pov_spi_remove(struct spi_device *spi)
{
    hrtimer_cancel(&pov_timer);
    unregister_mag_callback();

    apa102_off();
    
    device_destroy(pov_class, pov_dev_num);
    class_destroy(pov_class);
    cdev_del(&pov_cdev);
    unregister_chrdev_region(pov_dev_num, 1);
    
    pr_info("POV: SPI Driver removed. Resources freed.\n");
}

static const struct of_device_id pov_dt_ids[] = {
    {.compatible = "custom,pov_apa102"},
    {} 
};
MODULE_DEVICE_TABLE(of, pov_dt_ids);

static struct spi_driver pov_spi_driver = {
    .driver = {
        .name = "pov_apa102",
        .of_match_table = pov_dt_ids,
    },
    .probe = pov_spi_probe,
    .remove = pov_spi_remove,
};

module_spi_driver(pov_spi_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("bloodtrail0511");
MODULE_DESCRIPTION("Async SPI & Auto-Sync Driver for APA102 POV Display");