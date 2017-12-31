# old-vmware-modules
Patched kernel modules for VMware Player 7.1.6 and Workstation 7.1.6 to work on current kernels

VMWare crashing with Segmentation fault
---------------------------------------
If vmplayer crashes with segmentation fault (segfault), append this to /etc/vmware/bootstrap:

    export LD_LIBRARY_PATH=/usr/lib/vmware/lib/libglibmm-2.4.so.1/:/usr/lib/vmware/lib/libgdkmm-2.4.so.1/:/usr/lib/vmware/lib/libgtkmm-2.4.so.1
