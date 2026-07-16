#!/bin/sh
echo $$ > /data/data/com.termux/files/usr/tmp/bgtest.pid
exec sleep 3600
