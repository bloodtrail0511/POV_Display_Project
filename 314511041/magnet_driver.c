/***************************************************************************
 *  \file       magnet_driver.c
 *  \details    test driver for Hall effect sensor
 *  \author     bloodtrail0511
 ****************************************************************************/

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/uaccess.h> //copy_to/from_user()
#include <linux/gpio.h>    //GPIO
#include <linux/interrupt.h>
#include <linux/ktime.h> // 高精度時間for debounce

#define GPIO_DO 21 // digital out

dev_t dev = 0;
static struct class *dev_class;
static struct cdev my_cdev;
static short int mag_irq = 0;
static ktime_t last_trigger_time;
static int stable_hall_count = 0; // 給userspace使用，如果cnt > threshold -> 開機，cnt < threshold -> 關機
// 新增：定義冷卻時間 (Debounce Threshold)。
// 單位是奈秒 (ns)。5,000,000 ns = 5 毫秒 (ms)
// 根據馬達的實際最高轉速來微調
// #define DEBOUNCE_TIME_NS 5000000
// #define DEBOUNCE_TIME_NS 1000000000 // 1 sec
// #define DEBOUNCE_TIME_NS 10000000 // 10 ms
// #define DEBOUNCE_TIME_NS 80000000 // 20 ms
#define MIN_VALID_PERIOD_NS 25000000LL  // 25 ms
// #define MAX_VALID_PERIOD_NS 100000000LL  // 100 ms
#define MAX_VALID_PERIOD_NS 70000000LL  // 70 ms

static int __init mag_driver_init(void);
static void __exit mag_driver_exit(void);

/*************** Driver functions **********************/
static int mag_open(struct inode *inode, struct file *file);
static int mag_release(struct inode *inode, struct file *file);
static ssize_t mag_read(struct file *filp,
                        char __user *buf, size_t len, loff_t *off);
static ssize_t mag_write(struct file *filp,
                         const char *buf, size_t len, loff_t *off);
/******************************************************/

// File operation structure
static struct file_operations fops =
    {
        .owner = THIS_MODULE,
        .read = mag_read,
        .write = mag_write,
        .open = mag_open,
        .release = mag_release,
};

static int mag_open(struct inode *inode, struct file *file)
{
    pr_info("Device File Opened...!!!\n");
    return 0;
}

static int mag_release(struct inode *inode, struct file *file)
{
    pr_info("Device File Closed...!!!\n");
    return 0;
}

#define HALL_TIMEOUT_NS 500000000LL  // 500 ms，可依轉速調整

static ssize_t mag_read(struct file *filp, char __user *buf, size_t len, loff_t *off)
{
    uint8_t data_to_send;
    ktime_t now;
    s64 time_since_last_ns;

    if (len < 1) {
        return -EINVAL;
    }

    now = ktime_get();
    time_since_last_ns = ktime_to_ns(ktime_sub(now, last_trigger_time));

    /*
     * 如果太久沒有 Hall 中斷，代表馬達停止或轉速太低。
     * 這裡一定要把 stable_hall_count 清掉，不然 user space 會誤判仍在旋轉。
     */
    if (time_since_last_ns > HALL_TIMEOUT_NS) {
        stable_hall_count = 0;
    }

    data_to_send = stable_hall_count;

    if (copy_to_user(buf, &data_to_send, 1)) {
        return -EFAULT;
    }

    return 1;
}

static ssize_t mag_write(struct file *filp, const char __user *buf, size_t len, loff_t *off)
{
    pr_info("Write Function : Device cannot be written.\n");
    return 0;
}

// 2. 宣告一個全域的函式指標，用來存儲 POV 驅動傳進來的函式
static void (*pov_sync_callback)(ktime_t current_time, s64 time_diff_ns) = NULL;

// 3. 提供註冊函式 (給 POV 驅動呼叫)
void register_mag_callback(void (*callback_func)(ktime_t, s64))
{
    pov_sync_callback = callback_func;
    pr_info("Magnet Driver: Callback registered successfully.\n");
}
EXPORT_SYMBOL(register_mag_callback); // 把這個函式開放給整個 Kernel

// 4. 提供解除註冊函式
void unregister_mag_callback(void)
{
    pov_sync_callback = NULL;
    pr_info("Magnet Driver: Callback unregistered.\n");
}
EXPORT_SYMBOL(unregister_mag_callback);

// static irqreturn_t mag_isr(int irq, void *data)
// {
//     // 1. 獲取當下的高精度時間
//     ktime_t now = ktime_get();

