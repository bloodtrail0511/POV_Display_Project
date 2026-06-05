#include "net_server.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <thread>

#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

/*
 * net_server.cpp 是遊戲的 TCP 輸入層。
 *
 * 資料流：
 * pong_client 送出 "UP\n" / "DOWN\n" / "QUIT\n"
 * -> client_handler() 收到文字命令
 * -> 更新 net_players[player_id] 的 atomic 狀態
 * -> app_pong_multi.cpp 的主迴圈讀取狀態並移動 P3 / P4 擋板
 */

NetPlayerInput::NetPlayerInput() {
    connected.store(false);
    up_pressed.store(false);
    down_pressed.store(false);
}

NetPlayerInput net_players[MAX_NET_PLAYERS];

/*
 * 這個 mutex 只保護「玩家 ID / slot 分配」。
 * 不保護 up_pressed / down_pressed，因為它們本身是 atomic<bool>。
 */
static std::mutex player_alloc_mutex;
static std::atomic<bool> server_running(false);
static int server_fd = -1;

// 找一個尚未連線的玩家 slot。
// 回傳 0 代表 P3，回傳 1 代表 P4；沒有空位則回傳 -1。
static int allocate_player_id() {
    std::lock_guard<std::mutex> lock(player_alloc_mutex);

    for (int i = 0; i < MAX_NET_PLAYERS; i++) {
        bool expected = false;

        // compare_exchange_strong 可以避免兩個 client 同時搶到同一個 slot。
        if (net_players[i].connected.compare_exchange_strong(expected, true)) {
            net_players[i].up_pressed.store(false);
            net_players[i].down_pressed.store(false);
            return i;
        }
    }

    return -1;
}

static void release_player_id(int player_id) {
    if (player_id < 0 || player_id >= MAX_NET_PLAYERS) {
        return;
    }

    // client 離線時清掉輸入，避免下一位接手同 slot 時繼承舊狀態。
    net_players[player_id].up_pressed.store(false);
    net_players[player_id].down_pressed.store(false);
    net_players[player_id].connected.store(false);
}

static void client_handler(int client_fd, int player_id) {
    char buffer[128];
    char welcome[128];
    snprintf(
        welcome,
        sizeof(welcome),
        "Connected to POV Pong server.\n"
        "You are Player %d.\n"
        "Commands: UP, DOWN, QUIT\n",
        player_id + 3
    );

    send(client_fd, welcome, strlen(welcome), 0);

    printf("[NET] Player %d connected\n", player_id + 3);

    // 每個 client 都由獨立 thread 處理；這個 loop 只負責該玩家的 socket。
    while (server_running.load()) {
        memset(buffer, 0, sizeof(buffer));

        // recv() 不放進 mutex；否則單一慢 client 可能阻塞其他資料處理。
        int n = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

        if (n <= 0) {
            break;
        }

        buffer[n] = '\0';

        // 指令是文字協定，外部硬體只要 TCP 送這幾個字串也能控制遊戲。
        if (strstr(buffer, "UP")) {
            net_players[player_id].up_pressed.store(true);
            net_players[player_id].down_pressed.store(false);
        }
        else if (strstr(buffer, "DOWN")) {
            net_players[player_id].up_pressed.store(false);
            net_players[player_id].down_pressed.store(true);
        }
        else if (strstr(buffer, "QUIT")) {
            break;
        }
    }

    close(client_fd);
    release_player_id(player_id);

    printf("[NET] Player %d disconnected\n", player_id + 3);
}

static void accept_loop() {
    // 背景 accept loop：server 開著時持續接受新連線，並分配 P3/P4。
    while (server_running.load()) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);

        if (client_fd < 0) {
            if (server_running.load()) {
                perror("[NET] accept");
            }
            continue;
        }

        int player_id = allocate_player_id();

        if (player_id < 0) {
            const char* full_msg = "Server full. Only two network players are enabled now.\n";
            send(client_fd, full_msg, strlen(full_msg), 0);
            close(client_fd);
            printf("[NET] Reject client from %s: server full\n", inet_ntoa(client_addr.sin_addr));
            continue;
        }

        printf("[NET] New client from %s assigned to Player %d\n",
               inet_ntoa(client_addr.sin_addr),
               player_id + 3);

        // detach 後 client_handler 自己負責關閉 client_fd 與釋放 player slot。
        std::thread(client_handler, client_fd, player_id).detach();
    }
}

void start_network_server() {
    if (server_running.load()) {
        return;
    }

    // 建立 IPv4 TCP socket，所有 client 都連到 SERVER_PORT。
    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0) {
        perror("[NET] socket");
        exit(1);
    }

    // 允許程式重開時快速重新 bind 同一個 port。
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(SERVER_PORT);

    if (bind(server_fd, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("[NET] bind");
        close(server_fd);
        exit(1);
    }

    if (listen(server_fd, MAX_NET_PLAYERS) < 0) {
        perror("[NET] listen");
        close(server_fd);
        exit(1);
    }

    // server_running 先設 true，再啟動 accept thread。
    server_running.store(true);
    std::thread(accept_loop).detach();

    printf("[NET] Socket server started on port %d\n", SERVER_PORT);
}

void stop_network_server() {
    // 通知 accept/client threads 停止；close(server_fd) 會讓阻塞中的 accept() 醒來。
    server_running.store(false);

    if (server_fd >= 0) {
        close(server_fd);
        server_fd = -1;
    }

    for (int i = 0; i < MAX_NET_PLAYERS; i++) {
        release_player_id(i);
    }

    printf("[NET] Socket server stopped\n");
}
