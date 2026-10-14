#!/usr/bin/env python3
"""Does the cost model's predicted fan-out stand in for the measured one?

    scripts/collect-fanout.py                      # measured
    scripts/collect-predicted-fanout.sh            # predicted
    scripts/fanout-predicted-vs-measured.py

If it does, the gate declines at plan time and pays no abandonment at all,
which is strictly better than any runtime check. D84 records the answer: on
the runnable corpus the prediction looks *better* than the measurement, and on
the excluded corpus the disagreements it gets away with here turn out to be 13
rescues. Run scripts/fanout-excluded.py against the predicted file before
believing anything this prints.
"""

import collections
import csv

def num(v, cap=300.0): return cap if v in ("to","",None) else float(v)
pred = collections.defaultdict(list)
for r in csv.DictReader(open("/tmp/predfan.csv")):
    try: pred[r["query"]].append(float(r["partners"]))
    except ValueError: pass
meas = collections.defaultdict(list)
for r in csv.DictReader(open("tmp/fanout_runnable.csv")):
    meas[r["query"]].append(float(r["fanout_mean"]))
base = {r["query"]: r for r in csv.DictReader(open("tmp/headline.csv"))}

pairs = [(min(pred[q]), min(meas[q]), q) for q in pred if q in meas and pred[q]]
print(f"{len(pairs)} queries with both a predicted and a measured minimum fan-out\n")

# Does predicted <= 1 identify the same queries measured <= 1 does?
tp = sum(1 for p,m,_ in pairs if p<=1.0 and m<=1.0)
fp = sum(1 for p,m,_ in pairs if p<=1.0 and m>1.0)
fn = sum(1 for p,m,_ in pairs if p>1.0 and m<=1.0)
tn = sum(1 for p,m,_ in pairs if p>1.0 and m>1.0)
print(f"  predicted<=1 & measured<=1 : {tp:>3}   (agree, decline)")
print(f"  predicted<=1 & measured >1 : {fp:>3}   (would wrongly decline)")
print(f"  predicted >1 & measured<=1 : {fn:>3}   (would wrongly keep)")
print(f"  predicted >1 & measured >1 : {tn:>3}   (agree, keep)")

# And the only question that matters: what would declining on the PREDICTED
# value cost, scored the same way the measured one was?
fires=[]
for p,m,q in pairs:
    if q not in base or base[q].get("fired")!="1": continue
    off,auto,force = num(base[q]["off_s"]),num(base[q]["auto_s"]),num(base[q]["force_s"])
    if min(off,auto,force)<=0.002: continue
    fires.append((q,p,m,off,auto,force))
others = sum(num(base[q]["auto_s"]) for q in base if base[q].get("fired")!="1")
for label, key in (("measured", 2), ("predicted", 1)):
    dec = [f for f in fires if f[key]<=1.0]
    cut = sum(1 for f in dec if f[4]>f[3])
    lost = sum(1 for f in dec if f[3]/f[5]>1.0)
    after = sum(f[3] if f[key]<=1.0 else f[4] for f in fires)+others
    print(f"\n  decline on {label:<9} fan-out<=1: {len(dec)} of {len(fires)} fires, "
          f"{cut} regressions cut, {lost} real wins lost, {after:.2f}s")
for f in fires:
    if f[1]<=1.0 and f[3]/f[5]>1.0:
        print(f"    predicted would give up {f[0]} at x{f[3]/f[5]:.2f} (measured fan-out {f[2]:.2f})")
