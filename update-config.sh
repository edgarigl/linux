#!/bin/bash

CF=arch/arm64/configs/my-extras.config
echo "# Config fragment mixins from hvac-dmeo all in one" >$CF

for f in ../hvac-demo/mixins/linux/*.config; do
	echo "" >>$CF
	echo "# from $f" >>$CF
	cat $f >>$CF
done

