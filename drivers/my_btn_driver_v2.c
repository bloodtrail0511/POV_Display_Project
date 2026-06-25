#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/uaccess.h>      // for copy_to_user
#include <linux/workqueue.h>    // for delayed_work
#include <linux/spinlock.h>

/*
 * 目前按鈕 GPIO 設定
 * 注意：這裡是 BCM GPIO 編號，不是實體 pin 編號。
 */
// #define GPIO_LEFT    17
// #define GPIO_RIGHT   5
// #define GPIO_CONFIRM 25
// #define GPIO_CANCEL  16
#define GPIO_LEFT 27
#define GPIO_RIGHT 22
#define GPIO_CONFIRM 23
#define GPIO_CANCEL 24

/*
 * delayed work debounce 時間
 * 20ms 通常可以濾掉大部分機械按鈕彈跳。
 * 如果還會彈跳，可以改成 30 或 50。
 */
#define DEBOUNCE_DELAY_MS 20

dev_t dev = 0;
static struct class *dev_class;
static struct cdev my_cdev;

/*
 * FASYNC 結構，用來發送 SIGIO 給 userspace
 */
static struct fasync_struct *async_queue;

/*
 * last_pressed_key 由 delayed work 寫入，由 read() 讀取。
 * 用 spinlock 保護，避免 read 與 work 同時存取。
 */
static uint8_t last_pressed_key = 0;
static DEFINE_SPINLOCK(key_lock);

/*
 * 每顆按鈕各自保存 GPIO、IRQ、按鍵字元、pressed 狀態與 delayed work。
 * pressed:
 *   false = 目前是放開狀態
 *   true  = 目前是按下狀態
 */
struct btn_info {
    int gpio;
    int irq;
    uint8_t key;
    const char *name;
    bool pressed;
    struct delayed_work work;
};

static struct btn_info buttons[] = {
    { .gpio = GPIO_LEFT,    .irq = -1, .key = 'a', .name = "btn_left",    .pressed = false },
    { .gpio = GPIO_RIGHT,   .irq = -1, .key = 'd', .name = "btn_right",   .pressed = false },
    { .gpio = GPIO_CONFIRM, .irq = -1, .key = 'w', .name = "btn_confirm", .pressed = false },
    { .gpio = GPIO_CANCEL,  .irq = -1, .key = 'b', .name = "btn_cancel",  .pressed = false },
};

#define NUM_BUTTONS ARRAY_SIZE(buttons)

static int __init my_btn_driver_init(void);
static void __exit my_btn_driver_exit(void);

/*************** Driver functions **********************/
static int my_btn_open(struct inode *inode, struct file *file);
static int my_btn_release(struct inode *inode, struct file *file);
static ssize_t my_btn_read(struct file *filp, char __user *buf, size_t len, loff_t *off);
static int my_btn_fasync(int fd, struct file *filp, int mode);
/******************************************************/

static struct file_operations fops =
{
    .owner = THIS_MODULE,
    .read = my_btn_read,
    .open = my_btn_open,
    .release = my_btn_release,
    .fasync = my_btn_fasync,
};

static int my_btn_open(struct inode *inode, struct file *file)
{
    pr_info("Device File Opened...!!!\n");
    return 0;
}

static int my_btn_release(struct inode *inode, struct file *file)
{
    /*
     * 當檔案關閉時，把 FASYNC 佇列清空
     */
    fasync_helper(-1, file, 0, &async_queue);
    pr_info("Device File Closed...!!!\n");
    return 0;
}

/*
 * 支援 userspace 設定 FASYNC
 */
static int my_btn_fasync(int fd, struct file *filp, int mode)
{
    return fasync_helper(fd, filp, mode, &async_queue);
}

/*
 * Userspace 收到 SIGIO 後，呼叫 read() 讀取按鍵值。
 */
static ssize_t my_btn_read(struct file *filp, char __user *buf, size_t len, loff_t *off)
{
    uint8_t key;
    unsigned long flags;

    if (len < 1)
        return -EINVAL;

    spin_lock_irqsave(&key_lock, flags);
    key = last_pressed_key;

    /*
     * 讀完之後清空，避免重複讀取。
     */
    last_pressed_key = 0;
    spin_unlock_irqrestore(&key_lock, flags);

    if (copy_to_user(buf, &key, 1))
        return -EFAULT;

    return 1;
}

/*************** Button debounce work **********************/

/*
 * debounce work 會在中斷後延遲 DEBOUNCE_DELAY_MS 執行。
 * 此時再讀 GPIO，判斷按鈕最後穩定狀態。
 *
 * 電路假設：
 *   GPIO 上拉到 3.3V
 *   按下時接 GND
 *
 * 所以：
 *   gpio_get_value() == 0 代表按下
 *   gpio_get_value() == 1 代表放開
 */
