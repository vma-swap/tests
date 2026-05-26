# -------- CONFIG --------
KDIR ?= /usr/src/linux-6.14-rc5
PWD := $(CURDIR)
SUDO := $(shell if [ "$$(id -u)" = 0 ]; then echo ""; else echo "sudo"; fi)

# Kernel module
obj-m := swpctl_module.o
MODULE := swpctl_module.ko
KDIR_READY := $(wildcard $(KDIR)/Makefile)

# Userspace test
TEST_SRC := test.c test_util.c
TEST_BIN := test_runner
CFLAGS := -Wall -Wextra -g

# -------- TARGETS --------
.PHONY: all clean kmod kmod-unsafe install_module vm host

ifneq ($(KDIR_READY),)
all: host
else
all: vm
endif

host: kmod
vm: $(TEST_BIN) install_module
port: vm

# Build the kernel module
kmod:
	@if [ ! -f "$(KDIR)/Module.symvers" ]; then \
		echo "Missing $(KDIR)/Module.symvers."; \
		echo "Build the target kernel first, or point KDIR at the prepared syzkaller kernel build tree."; \
		echo "Refusing unsafe module build. Use 'make kmod-unsafe' only if you intentionally want unresolved-symbol warnings."; \
		exit 1; \
	fi
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Escape hatch for incomplete syzkaller build trees without Module.symvers.
# Prefer fixing KDIR or building the kernel once so host builds are checked.
kmod-unsafe:
	$(MAKE) -C $(KDIR) M=$(PWD) KBUILD_MODPOST_WARN=1 modules
	
install_module:
	@if [ ! -f "$(MODULE)" ]; then \
		echo "Missing $(MODULE). Build it on the host against the VM kernel, then retry inside the VM."; \
		exit 1; \
	fi
	-$(SUDO) rmmod swpctl_module
	$(SUDO) insmod $(MODULE)

# Build the userspace test binary
$(TEST_BIN): $(TEST_SRC) test_framework.h test_util.h
	$(CC) $(CFLAGS) -o $@ $(TEST_SRC)

clean:
	@if [ -n "$(KDIR_READY)" ]; then $(MAKE) -C $(KDIR) M=$(PWD) clean; fi
	$(RM) -f $(TEST_BIN)
	-$(SUDO) rmmod swpctl_module
