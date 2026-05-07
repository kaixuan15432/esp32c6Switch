#!/bin/bash
mkdir -p ~/bin
ln -sf /Library/Developer/CommandLineTools/Library/Frameworks/Python3.framework/Versions/3.9/bin/python3.9 ~/bin/python3
export PATH=~/bin:$PATH
export IDF_PATH=/Users/hongyu/esp/esp-idf
export ESP_MATTER_PATH=/Users/hongyu/esp/esp-matter
export PYTHON=/Users/hongyu/.espressif/python_env/idf5.1_py3.9_env/bin/python
. /Users/hongyu/esp/esp-idf/export.sh
. /Users/hongyu/esp/esp-matter/export.sh
idf.py build
