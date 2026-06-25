#!/bin/bash
PROJECT_DIR="/home/rtes"
SSID="OUO"
PASS="12345678"
SERVER_HOST="admin.local"
IFACE="wlan0"

echo "=== 1. 準備 Wi-Fi profile：$SSID ==="

# 如果 OUO 這個 connection profile 不存在，就建立它
if ! nmcli -t -f NAME connection show | grep -qx "$SSID"; then
    echo "找不到 Wi-Fi profile，正在建立：$SSID"

    nmcli connection add type wifi ifname "$IFACE" con-name "$SSID" ssid "$SSID"
    nmcli connection modify "$SSID" wifi-sec.key-mgmt wpa-psk
    nmcli connection modify "$SSID" wifi-sec.psk "$PASS"
    nmcli connection modify "$SSID" connection.autoconnect yes
fi

echo "=== 2. 開始搜尋並連線 Wi-Fi ==="

while ! nmcli -t -f active,ssid dev wifi | grep -qx "yes:$SSID"; do
    echo "掃描熱點中..."
    nmcli dev wifi rescan
    sleep 2

    echo "嘗試連線至 $SSID..."
    nmcli connection up "$SSID"

    sleep 3
done

echo "成功連上熱點：$SSID！"

echo "=== 2. 等待 Server 開機並連上網路 ==="
# 不斷 Ping Server，直到 Server 也連上熱點為止
while ! ping -c 1 -W 1 $SERVER_HOST &> /dev/null; do
    echo "找不到 $SERVER_HOST，Server 可能還在開機，2秒後重試..."
    sleep 2
done
echo "Server 已上線！準備啟動搖桿程式！"

echo "=== 3. 載入驅動與執行程式 ==="

while true; do
    echo "等待 Server：$SERVER_HOST ..."

    while ! ping -c 1 -W 1 $SERVER_HOST &> /dev/null; do
        echo "找不到 $SERVER_HOST，2秒後重試..."
        sleep 2
    done

    echo "Server 已上線，啟動 controller_client..."

    sudo $PROJECT_DIR/controller_client $SERVER_HOST

    echo "controller_client 結束，可能是 Server 斷線，2秒後重新尋找 Server..."
    sleep 2
done