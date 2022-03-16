#!/bin/bash

trap 'catch $LINENO' ERR
catch() {
	echo "$0: error on line $1. Code coverage not stored." >&2
	exit 1
}

if [ -z "$IGT_KERNEL_TREE" ] ; then
	echo "Error! IGT_KERNEL_TREE environment var was not defined." >&2
	exit 1
fi

if [ -z "$1" ] ; then
	echo "Usage: $0 <output>" >&2
	echo "   Please notice that this script assumes $IGT_KERNEL_TREE is used as both Kernel source and object dir." >&2
	exit 1
fi

lcov -q --rc lcov_branch_coverage=1 \
	--test-name "$(basename $1)" -b $IGT_KERNEL_TREE --capture \
	--output-file $1.info

uptime=$(cat /proc/uptime|cut -d' ' -f 1)
echo "[$uptime]     Code coverage wrote to $1.info"
