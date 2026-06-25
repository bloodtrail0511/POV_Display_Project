# --- Cross-compilation settings ---
ARCH = arm64
CROSS_COMPILE = aarch64-linux-gnu-
KDIR = /home/ouo/master_degree/class/eos/linux-origin

# --- Directories ---
DRV_DIR = drivers
SRV_DIR = server
CLI_DIR = client
DTS_DIR = dts

# 預設編譯目標：排除舊版的 reader，新增 controller, spi_dts 與 tester
all: driver spi_dts tester controller

# 1. 編譯 Kernel Modules (驅動程式)
# 它會指派 Kbuild 去 drivers/ 目錄下尋找 Makefile 進行編譯
driver:
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KDIR) M=$(shell pwd)/$(DRV_DIR) modules
	@echo "清理中介檔案..."
	@rm -f $(DRV_DIR)/*.o $(DRV_DIR)/*.mod $(DRV_DIR)/*.mod.c $(DRV_DIR)/*.mod.o $(DRV_DIR)/modules.order $(DRV_DIR)/Module.symvers
	@rm -f $(DRV_DIR)/.*.cmd $(DRV_DIR)/.*.o.d
	@rm -rf $(DRV_DIR)/.tmp_versions

# 2. 編譯 Client 端 (遙控器手把)
controller:
	$(CROSS_COMPILE)g++ -o $(CLI_DIR)/controller_client $(CLI_DIR)/controller_client.cpp

# 3. 編譯 Device Tree Overlay
spi_dts:
	dtc -@ -I dts -O dtb -o $(DTS_DIR)/pov_apa102.dtbo $(DTS_DIR)/pov_apa102.dts

# 4. 編譯 Server 端模擬器 (使用本機原生 g++ 與 OpenCV)
tester:
	g++ -o $(SRV_DIR)/main_test_sim $(SRV_DIR)/main_test_sim.cpp $$(pkg-config --cflags --libs opencv4)

# 5. 清除所有編譯產物
clean:
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KDIR) M=$(shell pwd)/$(DRV_DIR) clean
	rm -f $(CLI_DIR)/controller_client
	rm -f $(SRV_DIR)/main_test_sim
	rm -f $(DTS_DIR)/pov_apa102.dtbo
	@echo "Clean completed."