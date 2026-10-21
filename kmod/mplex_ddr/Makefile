# Cross-build against the prepared MiSTer 5.15.1 tree:
#   make -C kmod/mplex_ddr KDIR=/data/misterplex/kernel/Linux-Kernel_MiSTer \
#     CROSS_COMPILE=arm-none-linux-gnueabihf- ARCH=arm
obj-m += mplex_ddr.o
KDIR ?= /lib/modules/$(shell uname -r)/build
ARCH ?= arm
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) clean
