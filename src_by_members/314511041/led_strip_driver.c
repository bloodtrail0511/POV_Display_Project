/***************************************************************************
 *  \file       led_strip_driver.c
 *  \details    test driver for Hall effect sensor
 *  \author     bloodtrail0511
 ****************************************************************************/

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/gpio.h>
#include <linux/delay.h>

#define GPIO_DATA_0 23
#define GPIO_DATA_1 24
#define GPIO_CLK    25

// --- 核心技術：軟體模擬送出 1 Byte (Bit-banging) ---
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

static int __init apa102_init(void) {
    pr_info("APA102: Initializing bit-bang driver...\n");

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

    // 模組載入時，立刻送出一波測試訊號
    apa102_update_strip();

    return 0;
}

static void __exit apa102_exit(void) {
    gpio_set_value(GPIO_DATA_0, 0);
    gpio_set_value(GPIO_DATA_1, 0);
    gpio_set_value(GPIO_CLK, 0);
    gpio_free(GPIO_DATA_0);
    gpio_free(GPIO_DATA_1);
    gpio_free(GPIO_CLK);
    pr_info("APA102: Driver removed.\n");
}

module_init(apa102_init);
module_exit(apa102_exit);

MODULE_LICENSE("GPL");