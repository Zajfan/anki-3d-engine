#!/usr/bin/env bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

export LD_LIBRARY_PATH=$SCRIPT_DIR:$LD_LIBRARY_PATH

$SCRIPT_DIR/dxc.bin "$@"