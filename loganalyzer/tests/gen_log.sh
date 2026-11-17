#!/bin/bash
# gen_log.sh - generate a common-log-format test file
#
#   usage: ./tests/gen_log.sh [LINES] [OUTFILE]
#
# 5,000,000 lines is roughly 380 MB and takes about 25 seconds to write.
# srand(42) fixes the sequence, so counts verified once stay valid.
#
# The addresses are drawn from a small pool of regular clients most of the
# time, with occasional one-off visitors, so that ranking the top addresses
# produces something meaningful instead of five entries with a count of 2.

set -u

lines=${1:-5000000}
out=${2:-tests/big.log}

awk -v n="$lines" 'BEGIN {
  srand(42);
  split("INFO WARN ERROR DEBUG", lv, " ");
  split("/ /login /api/users /api/orders /static/app.js /static/style.css /health /admin", pa, " ");

  # a pool of 120 regular clients
  npool = 120;
  for (j = 1; j <= npool; j++)
    pool[j] = int(rand()*223)+1 "." int(rand()*254) "." int(rand()*254) "." int(rand()*253)+1;

  for (i = 0; i < n; i++) {
    if (rand() < 0.85)
      ip = pool[int(rand()*npool)+1];                 # a regular client
    else
      ip = int(rand()*223)+1 "." int(rand()*254) "." int(rand()*254) "." int(rand()*253)+1;

    # paths are skewed too: the first few are far more common
    r = rand();
    if (r < 0.40)      path = pa[1];
    else if (r < 0.60) path = pa[2];
    else if (r < 0.75) path = pa[3];
    else               path = pa[int(rand()*8)+1];

    if (rand() < 0.88) { status = 200; level = (rand() < 0.75 ? "INFO" : "DEBUG"); }
    else               { status = 500; level = (rand() < 0.60 ? "ERROR" : "WARN"); }

    printf "%s - - [12/Nov/2026:09:%02d:%02d] \"GET %s HTTP/1.1\" %d %d %s\n",
      ip, int(rand()*60), int(rand()*60), path, status, int(rand()*9000)+100, level;
  }
}' > "$out"

ls -lh "$out"
