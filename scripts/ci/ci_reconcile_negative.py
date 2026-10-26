#!/usr/bin/env python3
"""Guard the privileged source-log reconciliation boundary.

Both modifications use the installed TurboDB ORM SQLite driver, never raw
filesystem write tricks. An injected private field MUST stop publication /
reconciliation; an apparently valid but changed prior event MUST conflict
with its already committed consumer receipt. The consumer cursor never
advances following either failure.
"""
import os
import pathlib
import subprocess
import sys
import tempfile

def run(args, env, success=True):
    result=subprocess.run(args,env=env,capture_output=True,text=True,
                          timeout=15,check=False)
    if (result.returncode == 0) != success:
        raise AssertionError(
            "Unexpected archive reconciliation outcome: "+
            str([pathlib.Path(args[0]).name,*args[1:]])+
            "\nstatus="+str(result.returncode)+
            "\nstdout="+result.stdout[-2000:]+
            "\nstderr="+result.stderr[-2000:])
    return result

def main() -> int:
    if len(sys.argv) != 6:
        raise SystemExit("usage: ci_reconcile_negative.py "
                         "seed corrupt reconcile consumer-audit sqlite-driver")
    seed, corrupt, reconcile, audit, plugin=(
        str(pathlib.Path(p).resolve()) for p in sys.argv[1:])
    env=os.environ.copy()
    ns="mygames-reconcile-test-source"
    consumer="backend-projection-v1"
    with tempfile.TemporaryDirectory(prefix="mygames-archive-negative-") as root:
        for scenario in ("private", "forged-valid"):
            source=str(pathlib.Path(root)/f"{scenario}-source.sqlite")
            projection=str(pathlib.Path(root)/f"{scenario}-consumer.sqlite")
            run([seed,plugin,source,"--seed"],env)
            if scenario == "private":
                run([corrupt,plugin,source,"--private"],env)
                rejected=run([reconcile,plugin,source,projection,ns,consumer,
                              "--catchup","128"],env,success=False)
                if "missing/corrupt committed event" not in rejected.stderr:
                    raise AssertionError("Did not reject private payload schema")
                run([audit,plugin,projection,ns,"0","0"],env)
            else:
                # Apply valid original sequence 1,2; afterwards rewrite
                # seq1 to different, still schema-valid canonical event.
                run([reconcile,plugin,source,projection,ns,consumer,
                     "--catchup","128"],env)
                run([audit,plugin,projection,ns,"2","2"],env)
                run([corrupt,plugin,source,"--forged-valid"],env)
                refused=run([reconcile,plugin,source,projection,ns,consumer,
                             "--verify-all","128"],env,success=False)
                if "consumer refused seq=1" not in refused.stderr:
                    raise AssertionError("Forged historical event was not detected")
                run([audit,plugin,projection,ns,"2","2"],env)
        # The explicit batch cap must report INCOMPLETE without inventing
        # an async retry. Accepted first-page effects are durable, and the
        # next invocation resumes exactly at the following sequence.
        bounded_source=str(pathlib.Path(root)/"bounded-source.sqlite")
        bounded_consumer=str(pathlib.Path(root)/"bounded-consumer.sqlite")
        run([seed,plugin,bounded_source,"--seed"],env)
        first=run([reconcile,plugin,bounded_source,bounded_consumer,ns,consumer,
                   "--catchup","1"],env,success=False)
        if first.returncode != 3 or "bounded batch incomplete" not in first.stderr:
            raise AssertionError("Batch capacity was not explicitly reported")
        run([audit,plugin,bounded_consumer,ns,"1","1"],env)
        run([reconcile,plugin,bounded_source,bounded_consumer,ns,consumer,
             "--catchup","1"],env)
        run([audit,plugin,bounded_consumer,ns,"2","2"],env)
        verify=run([reconcile,plugin,bounded_source,bounded_consumer,ns,consumer,
                    "--verify-all","1"],env,success=False)
        if verify.returncode != 3:
            raise AssertionError("Historical verification bypassed batch bound")
        run([reconcile,plugin,bounded_source,bounded_consumer,ns,consumer,
             "--verify-all","128"],env)
        run([audit,plugin,bounded_consumer,ns,"2","2"],env)
        print("mygames.reconcile_fail_closed: PASS "
              "(private payload reject, changed receipt conflict, no effect)",flush=True)
    return 0

if __name__=="__main__":
    raise SystemExit(main())
