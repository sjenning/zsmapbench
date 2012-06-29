EXTRA_CFLAGS := -g -Wall
KERNEL_BUILD_PATH ?= "/lib/modules/$(shell uname -r)/build"

obj-m += zsmapbench.o

all:
	make -C $(KERNEL_BUILD_PATH) M=$(PWD) modules

clean:
	make -C $(KERNEL_BUILD_PATH) M=$(PWD) clean
	@$(RM) -rf *.o *~ *.c.gcov *.gcda *.gcno cscope.* tags *.ko

