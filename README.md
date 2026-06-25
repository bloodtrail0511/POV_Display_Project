# 視覺暫留 (POV) 互動遊戲顯示器

本專案是一個基於 Raspberry Pi 與 Linux 作業系統開發的 360 度環狀立體顯示螢幕。利用視覺暫留 (Persistence of Vision, POV) 原理結合高速旋轉的 APA102 LED 燈條，並透過自定義的 Linux Character Device Driver 達到極低延遲的硬體中斷與畫面同步，實現高流暢度的互動遊戲與多媒體展示。

## ✨ 核心技術與系統特色

* **Device Tree Overlay (DTS) 硬體綁定**：撰寫 `.dts` 檔案關閉預設的 `spidev`，將 SPI0 硬體節點直接交由自訂的驅動程式接管，突破 User Space Scheduler 的時間延遲限制。
* **即時硬體同步 (Real-time Synchronization)**：實作 Hall Sensor 核心驅動，精準記錄觸發時間 (`ktime`)，並透過低通濾波演算法計算轉速，解決高速旋轉時的畫面抖動問題。
* **核心級雙重緩衝 (Double Buffering)**：在 Kernel Space 實作 Double Buffer，利用 `Spinlock` 保護共享資源。唯有在 Hall Sensor 觸發 0 度回呼 (Callback) 時才進行畫面交換，完美解決畫面撕裂 (Tearing) 現象。
* **非同步 SPI 傳輸 (Async SPI)**：使用 `hrtimer` 以極高頻率觸發 SPI 傳輸，並利用 `atomic_t` 實作 Busy Flag，防止傳輸重疊。
* **TCP Client-Server 架構**：分離主機端 (Server) 與手把端 (Client)。使用 `SIGIO` 非同步訊號讀取硬體按鍵，透過 Socket 實現多玩家即時連線。
* **虛擬模擬器 (Virtual Device)**：內建基於 OpenCV 開發的虛擬模擬器，可在無實體硬體的情況下，直接於 PC 端進行 UI 渲染、狀態機 (FSM) 轉換與遊戲邏輯的開發。

## 📂 專案目錄結構
```
POV-Display-Project/
├── README.md               # 專案介紹與使用說明
├── Makefile                # 專案頂層控制 Makefile (統籌整體自動化編譯)
├── docs/                   # 存放專題說明文件與簡報
│   └── EOS-Final.pdf
├── dts/                    # 裝置樹重疊設定檔 (Device Tree Overlay)
│   └── pov_apa102.dts
├── drivers/                # Linux 核心驅動程式 (Kernel Space)
│   ├── Makefile            # 核心模組專用 Kbuild Makefile
│   ├── magnet_driver.c     # 霍爾感測器驅動
│   ├── my_btn_driver_v2.c  # 手把按鍵驅動 (Debounce 機制)
│   └── pov_display_driver_v3.c # APA102 SPI 顯示驅動
├── server/                 # 顯示器主機端主程式 (User Space)
│   ├── main_test.cpp
│   ├── main_test_sim.cpp   # PC 端虛擬模擬器
│   ├── pov_display.cpp
│   └── pov_display.hpp
├── client/                 # 遙控器手把端程式 (User Space)
│   └── controller_client.cpp
├── scripts/                # 開機自動化與網路連線腳本
│   ├── pov_auto.service
│   ├── start_client.sh
│   └── start_server.sh
└── assets/                 # 執行時期需要的圖片與影片資源
    ├── icon/
    └── video/
```
* `Makefile` : 編譯所需檔案
* `dts/`：包含 `pov_apa102.dts`，用於重新定義 Raspberry Pi 的 SPI 腳位對應。
* `drivers/`：包含三個 Linux Kernel Modules (`.ko`)。
  * `magnet_driver.c`：霍爾感測器 ISR 與轉速濾波。
  * `my_btn_driver_v2.c`：包含 `delayed_work` 軟體防彈跳 (Debounce) 的按鍵驅動。
  * `pov_display_driver_v3.c`：APA102 LED 燈條 SPI 渲染與時序控制驅動。
* `server/`：主機端應用程式 (C++)，負責 OpenCV 影像處理、狀態機邏輯與 Socket 監聽。
* `client/`：手把端應用程式 (C++)，負責讀取本機驅動訊號並透過 TCP 傳送。
* `scripts/`：NetworkManager 自動連線腳本與 systemd service 設定，實現設備開機自動連線與執行。

## 🚀 編編譯與執行方式

### 1. 修改目錄
將 Makefile 裡面的 `KDIR` 修改為實際用於樹莓派或交叉編譯的 Linux 核心原始碼樹（Kernel Source Tree）目錄。例如：
```makefile
KDIR = /home/ouo/master_degree/class/eos/linux-origin
```

### 2. 編編譯檔案
在開發主機的專案根目錄下執行以下指令，編譯驅動程式、手把端程式與裝置樹重疊檔：
```bash
make all
```

### 3. 檔案上傳
分別將編譯完成的驅動、執行檔與相關腳本透過 `scp` 上傳至對應的 Server、Client 樹莓派（例如工作目錄 `/home/rtes/`）：

**Server (主機端樹莓派) 需上傳：**
* 核心驅動模組：`magnet_driver.ko`、`pov_display_driver_v3.ko`、`my_btn_driver_v2.ko`
* 裝置樹重疊檔：`pov_apa102.dtbo`
* 主程式原始碼：`main_test_serverjoin.cpp`、`pov_display.cpp`、`pov_display.hpp`
* 自動化腳本與服務：`start_server.sh`、`pov_auto.service`
* 多媒體資源資料夾：`assets/`（包含 `icon/` 與 `video/`，務必與主程式放在同一層目錄）

**Client (手把端樹莓派)：**
* 核心驅動模組：`my_btn_driver_v2.ko`
* 交叉編譯執行檔：`controller_client`
* 自動化腳本與服務：`start_client.sh`、`pov_auto.service`

### 4. 在樹莓派(Server)執行main_test.cpp的編譯
登入 Server 樹莓派後，切換至擺放原始碼的目錄，執行以下命令編譯結合 OpenCV 與多執行緒的 POV 主程式：
```bash
g++ -o main_test_serverjoin main_test_serverjoin.cpp pov_display.cpp $(pkg-config --cflags --libs opencv4) -lpthread
```

### 5. 複製DTBO並執行指令
在 **Server 樹莓派** 上，將裝置樹重疊檔複製到系統韌體目錄下：
```bash
sudo cp pov_apa102.dtbo /boot/firmware/overlays/
```
編輯開機組態設定檔以啟用自訂的 overlay：
```bash
sudo nano /boot/firmware/config.txt
```
在檔案的最末端加上以下設定：
```text
dtoverlay=pov_apa102
```
*修改完成後存檔退出，並重啟樹莓派 (`sudo reboot`) 以載入 `/dev/pov_display` 與 `/dev/mag_sensor` 設備節點。*

### 6. 啟用服務設定開機自啟動
請先確認兩台樹莓派上的 `pov_auto.service` 服務檔內，其 `WorkingDirectory` 與 `ExecStart` 腳本（`start_server.sh` / `start_client.sh`）的**絕對路徑**皆正確無誤。接著在兩台樹莓派上分別執行：
```bash
sudo cp pov_auto.service /etc/systemd/system/pov_auto.service
sudo systemctl daemon-reload
sudo systemctl enable pov_auto.service
sudo systemctl start pov_auto.service
```
若需要查看自動啟動與雙機 Wi-Fi 通訊的即時 Debug 日誌，可執行：
```bash
sudo journalctl -u pov_auto.service -f
```