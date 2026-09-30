"""Conformance harness: lift every code entry of every game in the corpus.

    python tools/conformance.py [--update]

Corpus: data.win paths, one per line, in conformance/corpus.txt (gitignored,
it points at your own games) or in GMRECOMP_CORPUS separated by ';'. With no
corpus the harness says so and exits 0, so CI stays green without game data.

Checks per game, each counted pass/fail:
  - every code entry lifts to C with a consistent stack on every path
  - every built-in function the game calls exists in the runtime (gmrt.h)
  - every built-in variable the game touches is one the runtime implements
Totals are compared with conformance/baseline.json; any count going down
fails the run (--update rewrites the baseline after an improvement).
"""
import json, os, re, sys
sys.path.insert(0, os.path.dirname(__file__))
from gmdata import GameData
from gmrecomp import Lifter, BUILTINS

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = os.path.join(ROOT, 'conformance', 'baseline.json')

def corpus():
    paths = []
    f = os.path.join(ROOT, 'conformance', 'corpus.txt')
    if os.path.exists(f):
        paths += [l.strip() for l in open(f) if l.strip() and not l.startswith('#')]
    paths += [p for p in os.environ.get('GMRECOMP_CORPUS', '').split(';') if p]
    return [p for p in paths if os.path.exists(p)]

def runtime_funcs():
    h = open(os.path.join(ROOT, 'runtime', 'gmrt.h')).read()
    return set(re.findall(r'GMF\((\w+)\)', h)) | {'choose'}   # choose() is gm_choose, the luck hook

def check(path):
    g = GameData(path)
    lf = Lifter(g)
    ok = sum(1 for e in g.code if try_lift(lf, e))
    have = runtime_funcs()
    funcs = set(g.funcs)
    bvars = {v['name'] for v in g.vars if v['id'] == -6}
    return {
        'code': [ok, len(g.code)],
        'functions': [len(funcs & have), len(funcs)],
        'builtin_vars': [len(bvars & set(BUILTINS)), len(bvars)],
        'missing': sorted(funcs - have) + sorted(bvars - set(BUILTINS)),
    }

def try_lift(lf, e):
    try:
        lf.lift(0, e); return True
    except Exception as ex:
        print('  fail %s: %s' % (e['name'], ex)); return False

def main():
    games = corpus()
    if not games:
        print('SKIP: no corpus. List data.win paths in conformance/corpus.txt or GMRECOMP_CORPUS.')
        return 0
    results = {}
    for p in games:
        name = os.path.basename(os.path.dirname(os.path.abspath(p))) or p
        r = check(p)
        results[name] = r
        print('%-24s code %d/%d  functions %d/%d  builtin vars %d/%d%s' % (
            name, *r['code'], *r['functions'], *r['builtin_vars'],
            ('  missing: ' + ', '.join(r['missing'])) if r['missing'] else ''))
    tot = [sum(r[k][i] for r in results.values()) for k in ('code', 'functions', 'builtin_vars') for i in (0, 1)]
    print('TOTAL: %d games, code %d/%d, functions %d/%d, builtin vars %d/%d' % (len(results), *tot))
    if '--update' in sys.argv:
        os.makedirs(os.path.dirname(BASE), exist_ok=True)
        json.dump({k: {m: v[m] for m in ('code', 'functions', 'builtin_vars')} for k, v in results.items()},
                  open(BASE, 'w'), indent=2)
        print('baseline updated')
        return 0
    if os.path.exists(BASE):
        base = json.load(open(BASE))
        bad = [(g, m) for g, v in base.items() if g in results for m in v if results[g][m][0] < v[m][0]]
        for g, m in bad: print('REGRESSION: %s %s %d -> %d' % (g, m, base[g][m][0], results[g][m][0]))
        if bad: return 1
    fails = sum(r['code'][1] - r['code'][0] for r in results.values())
    return 1 if fails else 0

if __name__ == '__main__':
    sys.exit(main())
