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
#include <linux/uaccess.h> // for copy_to_user

// #define GPIO_LEFT 27
// #define GPIO_RIGHT 22
// #define GPIO_CONFIRM 23
// #define GPIO_CANCEL 24
#define GPIO_LEFT 17
#define GPIO_RIGHT 5
#define GPIO_CONFIRM 25
#define GPIO_CANCEL 16
#define DEBOUNCE_TIME_NS 100000000 // 100ms

dev_t dev = 0;
static struct class *dev_class;
static struct cdev my_cdev;

// 儲存中斷號碼
static int irq_left, irq_right, irq_confirm, irq_cancel;

// 防彈跳與按鍵狀態
static ktime_t last_trigger_time;
static uint8_t last_pressed_key = 0; // 用來存哪顆按鍵被按下了 ('a', 'd', 'w', 'b')

// FASYNC 結構 (發送 Signal 的核心)
struct fasync_struct *async_queue;

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
    // 當檔案關閉時，把 FASYNC 佇列清空
    fasync_helper(-1, file, 0, &async_queue);
    pr_info("Device File Closed...!!!\n");
    return 0;
}

// 支援 Userspace 設定 FASYNC
static int my_btn_fasync(int fd, struct file *filp, int mode)
{
    return fasync_helper(fd, filp, mode, &async_queue);
}

// 當 Userspace 收到 Signal 後，會呼叫 read 來拿按鍵值
static ssize_t my_btn_read(struct file *filp, char __user *buf, size_t len, loff_t *off)
{
    if (len < 1)
        return -EINVAL;
    if (copy_to_user(buf, &last_pressed_key, 1))
        return -EFAULT;

    // 讀完之後清空，避免重複讀取
    last_pressed_key = 0;
    return 1;
}

/*************** 中斷服務常式 (ISR) **********************/
// 注意：我們在 request_irq 時，把 dev_id 當作按鍵字元 ('a', 'd'...) 傳進來
static irqreturn_t btn_isr(int irq, void *dev_id)
{
    ktime_t now = ktime_get();
    s64 time_diff_ns = ktime_to_ns(ktime_sub(now, last_trigger_time));

    // 防彈跳過濾
    if (time_diff_ns < DEBOUNCE_TIME_NS)
    {
        return IRQ_HANDLED;
    }

    last_trigger_time = now;

    // 取得是哪顆按鍵觸發的 (把 dev_id 轉回字元)
    last_pressed_key = (uint8_t)(uintptr_t)dev_id;
    pr_info("Button pressed: %c\n", last_pressed_key);

    // 發送 SIGIO 訊號給 Userspace 的 C++ 程式！
    if (async_queue)
    {
        kill_fasync(&async_queue, SIGIO, POLL_IN);
    }

    return IRQ_HANDLED;
}