static void btn_debounce_work(struct work_struct *work)
{
    struct btn_info *btn;
    int value;
    unsigned long flags;

    btn = container_of(to_delayed_work(work), struct btn_info, work);

    value = gpio_get_value(btn->gpio);

    /*
     * 使用上拉電阻：
     * value == 0 代表按下
     * value == 1 代表放開
     */
    if (value == 0) {
        /*
         * 只有 released -> pressed 這一刻才送出按鍵事件。
         * 如果已經是 pressed，代表長按中，不重複送。
         */
        if (!btn->pressed) {
            btn->pressed = true;

            spin_lock_irqsave(&key_lock, flags);
            last_pressed_key = btn->key;
            spin_unlock_irqrestore(&key_lock, flags);

            pr_info("Button pressed: %c\n", btn->key);

            if (async_queue) {
                kill_fasync(&async_queue, SIGIO, POLL_IN);
            }
        }

        /*
         * 重點：
         * 只要按鈕還是低電位，就繼續排下一次檢查。
         * 這樣即使放開時 rising interrupt 沒有觸發，
         * delayed work 也會自己在下一次檢查時發現已經 release。
         */
        mod_delayed_work(system_wq,
                         &btn->work,
                         msecs_to_jiffies(DEBOUNCE_DELAY_MS));

    } else {
        /*
         * 放開只更新狀態，不送按鍵事件。
         */
        // if (btn->pressed) {
        //     btn->pressed = false;
        //     pr_info("Button released: %c\n", btn->key);
        // }
        if (btn->pressed) {
            btn->pressed = false;
            pr_info("Button released: %c\n", btn->key);

            // 💡 新增這段：把字元減去 32 (ASCII 轉大寫)，並發送 SIGIO
            spin_lock_irqsave(&key_lock, flags);
            last_pressed_key = btn->key - 32; // 'a' -> 'A', 'd' -> 'D'
            spin_unlock_irqrestore(&key_lock, flags);

            if (async_queue) {
                kill_fasync(&async_queue, SIGIO, POLL_IN);
            }
        }
    }
}
// static void btn_debounce_work(struct work_struct *work)
// {
//     struct btn_info *btn;
//     int value;
//     unsigned long flags;

//     btn = container_of(to_delayed_work(work), struct btn_info, work);

//     value = gpio_get_value(btn->gpio);

//     if (value == 0) {
//         /*
//          * 只有 released -> pressed 這一刻才送出按鍵事件。
//          * 如果已經是 pressed，代表長按中或彈跳，不重複送。
//          */
//         if (!btn->pressed) {
//             btn->pressed = true;

//             spin_lock_irqsave(&key_lock, flags);
//             last_pressed_key = btn->key;
//             spin_unlock_irqrestore(&key_lock, flags);

//             pr_info("Button pressed: %c\n", btn->key);

//             /*
//              * 通知 userspace 有按鍵事件
//              */
//             if (async_queue) {
//                 kill_fasync(&async_queue, SIGIO, POLL_IN);
//             }
//         }
//     } else {
//         /*
//          * 放開只更新狀態，不送按鍵事件。
//          * 這樣可以避免「長按放開時又觸發一次」。
//          */
//         if (btn->pressed) {
//             btn->pressed = false;
//             pr_info("Button released: %c\n", btn->key);
//         }
//     }
// }

/*************** 中斷服務常式 ISR **********************/

/*
 * ISR 只負責排 delayed work，不在 ISR 裡直接判斷按下。
 * 這樣可以避免機械彈跳造成多次觸發。
 */
static irqreturn_t btn_isr(int irq, void *dev_id)
{
    struct btn_info *btn = dev_id;

    /*
     * mod_delayed_work 的效果：
     * 如果彈跳期間一直觸發中斷，它會一直把 work 延後，
     * 最後只在訊號穩定 DEBOUNCE_DELAY_MS 後執行一次。
     */
    mod_delayed_work(system_wq,
                     &btn->work,
                     msecs_to_jiffies(DEBOUNCE_DELAY_MS));

    return IRQ_HANDLED;
}

