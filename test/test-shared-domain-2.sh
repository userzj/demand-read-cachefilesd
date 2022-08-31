#!/bin/bash
#
# Test shared domain err path.
# This is fixed by the kernel commit "erofs: remove duplicated unregister_cookie 

fscachedir="/root"
_bootstrap="test.img"
_datablob="blob1.img"

make > /dev/null 2>&1
if [ $? -ne 0 ]; then
	echo "make failed"
	exit
fi

_volume="erofs,$_bootstrap"
volume="I$_volume"

bootstrap_fan=$(../getfan $_volume $_bootstrap)
bootstrap_path="$fscachedir/cache/$volume/@$bootstrap_fan/D$_bootstrap"

datablob_fan=$(../getfan $_volume $_datablob)
datablob_path="$fscachedir/cache/$volume/@$datablob_fan/D$_datablob"

rm -f $bootstrap_path
rm -f $datablob_path

cd ..
./cachefilesd2 $fscachedir > /dev/null  &
cd test

sleep 2

# test multidev data layout
rm -f $bootstrap_path
rm -f $datablob_path
cp img/multidev/test.img ../
cp img/multidev/blob1.img ../

mount -t erofs none -o fsid=test.img,device=blob1.img,domain_id=domain0 /mnt/
if [ $? -ne 0 ]; then
	echo "[shared domain] mount failed"
	pkill cachefilesd2
	exit
fi

content="$(cat /mnt/stamp-h1)"
if [ $? -ne 0 ]; then
	echo "[shared domain] read failed"
	umount /mnt
	pkill cachefilesd2
	exit
fi

if [ "$content" != "timestamp for config.h" ]; then
	echo "[shared domain] data corruption ($content)"
	umount /mnt
	pkill cachefilesd2
	exit
fi

mkdir -p mnt-domain

mount -t erofs none -o fsid=test.img,device=blob1.img,domain_id=domain0 mnt-domain
if [ $? -eq 0 ]; then
	echo "mount shall not succeed"
	umount mnt-domain
	pkill cachefilesd2
	exit
fi

content="$(cat /mnt/stamp-h1)"
if [ $? -ne 0 ]; then
	echo "[shared domain] read failed"
	umount /mnt
	pkill cachefilesd2
	exit
fi

if [ "$content" != "timestamp for config.h" ]; then
	echo "[shared domain] data corruption ($content)"
	umount /mnt
	pkill cachefilesd2
	exit
fi

umount /mnt
echo "[shared domain] pass"

#cleanup
pkill cachefilesd2
rmdir mnt-domain
