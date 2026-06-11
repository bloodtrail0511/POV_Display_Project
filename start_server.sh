#!/bin/bash

PROJECT_DIR="/home/admin/eos/project"
SSID="OUO"
PASS="12345678"
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

echo "=== 3. 啟動 POV 主程式 ==="
cd "$PROJECT_DIR" || exit 1
exec "$PROJECT_DIR/main_test"