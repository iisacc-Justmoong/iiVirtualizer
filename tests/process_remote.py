"""Bounded remote-process checks; invoked by the real SSH/TLS VM fixture."""
import base64
import hashlib
import json
import time


def exercise_processes(pop, fop):
    encode=lambda data:base64.b64encode(data).decode()
    decode=lambda reply:base64.b64decode(reply['data_base64'])
    def launch(key, mode, instance='alice-a', **options):
        return pop('launch', instance=instance, key=key, executable='@probe', arguments=[mode], **options)['process']
    def finished(identity, instance='alice-a'):
        deadline=time.monotonic()+20
        while True:
            state=pop('stat',id=identity,instance=instance)['process']
            if 'exit' in state and state['output_eof'] and state['error_eof']:return state
            assert time.monotonic()<deadline,('Process exit/pipe deadline',state)
            time.sleep(.05)
    def release(identity, instance='alice-a'):
        finished(identity,instance);assert pop('release',id=identity,instance=instance)['released']
    def read_all(identity, stream='stdout', actor='alice'):
        result=b'';offset=0;gap=False
        while True:
            page=pop('read',id=identity,stream=stream,offset=offset,length=16384,actor=actor)
            gap=gap or page['truncated'];result+=decode(page);offset=page['next_offset']
            if page['eof']:return result,gap
            assert page['next_offset']>page['offset'],page
    monitor=pop('monitor')['monitor'];assert monitor['processes_available'] and monitor['files_available'],monitor
    assert monitor['filesystem_profile']=='society-v1' and monitor['memory_total']>monitor['memory_available']>=0 and monitor['cpus']==1
    initial_boot=monitor['boot_id']
    echo=launch('echo-binary','echo');replay=launch('echo-binary','echo');assert replay['id']==echo['id']
    conflict=pop('launch',key='echo-binary',executable='@probe',arguments=['wait'],error=True);assert conflict['items'][0]['error']['code']==3,conflict
    payload=bytes(range(256))*64+b'\0binary-tail\xff'
    accepted=pop('write',id=echo['id'],offset=0,data_base64=encode(payload[:16384]))['process'];assert accepted['input_accepted']==16384
    duplicate=pop('write',id=echo['id'],offset=0,data_base64=encode(b'duplicate'),error=True);assert duplicate['items'][0]['error']['code']==1
    pop('write',id=echo['id'],offset=16384,data_base64=encode(payload[16384:]));pop('close-input',id=echo['id'])
    ended=finished(echo['id']);assert ended['exit']=={'code':7,'signal':0} and ended['input_written']==len(payload) and not ended['input_discarded'],ended
    for stream in ['stdout','stderr']:
        actual,gap=read_all(echo['id'],stream,actor='reader');assert actual==payload and not gap
    denied=pop('stop',id=echo['id'],actor='reader',error=True);assert not denied['ok'] and denied['error']['code']==2
    denied=pop('stat',id=echo['id'],instance='alice-b',actor='reader',error=True);assert not denied['ok'] and denied['error']['code']==2
    unknown=pop('stat',id=echo['id'],instance='alice-b',error=True);assert unknown['items'][0]['error']['code']==2
    release(echo['id'])
    flood=launch('flood-streams','flood');state=finished(flood['id']);assert state['exit']['code']==9 and state['output_end']==262144 and state['error_end']==262144,state
    for stream in ['stdout','stderr']:
        actual,gap=read_all(flood['id'],stream);assert gap and actual==bytes(range(256))*256
    release(flood['id'])
    timed=launch('lifetime-bound','wait',lifetime_ms=100);assert finished(timed['id'])['state']=='timed-out';release(timed['id'])
    fop('mkdir','process-work');fop('mkdir','process-work/nested')
    probe=launch('confined-probe','probe',working_directory='process-work',environment={'APP_VALUE':'configured','GLIBC_TUNABLES':'glibc.malloc.perturb=165','LANG':'C'})
    state=finished(probe['id']);data,_=read_all(probe['id']);checks=json.loads(data)
    assert state['exit']['code']==0 and checks['failed']==0 and checks['checks']>=12 and checks['cwd']=='/data/Society.societycontainer/Files/process-work' and checks['env']=='configured',(state,checks)
    assert decode(fop('read','process-work/process-inside'))==b'inside Files'
    release(probe['id'])
    install=launch('install-local','install',working_directory='process-work');assert finished(install['id'])['exit']['code']==0;release(install['id'])
    assert fop('stat','process-work/installed-probe')['entry']['size']>64
    local=pop('launch',key='files-executable',executable='process-work/installed-probe',arguments=['probe'],working_directory='process-work/nested')['process']
    state=finished(local['id']);data,_=read_all(local['id']);assert state['exit']['code']==0 and json.loads(data)['failed']==0
    release(local['id'])
    for executable in ['/bin/sh','../outside','@../outside']:
        assert pop('launch',key='invalid',executable=executable,error=True) is None
    assert pop('launch',key='invalid-env',executable='@probe',environment={'HOME':'/'},error=True) is None
    assert pop('launch',key='invalid-nul',executable='@probe',arguments=['a\0b'],error=True) is None
    fop('create','process-work/not-elf',data_base64=encode(b'not an executable'))
    invalid=pop('launch',key='bad-elf',executable='process-work/not-elf')['process'];assert finished(invalid['id'])['exit']['code']==125
    error,_=read_all(invalid['id'],'stderr');assert b'blocked:' in error;release(invalid['id'])
    redirected=pop('launch',key='bad-cwd',executable='@probe',arguments=['exit'],working_directory='process-work/process-escape-link')['process'];assert finished(redirected['id'])['exit']['code']==125;release(redirected['id'])
    tree=launch('owned-tree','tree',working_directory='process-work');time.sleep(.1)
    assert pop('stat',id=tree['id'])['process']['tasks']>=2
    stopped=pop('stop',id=tree['id'])['process'];assert stopped['state']=='stopped' and stopped['exit']['signal']==9
    finished(tree['id']);first=decode(fop('read','process-work/tree-child-ticks'));time.sleep(.15);assert decode(fop('read','process-work/tree-child-ticks'))==first
    assert pop('stop',id=tree['id'])['process']['state']=='stopped';release(tree['id'])
    orphan=launch('orphan-tree','orphan',working_directory='process-work');assert finished(orphan['id'])['exit']['code']==0
    first=decode(fop('read','process-work/tree-child-ticks'));time.sleep(.15);assert decode(fop('read','process-work/tree-child-ticks'))==first;release(orphan['id'])
    a=launch('live-a','cpu',memory_mib=32,pids=8);b=launch('live-b','wait',instance='alice-b')
    live=pop('stat',id=a['id'])['process'];assert live['cpu_usec']>0 and live['memory_bytes']>0 and live['tasks']>=1,live
    listed=pop('list',limit=1);assert listed['processes'] and len(listed['processes'])==1
    busy=pop('release',id=a['id'],error=True);assert busy['items'][0]['error']['code']==10
    result={'initial_boot':initial_boot,'a':a['id'],'b':b['id'],'binary_bytes':len(payload),'binary_sha256':hashlib.sha256(payload).hexdigest(),
            'stdin_stdout_stderr':True,'exit_status':True,'launch_idempotency':True,'ring_gap':True,'lifetime':True,'filesystem_checks':checks['checks'],
            'files_executable':True,'cgroup_owned_tree_stop':True,'escaped_orphan_cleanup':True,'resource_metrics':live,'monitor':monitor,'read_only_grant':True}
    return result


def finish_process_checks(pop, fop, state):
    monitor=pop('monitor')['monitor'];assert monitor['boot_id']!=state['initial_boot'] and monitor['retained']==0,monitor
    stale=pop('stat',id=state['a'],error=True);assert stale['items'][0]['error']['code']==2
    assert pop('stat',id=state['b'],instance='alice-b')['process']['state']=='running'
    pop('stop',id=state['b'],instance='alice-b')
    deadline=time.monotonic()+10
    while True:
        row=pop('stat',id=state['b'],instance='alice-b')['process']
        if row['output_eof'] and row['error_eof']:break
        assert time.monotonic()<deadline;time.sleep(.05)
    assert pop('release',id=state['b'],instance='alice-b')['released']
    for path in ['process-work/nested/process-inside','process-work/nested/process-escape-link','process-work/nested',
                 'process-work/process-inside','process-work/process-escape-link','process-work/tree-child-ticks','process-work/installed-probe','process-work/not-elf','process-work']:
        fop('remove',path)
    state.update({'reboot_handles_invalidated':True,'vm_isolation':True,'cleanup':True,'passed':True})
