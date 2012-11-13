#!/sbin/sh
#
if /sbin/[ -d /system/etc/init.d ]
then
  /sbin/run-parts /system/etc/init.d
fi