//     // 2. 計算與上一次觸發的時間差 (轉換為奈秒)
//     s64 time_diff_ns = ktime_to_ns(ktime_sub(now, last_trigger_time));

//     // 3. 防彈跳判斷：如果時間差小於設定的冷卻時間，視為雜訊
//     if (time_diff_ns < DEBOUNCE_TIME_NS) {
//         return IRQ_HANDLED; // 直接結束，什麼都不做
//     }

//     last_trigger_time = now;
//     // pr_info("magnetic interrupt !!!!\n");
//     pr_info("Hall trigger, diff = %lld ns\n", time_diff_ns);

//     // 如果有註冊 Callback 就呼叫
//     if (pov_sync_callback != NULL) {
//         pov_sync_callback(now, time_diff_ns);
//     }
    
//     return IRQ_HANDLED;
// }
static irqreturn_t mag_isr(int irq, void *data)
{
    ktime_t now = ktime_get();
    s64 time_diff_ns = ktime_to_ns(ktime_sub(now, last_trigger_time));

    if (time_diff_ns < MIN_VALID_PERIOD_NS) {
        return IRQ_HANDLED;
    }

    last_trigger_time = now;


    if (time_diff_ns > MAX_VALID_PERIOD_NS) {
        /*
         * 太久沒收到合理觸發，可能馬達變慢或剛啟動。
         * 這裡先更新 last_trigger_time，但不要 callback，
         * 避免拿異常週期去改 POV timer。
         */
        if (stable_hall_count > 0) {
            stable_hall_count--;
        }
        return IRQ_HANDLED;
    }
    if (stable_hall_count < 10) {
        stable_hall_count++;
    }

    pr_info("Valid Hall trigger, diff = %lld ns\n", time_diff_ns);

    if (pov_sync_callback != NULL) {
        pov_sync_callback(now, time_diff_ns);
    }

    return IRQ_HANDLED;
}


static int __init mag_driver_init(void)
{
    /*Allocating Major number*/
    if ((alloc_chrdev_region(&dev, 0, 1, "mag_dev")) < 0)
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
    if ((dev_class = class_create(THIS_MODULE, "mag_class")) == NULL)
    {
        pr_err("Cannot create the struct class\n");
        goto r_class;
    }

    /*Creating device*/
    if ((device_create(dev_class, NULL, dev, NULL, "mag_sensor")) == NULL)
    {
        pr_err("Cannot create the Device \n");
        goto r_device;
    }

    // Checking the GPIO is valid or not
    if (gpio_is_valid(GPIO_DO) == false)
    {
        pr_err("GPIO %d is not valid\n", GPIO_DO);
        goto r_device;
    }

    // Requesting the GPIO
    if (gpio_request(GPIO_DO, "GPIO_DO") < 0)
    {
        pr_err("ERROR: GPIO %d request\n", GPIO_DO);
        goto r_gpio;
    }

    // configure the GPIO as input
    gpio_direction_input(GPIO_DO);

    /* Using this call the GPIO 21 will be visible in /sys/class/gpio/
    ** Now you can change the gpio values by using below commands also.
    ** echo 1 > /sys/class/gpio/gpio21/value  (turn ON the LED)
    ** echo 0 > /sys/class/gpio/gpio21/value  (turn OFF the LED)
    ** cat /sys/class/gpio/gpio21/value  (read the value LED)
    **
    ** the second argument prevents the direction from being changed.
    */

    // ISR
    if( (mag_irq = gpio_to_irq(GPIO_DO)) < 0 ) {
        pr_err("GPIO to IRQ failed\n");
        goto r_gpio; 
    }
    if( request_irq(mag_irq, mag_isr, IRQF_TRIGGER_RISING, "mag_sensor_irq", NULL)) {
        pr_err("Cannot request IRQ\n");
        goto r_gpio; // 註冊失敗要跳到 r_gpio 釋放資源，不能直接 return -1
    }

    last_trigger_time = ktime_get();

    pr_info("Device Driver Insert...Done!!!\n");
    return 0;

r_gpio:
    gpio_free(GPIO_DO);
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

static void __exit mag_driver_exit(void)
{
    free_irq(mag_irq, NULL);
    gpio_free(GPIO_DO);
    device_destroy(dev_class, dev);
    class_destroy(dev_class);
    cdev_del(&my_cdev);
    unregister_chrdev_region(dev, 1);
    pr_info("Device Driver Remove...Done!!\n");
}

module_init(mag_driver_init);
module_exit(mag_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("bloodtrail0511 <beckham051188@gmail.com>");
MODULE_DESCRIPTION("A test driver for Hall effect sensor");
MODULE_VERSION("0.01");