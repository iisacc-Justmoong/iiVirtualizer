#!/usr/bin/env python3
import base64
import json
from pathlib import Path
import subprocess
import sys
import time

exe=Path(sys.argv[1]).resolve();root=Path(sys.argv[2]).resolve()/str(time.time_ns())[-8:]
root.mkdir(parents=True)
payload=bytes(range(256))*97
(root/'artifact.bin').write_bytes(payload)
def run(job):
    p=root/'job.json';p.write_text(json.dumps(job))
    result=subprocess.run([str(exe),str(p),'stdio'],capture_output=True,text=True,timeout=15)
    events=[json.loads(line[6:]) for line in result.stdout.splitlines() if line.startswith('IIVB1 ')]
    assert all(e['job']==job['job'] for e in events)
    return result,events
job={'job':'native-contract','workspace':str(root),'steps':[{'argv':[sys.executable,'-c','print("worker-ran")'],'timeout_ms':5000}],'artifacts':['artifact.bin']}
r,e=run(job);assert r.returncode==0 and e[-1]['kind']=='complete',r.stderr
assert b'worker-ran' in b''.join(base64.b64decode(x['data']) for x in e if x['kind']=='log')
assert b''.join(base64.b64decode(x['data']) for x in e if x['kind']=='chunk')==payload
job['artifacts']=['../outside'];r,e=run(job);assert r.returncode!=0 and e[-1]['kind']=='error'
job['artifacts']=[];job['steps'][0]['argv']=[sys.executable,'-c','raise SystemExit(7)']
r,e=run(job);assert r.returncode!=0 and e[-1]['kind']=='failed' and any(x.get('code')==7 for x in e)
job['steps'][0]['argv']=[sys.executable,'-c','import time; time.sleep(10)'];job['steps'][0]['timeout_ms']=20
r,e=run(job);assert r.returncode!=0 and e[-1]['kind']=='failed'
print('Native SDK process execution, streamed binary artifacts, failure and deadline contracts passed')