static int __init my_btn_driver_init(void)
{
    int ret;
    int i;
    int requested_gpio_count = 0;
    int requested_irq_count = 0;

    /*
     * Allocating Major number
     */
    ret = alloc_chrdev_region(&dev, 0, 1, "my_btn_dev");
    if (ret < 0) {
        pr_err("Cannot allocate major number\n");
        return ret;
    }

    pr_info("Major = %d Minor = %d \n", MAJOR(dev), MINOR(dev));

    /*
     * Creating cdev structure
     */
    cdev_init(&my_cdev, &fops);

    /*
     * Adding character device to the system
     */
    ret = cdev_add(&my_cdev, dev, 1);
    if (ret < 0) {
        pr_err("Cannot add the device to the system\n");
        goto r_cdev;
    }

    /*
     * Creating struct class
     */
    dev_class = class_create(THIS_MODULE, "my_btn_class");
    if (dev_class == NULL) {
        pr_err("Cannot create the struct class\n");
        ret = -ENOMEM;
        goto r_class;
    }

    /*
     * Creating device
     */
    if (device_create(dev_class, NULL, dev, NULL, "my_btn") == NULL) {
        pr_err("Cannot create the Device\n");
        ret = -ENOMEM;
        goto r_device;
    }

    /*
     * 檢查 GPIO、request GPIO、設成 input、初始化 delayed work
     */
    for (i = 0; i < NUM_BUTTONS; i++) {
        if (!gpio_is_valid(buttons[i].gpio)) {
            pr_err("GPIO %d is not valid\n", buttons[i].gpio);
            ret = -EINVAL;
            goto r_gpio;
        }

        ret = gpio_request(buttons[i].gpio, buttons[i].name);
        if (ret < 0) {
            pr_err("ERROR: GPIO %d request failed\n", buttons[i].gpio);
            goto r_gpio;
        }

        requested_gpio_count++;

        ret = gpio_direction_input(buttons[i].gpio);
        if (ret < 0) {
            pr_err("ERROR: GPIO %d direction input failed\n", buttons[i].gpio);
            goto r_gpio;
        }

        /*
         * 初始化 pressed 狀態。
         * active low: value 0 表示目前已經被按下。
         */
        buttons[i].pressed = (gpio_get_value(buttons[i].gpio) == 0);

        INIT_DELAYED_WORK(&buttons[i].work, btn_debounce_work);
    }

    /*
     * GPIO 轉 IRQ 並註冊雙邊緣中斷。
     * 雙邊緣原因：
     *   falling: 偵測按下
     *   rising : 偵測放開，讓 pressed 狀態可以被清回 false
     */
    for (i = 0; i < NUM_BUTTONS; i++) {
        buttons[i].irq = gpio_to_irq(buttons[i].gpio);
        if (buttons[i].irq < 0) {
            pr_err("ERROR: gpio_to_irq failed for GPIO %d\n", buttons[i].gpio);
            ret = buttons[i].irq;
            goto r_irq;
        }

        ret = request_irq(buttons[i].irq,
                          btn_isr,
                          IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
                          buttons[i].name,
                          &buttons[i]);

        if (ret) {
            pr_err("my_btn: Failed to request irq for %s GPIO=%d irq=%d\n",
                   buttons[i].name, buttons[i].gpio, buttons[i].irq);
            goto r_irq;
        }

        requested_irq_count++;

        pr_info("my_btn: %s GPIO=%d irq=%d initial_value=%d pressed=%d\n",
                buttons[i].name,
                buttons[i].gpio,
                buttons[i].irq,
                gpio_get_value(buttons[i].gpio),
                buttons[i].pressed);
    }

    pr_info("Device Driver Insert...Done!!!\n");
    return 0;

r_irq:
    for (i = 0; i < requested_irq_count; i++) {
        free_irq(buttons[i].irq, &buttons[i]);
    }

r_gpio:
    for (i = 0; i < NUM_BUTTONS; i++) {
        cancel_delayed_work_sync(&buttons[i].work);
    }

    for (i = 0; i < requested_gpio_count; i++) {
        gpio_free(buttons[i].gpio);
    }

    device_destroy(dev_class, dev);

r_device:
    class_destroy(dev_class);

r_class:
    cdev_del(&my_cdev);

r_cdev:
    unregister_chrdev_region(dev, 1);

    return ret;
}

static void __exit my_btn_driver_exit(void)
{
    int i;

    /*
     * 先取消 delayed work，避免 module remove 時 work 還在跑。
     */
    for (i = 0; i < NUM_BUTTONS; i++) {
        cancel_delayed_work_sync(&buttons[i].work);
    }

    /*
     * 釋放中斷，dev_id 必須和 request_irq 時一樣。
     */
    for (i = 0; i < NUM_BUTTONS; i++) {
        if (buttons[i].irq >= 0) {
            free_irq(buttons[i].irq, &buttons[i]);
        }
    }

    /*
     * 釋放 GPIO
     */
    for (i = 0; i < NUM_BUTTONS; i++) {
        gpio_free(buttons[i].gpio);
    }

    device_destroy(dev_class, dev);
    class_destroy(dev_class);
    cdev_del(&my_cdev);
    unregister_chrdev_region(dev, 1);

    pr_info("Device Driver Remove...Done!!\n");
}

module_init(my_btn_driver_init);
module_exit(my_btn_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("bloodtrail0511 <beckham051188@gmail.com>");
MODULE_DESCRIPTION("A test driver for button isr");
MODULE_VERSION("0.02");