static int __init my_btn_driver_init(void)
{
    /*Allocating Major number*/
    if ((alloc_chrdev_region(&dev, 0, 1, "my_btn_dev")) < 0)
    {
        pr_err("Cannot allocate major number\n");
        goto r_unreg;
    }
    pr_info("Major = %d Minor = %d \n", MAJOR(dev), MINOR(dev));

    /*Creating cdev structure*/
    cdev_init(&my_cdev, &fops);

    /*Adding character device to the system*/
    if ((cdev_add(&my_cdev, dev, 1)) < 0)
    {
        pr_err("Cannot add the device to the system\n");
        goto r_del;
    }

    /*Creating struct class*/
    if ((dev_class = class_create(THIS_MODULE, "my_btn_class")) == NULL)
    {
        pr_err("Cannot create the struct class\n");
        goto r_class;
    }

    /*Creating device*/
    if ((device_create(dev_class, NULL, dev, NULL, "my_btn")) == NULL)
    {
        pr_err("Cannot create the Device \n");
        goto r_device;
    }

    // Checking the GPIO is valid or not
    if (gpio_is_valid(GPIO_LEFT) == false)
    {
        pr_err("GPIO %d is not valid\n", GPIO_LEFT);
        goto r_device;
    }
    if (gpio_is_valid(GPIO_RIGHT) == false)
    {
        pr_err("GPIO %d is not valid\n", GPIO_RIGHT);
        goto r_device;
    }
    if (gpio_is_valid(GPIO_CONFIRM) == false)
    {
        pr_err("GPIO %d is not valid\n", GPIO_CONFIRM);
        goto r_device;
    }
    if (gpio_is_valid(GPIO_CANCEL) == false)
    {
        pr_err("GPIO %d is not valid\n", GPIO_CANCEL);
        goto r_device;
    }

    // Requesting the GPIO
    if (gpio_request(GPIO_LEFT, "GPIO_LEFT") < 0)
    {
        pr_err("ERROR: GPIO %d request\n", GPIO_LEFT);
        goto r_gpio;
    }
    if (gpio_request(GPIO_RIGHT, "GPIO_RIGHT") < 0)
    {
        pr_err("ERROR: GPIO %d request\n", GPIO_RIGHT);
        goto r_gpio;
    }
    if (gpio_request(GPIO_CONFIRM, "GPIO_CONFIRM") < 0)
    {
        pr_err("ERROR: GPIO %d request\n", GPIO_CONFIRM);
        goto r_gpio;
    }
    if (gpio_request(GPIO_CANCEL, "GPIO_CANCEL") < 0)
    {
        pr_err("ERROR: GPIO %d request\n", GPIO_CANCEL);
        goto r_gpio;
    }

    // configure the GPIO as input
    gpio_direction_input(GPIO_LEFT);
    gpio_direction_input(GPIO_RIGHT);
    gpio_direction_input(GPIO_CONFIRM);
    gpio_direction_input(GPIO_CANCEL);

    // 1. 將 GPIO 轉換為 IRQ Number
    irq_left = gpio_to_irq(GPIO_LEFT);
    irq_right = gpio_to_irq(GPIO_RIGHT);
    irq_confirm = gpio_to_irq(GPIO_CONFIRM);
    irq_cancel = gpio_to_irq(GPIO_CANCEL);

    // 2. 註冊中斷 (因為有上拉電阻，按下時電位從 HIGH 變 LOW，所以觸發條件是 IRQF_TRIGGER_FALLING)
    // 最後一個參數 dev_id 我們巧妙地傳入對應的字元 ('a', 'd', 'w', 'b')，這樣 ISR 裡就能分辨是誰按的
int ret; // 在函式最上方宣告也可以，或者在這裡宣告

    // 2. 註冊中斷並強制檢查回傳值
    ret = request_irq(irq_left, btn_isr, IRQF_TRIGGER_FALLING, "btn_left", (void *)(uintptr_t)'a');
    if (ret) {
        pr_err("my_btn: Failed to request irq_left\n");
        goto r_gpio;
    }

    ret = request_irq(irq_right, btn_isr, IRQF_TRIGGER_FALLING, "btn_right", (void *)(uintptr_t)'d');
    if (ret) {
        pr_err("my_btn: Failed to request irq_right\n");
        free_irq(irq_left, (void *)(uintptr_t)'a'); // 清理上一個成功的
        goto r_gpio;
    }

    ret = request_irq(irq_confirm, btn_isr, IRQF_TRIGGER_FALLING, "btn_confirm", (void *)(uintptr_t)'w');
    if (ret) {
        pr_err("my_btn: Failed to request irq_confirm\n");
        free_irq(irq_right, (void *)(uintptr_t)'d');
        free_irq(irq_left, (void *)(uintptr_t)'a');
        goto r_gpio;
    }

    ret = request_irq(irq_cancel, btn_isr, IRQF_TRIGGER_FALLING, "btn_cancel", (void *)(uintptr_t)'b');
    if (ret) {
        pr_err("my_btn: Failed to request irq_cancel\n");
        free_irq(irq_confirm, (void *)(uintptr_t)'w');
        free_irq(irq_right, (void *)(uintptr_t)'d');
        free_irq(irq_left, (void *)(uintptr_t)'a');
        goto r_gpio;
    }

    last_trigger_time = ktime_get();

    pr_info("Device Driver Insert...Done!!!\n");
    return 0;

r_gpio:
    gpio_free(GPIO_LEFT);
    gpio_free(GPIO_RIGHT);
    gpio_free(GPIO_CONFIRM);
    gpio_free(GPIO_CANCEL);
r_device:
    device_destroy(dev_class, dev);
r_class:
    class_destroy(dev_class);
r_del:
    cdev_del(&my_cdev);
r_unreg:
    unregister_chrdev_region(dev, 1);

    return -1;
}

static void __exit my_btn_driver_exit(void)
{
    // 釋放中斷 (必須在 free GPIO 之前)
    free_irq(irq_left, (void *)(uintptr_t)'a');
    free_irq(irq_right, (void *)(uintptr_t)'d');
    free_irq(irq_confirm, (void *)(uintptr_t)'w');
    free_irq(irq_cancel, (void *)(uintptr_t)'b');

    gpio_free(GPIO_LEFT);
    gpio_free(GPIO_RIGHT);
    gpio_free(GPIO_CONFIRM);
    gpio_free(GPIO_CANCEL);
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
MODULE_VERSION("0.01");