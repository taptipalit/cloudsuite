import sys
import random

input_fname = sys.argv[1]

pair_list = []

with open(input_fname, 'r') as f:
    for ln in f:
        toks = ln.replace(',', ' ').split()
        cdf_point = float(toks[0].strip())
        val_size = int(toks[1].strip())
        pair_list.append((cdf_point, val_size))

pair_list.append((0.0, 0.0))

random.seed(10)

for idx in range(0, len(pair_list) - 1):
    cp0, vs0 = pair_list[idx]
    cp1, _vs1 = pair_list[idx+1]
    print(random.randint(8,250), vs0, int(round((cp0 - cp1) * 1.0e9)))
