pov_display_project/
├── .gitignore               # 忽略編譯產生的暫存檔 (如 .o, .ko, 執行檔)
├── README.md                # 專題首頁介紹 (放系統架構圖、展示影片連結)
├── LICENSE                  # 開源授權條款 (建議用 MIT)
├── Makefile                 # 頂層 Makefile，用來一鍵編譯整個專題
│
├── app/                     # User Space: 應用程式層 (遊戲引擎)
│   ├── src/                 # C/C++ 原始碼 (.cpp, .c)
│   │   ├── main.cpp         # 遊戲主迴圈 (Game Loop)
│   │   ├── physics.cpp      # 物理引擎 (處理重力與碰撞)
│   │   └── renderer.cpp     # 算圖引擎 (轉換極座標陣列並寫入 /dev/pov)
│   ├── include/             # 標頭檔 (.h, .hpp)
│   │   ├── physics.h
│   │   └── renderer.h
│   └── Makefile             # 專門用來編譯 User Space 執行檔的 Makefile
│
├── driver/                  # Kernel Space: 作業系統核心層 (設備驅動)
│   ├── src/                 # 驅動程式原始碼
│   │   ├── pov_driver.c     # Character Device Driver 主程式 (chrdev 註冊)
│   │   ├── pov_timer.c      # hrtimer 與硬體時序控制邏輯
│   │   └── pov_isr.c        # 霍爾感測器與按鍵的中斷處理常式 (Top-Half)
│   ├── include/             # 驅動程式標頭檔
│   │   └── pov_driver.h
│   └── Makefile             # Kernel Module 的 Makefile (呼叫核心編譯系統)
│
├── hardware/                # 硬體層: 實體配置與硬體描述
│   ├── dts/                 # Device Tree Source (.dts)
│   │   └── pov-overlay.dts  # 註冊 GPIO 中斷與 SPI 的設備樹疊加層
│   ├── pcb/                 # (可選) 電路板設計檔 (KiCad 或 Altium 專案)
│   └── mech/                # (可選) 機構設計檔 (旋轉臂 3D 列印 .stl 檔)
│
├── scripts/                 # 自動化腳本 (開發與部屬工具)
│   ├── deploy.sh            # 一鍵將 .ko 載入核心並啟動遊戲的腳本
│   └── setup_rt_env.sh      # (可選) 記錄如何設定 PREEMPT_RT 環境的腳本
│
└── docs/                    # 專題文件與報告
    ├── architecture.png     # 系統架構圖
    ├── latency_test.png     # cyclictest 的效能延遲圖表
    └── api_reference.md     # 說明 /dev/pov_display 接收的陣列格式