obj-m += magnet_driver.o
obj-m += pov_display_driver_v2.o
obj-m += pov_display_driver_v3.o
# obj-m += led_strip_driver.o
# obj-m += timer_driver.o

KDIR = /home/ouo/master_degree/class/eos/linux-origin

ARCH = arm64
CROSS_COMPILE = aarch64-linux-gnu-

all: driver reader

driver:
	make ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KDIR) M=$(shell pwd) modules
	@rm -f *.o *.mod *.mod.c *.mod.o modules.order Module.symvers
	@rm -f .*.cmd
	@rm -rf .tmp_versions

reader:
	$(CROSS_COMPILE)gcc -o magnet_reader magnet_reader.c

clean:
	make ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KDIR) M=$(shell pwd) clean
	rm -f magnet_reader