#!/bin/zsh

# Only for deployment test, not for paper.
# Compare tb-v2, aw-optimal, aw-flash and ae2vm.
python3 bench/run.py --datasets recipes-vanilla,recipes-small --configs aw-optimal,aw-flash,tb-v2,ae2vm --warmup 0 --time-limit 5
python3 bench/run.py --datasets recipes-nast,recipes-atm --configs aw-optimal,aw-flash,tb-v2,ae2vm --warmup 0 --time-limit 20
