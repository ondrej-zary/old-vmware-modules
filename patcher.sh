#!/bin/sh
# VMWare Workstation/Player _host kernel modules_ patcher
# Based on v0.6.2 by Artem S. Tashkinov
# Use at your own risk.

patchdir=patches
vmreqver=7.1.6
plreqver=3.1.6

error()
{
	echo "$*. Exiting"
	exit
}

curdir=`pwd`
bdate=`date "+%F-%H:%M:%S"` || error "date utility didn't quite work. Hm"
vmver=`vmware-installer -l 2>/dev/null | awk '/vmware-/{print $1substr($2,1,5)}'`
vmver="${vmver#vmware-}"
basedir=/usr/lib/vmware/modules/source
ptoken="$basedir/.patched"
bkupdir="$basedir-$vmver-$bdate-backup"

unset product
[ -z "$vmver" ] && error "VMWare not found (using vmware-installer -l)"
[ "$vmver" = "workstation$vmreqver" ] && product="VMWare WorkStation"
[ "$vmver" = "player$plreqver" ] && product="VMWare Player"
[ -z "$product" ] && error "Sorry, this script is only for VMWare WorkStation $vmreqver or VMWare Player $plreqver"

[ ! -d "$patchdir" ] && error "Patch directory '$patchdir' not found"
[ `find "$patchdir" -prune -empty` ] && error "Patch directory '$patchdir' is empty"
[ "`id -u`" != "0" ] && error "You must be root to run this script"
[ -f "$ptoken" ] && error "$ptoken found. The sources are already patched. If you want to run patcher again, restore unpatched sources from backup first ($basedir-$vmver-*-backup)"
[ ! -d "$basedir" ] && error "Source '$basedir' directory not found, reinstall $product"
[ ! -f /lib/modules/`uname -r`/build/include/generated/uapi/linux/version.h ] && error "Kernel headers not found. Install kernel headers for the currently running kernel"

# patch binary files to accept two-number kernel versions
sed 's/\x83\xe8\x03\x83\xf8\x01\x0f\x96\xc0/\x83\xe8\x02\x83\xf8\x01\x0f\x96\xc0/' -i /usr/lib/vmware/lib/libvmware-modconfig-console.so/libvmware-modconfig-console.so
sed 's/\x83\xe8\x03\x83\xf8\x01\x0f\x96\xc0/\x83\xe8\x02\x83\xf8\x01\x0f\x96\xc0/' -i /usr/lib/vmware/lib/libvmware-modconfig.so/libvmware-modconfig.so

tmpdir=`mktemp -d` || exit 1
cp -an "$basedir" "$bkupdir" || exit 2

tar xf "$basedir"/vmci.tar    -C "$tmpdir" || exit 3
tar xf "$basedir"/vsock.tar   -C "$tmpdir" || exit 3
tar xf "$basedir"/vmnet.tar   -C "$tmpdir" || exit 3
tar xf "$basedir"/vmmon.tar   -C "$tmpdir" || exit 3
tar xf "$basedir"/vmblock.tar -C "$tmpdir" || exit 3

for i in "$patchdir/"*; do patch -p1 -d "$tmpdir" < $i; done || exit 4
tar cf "$tmpdir"/vmci.tar    -C "$tmpdir"    vmci-only || exit 5
tar cf "$tmpdir"/vsock.tar   -C "$tmpdir"   vsock-only || exit 5
tar cf "$tmpdir"/vmnet.tar   -C "$tmpdir"   vmnet-only || exit 5
tar cf "$tmpdir"/vmmon.tar   -C "$tmpdir"   vmmon-only || exit 5
tar cf "$tmpdir"/vmblock.tar -C "$tmpdir" vmblock-only || exit 5

mv "$tmpdir"/vmci.tar "$tmpdir"/vsock.tar "$tmpdir"/vmnet.tar "$tmpdir"/vmmon.tar "$tmpdir"/vmblock.tar "$basedir" || exit 6
rm -rf "$tmpdir" || exit 7
touch "$ptoken" || exit 8

# create symlink to version.h so vmware-modconfig can find it at the old location
mkdir -p /lib/modules/`uname -r`/build/include/linux
ln -s /lib/modules/`uname -r`/build/include/generated/uapi/linux/version.h /lib/modules/`uname -r`/build/include/linux/version.h

vmware-modconfig --console --install-all

echo -e "\n"
echo "All done, you can now run $product."
echo "Modules sources backup can be found in the '$bkupdir' directory"
