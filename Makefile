##############################################
##
## Compile probe
##
##############################################

TARGET_NAME = pure_usbnet

ifneq ($(KERNELRELEASE),)
    obj-m := $(TARGET_NAME).o
else
	PWD := $(shell pwd)
	ROOT=$(realpath $(PWD)/../../)
# Knernel Direct
	KERNELDIR=$(ROOT)/kernel
# Compile Toolchain
	TOOLS=$(ROOT)/toolchain/gcc-linaro-aarch/bin/aarch64-linux-gnu-

default:
	@make -C $(KERNELDIR) M=$(PWD) ARCH=arm64 CROSS_COMPILE=$(TOOLS) modules
	@cp $(TARGET_NAME).ko /work/
endif

.PHONEY:clean 
clean:
	@rm -rf *.o
	@rm -rf *.ko
	@rm -rf *.symvers *.order *.mod.c
	
	