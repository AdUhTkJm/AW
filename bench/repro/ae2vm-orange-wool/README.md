# Minimal reproduction: AE2VM returns an unbalanced plan for a feasible request

`minecraft:orange_wool x1` with the `recipes-vanilla` `random20` inventory. AW in optimal
mode proves this feasible with cost 2 (two real recipe executions). AE2VM's answer depends on
how much state its VM has already accumulated:

| invocation | AE2VM status | cost | balance check |
|---|---|---|---|
| cold (`--warmup 0`) | `missing`, 3 items short | 6 | fails (partial plan) |
| warm (`--warmup 1`, i.e. the same query executed once before) | `ok`, `missing=[]` | 2 | **fails** |

The warm row is a false positive: it reports a complete plan whose firing vector does not
satisfy `produced - consumed >= rhs`, which no amount of timing analysis can detect. It is
caught by the `balance_ok` column of the benchmark harness.

## Ground truth (AW, optimal mode)

```bash
cd ../../..                      # repository root
./build/aw_bench --dataset repro --awr temp/recipes-vanilla.awr \
    --plan bench/repro/ae2vm-orange-wool/p --config aw-optimal --out /tmp/gt.jsonl \
    --nonoptimal 0 --stage satellite:optimal --time-limit 40 --warmup 0 --repeats 1
python3 -c "import json;print([r for r in map(json.loads,open('/tmp/gt.jsonl')) if r['type']=='query'])"
```

Expected: `status=ok`, `feasible=true`, `proven_optimal=true`, `cost=2`, `missing_count=0`.
With `awr_inspect` the plan itself is:

```
1 x minecraft:orange_dye  <- minecraft:orange_tulip x1
1 x minecraft:orange_wool <- minecraft:orange_dye x1, #1381 x1
1 x #1381                 <- minecraft:light_gray_wool x1
```

`minecraft:orange_tulip` (handle 839, stocked 450 834) and `minecraft:light_gray_wool`
(handle 666, stocked 127) are both in `p.stock.random20.tsv`, and `#1381` is the tag
pseudo-item for grey wools, obtained for free by its synthetic member recipe.

## AE2VM, cold and warm

```bash
cd compare/ae2vm
for W in 0 1; do
  ./gradlew --no-daemon -q benchAwr --args="--dataset repro \
      --awr ../../temp/recipes-vanilla.awr --plan ../../bench/repro/ae2vm-orange-wool/p \
      --config ae2vm-cold --out /tmp/ae2vm-w$W.jsonl --warmup $W --repeats 1 --deadline-ms 5000"
done
grep -h query /tmp/ae2vm-w0.jsonl /tmp/ae2vm-w1.jsonl | python3 -c "
import json,sys
for line in sys.stdin:
    r=json.loads(line)
    print('cold=%s status=%-8s cost=%-3s balance_ok=%-5s missing=%s'
          % (r['cold'], r['status'], r['cost'], r['balance_ok'], r['missing']))"
```

Expected:

```
cold=True  status=missing  cost=6   balance_ok=False missing=[['#275', 1], ['#581', 1], ['#106', 1]]
cold=False status=ok       cost=2   balance_ok=False missing=[]
```

## Notes

* The same instance inside the full 48-query sweep comes out as a false positive in *both*
  the cold and the warm configuration, so the `ae2vm` and `ae2vm-cold` rows of the summary
  table are not redundant: AE2VM's answer is a function of the run's query history, not only
  of the instance, and the sweep is not a controlled way to observe the difference.
* `bench/run.py` reports both, and `summarize.py`'s `false_positive` column is computed from
  `feasible and not balance_ok` for every engine, so this class of defect cannot pass
  unnoticed.
