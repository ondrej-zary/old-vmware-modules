all:
	make -C vmblock-only
	make -C vmci-only
	make -C vmmon-only
	make -C vmnet-only
	make -C vsock-only
clean:
	make -C vmblock-only clean
	make -C vmci-only clean
	make -C vmmon-only clean
	make -C vmnet-only clean
	make -C vsock-only clean
