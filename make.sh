#!/bin/sh
set -e
rm -f ./*.log
g++ -std=c++17 -Wall -Wextra -O2 main.cpp -o chess
