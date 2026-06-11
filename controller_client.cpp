#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int sock = -1;
int btn_fd = -1;
volatile sig_atomic_t g_stop = 0;

void sigint_handler(int sig) {
    g_stop = 1;
}

// 💡 Interrupt & Signal: 收到 Driver 傳來的 SIGIO 訊號時觸發
void button_signal_handler(int sig) {
    if (btn_fd < 0 || sock < 0) return;

    uint8_t key_code = 0;
    // 從 Driver 讀取按下的字母
    ssize_t n = read(btn_fd, &key_code, 1); 
    
    if (n > 0 && key_code != 0) {
        // 直接透過網路 Socket 傳送給 Server！
        // send(sock, &key_code, 1, 0);
        // printf("[Gamepad] Sent key: %c\n", key_code);
        ssize_t sent = send(sock, &key_code, 1, 0);
        if (sent <= 0) {
            perror("[Gamepad] send failed, server disconnected");
            g_stop = 1;
            return;
        }

        printf("[Gamepad] Sent key: %c\n", key_code);
    }
}

int main(int argc, char const *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <Server_IP>\n", argv[0]);
        return -1;
    }
    // 安裝driver
    system("sudo insmod my_btn_driver_v2.ko 2>/dev/null");

    signal(SIGINT, sigint_handler);
    signal(SIGPIPE, SIG_IGN);

    // 1. 開啟按鈕 Driver
    btn_fd = open("/dev/my_btn", O_RDWR);
    if (btn_fd < 0) {
        perror("Failed to open /dev/my_btn (Did you insmod?)");
        return -1;
    }

    // 2. 註冊 SIGIO 訊號
    signal(SIGIO, button_signal_handler);
    fcntl(btn_fd, F_SETOWN, getpid());
    int flags = fcntl(btn_fd, F_GETFL);
    fcntl(btn_fd, F_SETFL, flags | FASYNC);

    // 3. 建立 TCP Socket 連線
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return -1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(8888);

    // 💡 核心改進：使用 getaddrinfo 自動解析 Hostname 或 IP
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;       // 只拿 IPv4
    hints.ai_socktype = SOCK_STREAM; // TCP


    printf("Resolving and connecting to Server '%s:8888'...\n", argv[1]);
    
    // getaddrinfo 會自動判斷 argv[1] 是 "admin" 還是 "192.168.x.x"
    // 如果是 hostname，它會自動去問系統 DNS 幫你轉成真實 IP
    if (getaddrinfo(argv[1], NULL, &hints, &res) != 0) {
        printf("Error: Could not resolve hostname '%s'\n", argv[1]);
        close(btn_fd);
        return -1;
    }

    // 把解析出來的 IP 地址複製到我們的 serv_addr 結構中
    struct sockaddr_in *ipv4 = (struct sockaddr_in *)res->ai_addr;
    serv_addr.sin_addr = ipv4->sin_addr;
    freeaddrinfo(res); // 釋放系統動態配置的記憶體 (Memory Management)

    // 開始連線
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Connection Failed");
        close(btn_fd);
        return -1;
    }

    printf("[Gamepad] Connected successfully! Press buttons to control the Menu.\n");

    // 4. 主程式只要睡覺就好，全部交給非同步的 SIGIO 處理！
    while (!g_stop) {
        pause(); 
    }

    printf("\nClosing Gamepad Client...\n");
    system("sudo rmmod my_btn_driver_v2 2>/dev/null");
    close(sock);
    close(btn_fd);
    return 0;
}