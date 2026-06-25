#include <iostream>
#include <cstring>
#include <cstdlib>

#include <unistd.h>
#include <termios.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define SERVER_PORT 8888

/*
 * pong_client.cpp 是外部玩家的控制端。
 *
 * 使用方式：
 * ./pong_client <server_ip>
 *
 * 它會連到 app_pong_multi.cpp 啟動的 TCP server，然後把鍵盤方向鍵轉成文字命令：
 * 上鍵 -> "UP\n"
 * 下鍵 -> "DOWN\n"
 * Q    -> "QUIT\n"
 */

static struct termios old_terminal;

// 程式結束時把 terminal 設定恢復，避免 shell 繼續維持 raw mode。
static void reset_terminal() {
    tcsetattr(STDIN_FILENO, TCSANOW, &old_terminal);
}

static void set_terminal_raw_mode() {
    struct termios new_terminal;

    tcgetattr(STDIN_FILENO, &old_terminal);
    new_terminal = old_terminal;

    // 關閉 canonical mode：不需要按 Enter。
    // 關閉 echo：按鍵不顯示在 terminal 上。
    new_terminal.c_lflag &= ~(ICANON | ECHO);

    tcsetattr(STDIN_FILENO, TCSANOW, &new_terminal);
    atexit(reset_terminal);
}

static bool send_command(int sock_fd, const char* command) {
    // 所有控制都用簡單文字協定送給 server，方便其他裝置照著實作。
    int ret = send(sock_fd, command, strlen(command), 0);

    if (ret < 0) {
        perror("send");
        return false;
    }

    return true;
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: ./pong_client <server_ip>\n";
        std::cerr << "Example: ./pong_client 192.168.1.50\n";
        return 1;
    }

    const char* server_ip = argv[1];

    // 建立 IPv4 TCP socket，之後用 connect() 連到 server_ip:8888。
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (sock_fd < 0) {
        perror("socket");
        return 1;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);

    // inet_pton() 把使用者輸入的 IP 字串轉成 sockaddr_in 使用的 binary 格式。
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        std::cerr << "Invalid server IP: " << server_ip << "\n";
        close(sock_fd);
        return 1;
    }

    // 連線成功後，server 會把這個 client 分配成 Player 3 或 Player 4。
    if (connect(sock_fd, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sock_fd);
        return 1;
    }

    std::cout << "Connected to Pong server: " << server_ip << ":" << SERVER_PORT << "\n";
    std::cout << "Controls:\n";
    std::cout << "  Arrow Up    -> UP\n";
    std::cout << "  Arrow Down  -> DOWN\n";
    std::cout << "  Q           -> QUIT\n";

    // raw mode 讓 read() 可以即時收到按鍵，不需要使用者按 Enter。
    set_terminal_raw_mode();

    while (true) {
        unsigned char ch = 0;

        if (read(STDIN_FILENO, &ch, 1) <= 0) {
            continue;
        }

        // 方向鍵在 terminal 中通常是 escape sequence：
        // Up    = ESC [ A
        // Down  = ESC [ B
        if (ch == 27) { // ESC
            unsigned char seq[2] = {0, 0};

            if (read(STDIN_FILENO, &seq[0], 1) <= 0) {
                continue;
            }

            if (read(STDIN_FILENO, &seq[1], 1) <= 0) {
                continue;
            }

            // 每按一次方向鍵只送一次命令；server 讀完後會自動清掉該次輸入。
            if (seq[0] == '[' && seq[1] == 'A') {
                if (!send_command(sock_fd, "UP\n")) {
                    break;
                }
            }
            else if (seq[0] == '[' && seq[1] == 'B') {
                if (!send_command(sock_fd, "DOWN\n")) {
                    break;
                }
            }
        }
        else if (ch == 'q' || ch == 'Q') {
            // QUIT 只讓這個 client 離線，不會關閉整個遊戲 server。
            send_command(sock_fd, "QUIT\n");
            break;
        }
    }

    close(sock_fd);
    return 0;
}
