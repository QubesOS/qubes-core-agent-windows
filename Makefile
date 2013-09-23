help:
	@echo "make all           --- compile everything"
	@echo "make clean         --- clean"

all:
	$(MAKE) all -C win

clean:
	$(MAKE) clean -C win
