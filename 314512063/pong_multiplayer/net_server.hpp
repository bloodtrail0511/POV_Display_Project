#ifndef NET_SERVER_HPP
#define NET_SERVER_HPP

#include <atomic>

#define SERVER_PORT 8888
#define MAX_NET_PLAYERS 2   // 目前支援兩位外部玩家：P3、P4

/*
 * 每位網路玩家的輸入狀態。
 *
 * client thread 寫入 connected / up_pressed / down_pressed。
 * main game loop 讀取這些 atomic 狀態，並在主迴圈內更新擋板角度。
 */
struct NetPlayerInput {
    std::atomic<bool> connected;
    std::atomic<bool> up_pressed;
    std::atomic<bool> down_pressed;

    NetPlayerInput();
};

/*
 * net_players[0] 對應 P3。
 * net_players[1] 對應 P4。
 */
extern NetPlayerInput net_players[MAX_NET_PLAYERS];

void start_network_server();
void stop_network_server();

#endif
