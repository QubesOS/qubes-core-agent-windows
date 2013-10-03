help:
	@echo "make all      --- build all"
	@echo "make clean    --- clean all"

all:
	$(MAKE) all -C filecopy-common
	$(MAKE) all -C clipboard-copy
	$(MAKE) all -C clipboard-paste
	$(MAKE) all -C filecopy-agent
	$(MAKE) all -C filecopy-unpacker
	$(MAKE) all -C open-in-vm
	$(MAKE) all -C vm-file-editor
	$(MAKE) all -C wait-for-logon

clean:
	$(MAKE) clean -C filecopy-common
	$(MAKE) clean -C clipboard-copy
	$(MAKE) clean -C clipboard-paste
	$(MAKE) clean -C filecopy-agent
	$(MAKE) clean -C filecopy-unpacker
	$(MAKE) clean -C open-in-vm
	$(MAKE) clean -C vm-file-editor
	$(MAKE) clean -C wait-for-logon
