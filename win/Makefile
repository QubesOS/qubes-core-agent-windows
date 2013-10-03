VERSION := $(shell cat ../version_win)

help:
	@echo "make all      --- build all"
	@echo "make clean    --- clean all"

all:
	$(MAKE) all -C utils
	$(MAKE) all -C qrexec
	$(MAKE) all -C qrexec_client_vm
	$(MAKE) all -C ask_vm_and_run
	$(MAKE) all -C qrexec_services

clean:
	$(MAKE) clean -C utils
	$(MAKE) clean -C qrexec
	$(MAKE) clean -C qrexec_client_vm
	$(MAKE) clean -C ask_vm_and_run
	$(MAKE) clean -C qrexec_services
	rm -f libs/*.a libs/*.o

msi:
	candle -arch x64 -dversion=$(VERSION) installer.wxs
	light -o core-agent-windows.msm installer.wixobj
