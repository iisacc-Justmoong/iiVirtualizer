#!/usr/bin/env python3
"""Real SSH + TLS account authority; optionally controls two real Linux VMs."""
import argparse
import atexit
import base64
import concurrent.futures
import http.server
import hashlib
import json
import os
from pathlib import Path
import pwd
import secrets
import socket
import ssl
import subprocess
import sys
import threading
import time
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from test_isolation import require_test_container

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gateway',type=Path,required=True)
    parser.add_argument('--runtime',type=Path,required=True)
    parser.add_argument('--root',type=Path,required=True)
    parser.add_argument('--openssl',default='openssl')
    parser.add_argument('--ssh',default='/usr/bin/ssh')
    parser.add_argument('--sshd',default='/usr/sbin/sshd')
    parser.add_argument('--vms',action='store_true')
    parser.add_argument('--processes',action='store_true',help='Exercise supervised processes and monitoring; requires --vms --files')
    parser.add_argument('--files',action='store_true',help='Exercise confined guest Files CRUD; requires --vms')
    parser.add_argument('--qemu',type=Path)
    parser.add_argument('--image',type=Path)
    parser.add_argument('--accel',choices=['hvf','kvm','tcg'],default='hvf')
    args=parser.parse_args()
    if args.vms:require_test_container(args.accel)
    if args.files and not args.vms:parser.error('--files requires --vms')
    if args.processes and not (args.vms and args.files):parser.error('--processes requires --vms --files')
    root=args.root.resolve()/secrets.token_hex(3);root.mkdir(parents=True);root.chmod(0o700)
    def remove_private_keys():
        for name in ['host','client','wrong-host','authority.key','wrong-ca.key']:(root/name).unlink(missing_ok=True)
    atexit.register(remove_private_keys)
    gateway=args.gateway.resolve();runtime=args.runtime.resolve();lab=root/'lab';other=root/'other'
    records=[];auth_lock=threading.Lock();revoked=set();partial_errors=False;auth_count=0
    sessions={name:('proof-'+secrets.token_urlsafe(32),secrets.token_urlsafe(32)) for name in ['alice','bob','reader']}
    cookie=lambda name:f'iisacc_auth_id={sessions[name][0]}; iisacc_login_session={sessions[name][1]}'
    secret_values=[value for pair in sessions.values() for value in pair]
    def command(values,**kwargs):
        return subprocess.run(list(map(str,values)),check=True,capture_output=True,timeout=30,**kwargs)
    def write(path,value):path.write_text(json.dumps(value,indent=2)+'\n');path.chmod(0o600)
    tls_config=root/'tls.cnf'
    tls_config.write_text('[req]\ndistinguished_name=dn\nx509_extensions=ext\nprompt=no\n[dn]\nCN=localhost\n[ext]\nsubjectAltName=DNS:localhost,IP:127.0.0.1\nbasicConstraints=critical,CA:TRUE\nkeyUsage=critical,digitalSignature,keyEncipherment,keyCertSign\nextendedKeyUsage=serverAuth\n')
    for name in ['authority','wrong-ca']:
        command([args.openssl,'req','-x509','-newkey','rsa:2048','-nodes','-days','1','-config',tls_config,'-keyout',root/(name+'.key'),'-out',root/(name+'.crt')])
    class Authority(http.server.BaseHTTPRequestHandler):
        def log_message(self,*_):pass
        def do_POST(self):
            nonlocal auth_count
            body=self.rfile.read(int(self.headers.get('Content-Length','0')))
            query=json.loads(body)
            assert self.path=='/Account/GraphQL' and 'accountSession' in query['query']
            assert self.headers.get('Origin')==issuer
            who=next((name for name in sessions if self.headers.get('Cookie')==cookie(name)),None)
            with auth_lock:
                auth_count+=1
                account=None if who is None or who in revoked else {'sub':who,'email':who+'@example.test','userId':who+'-public'}
                value={'data':{'accountSession':{'account':account}}}
                if partial_errors:value['errors']=[{'message':'fixture-error'}]
            encoded=json.dumps(value).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(encoded)));self.end_headers();self.wfile.write(encoded)
    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Authority)
    issuer=f'https://localhost:{server.server_address[1]}'
    context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.load_cert_chain(root/'authority.crt',root/'authority.key')
    server.socket=context.wrap_socket(server.socket,server_side=True)
    authority_thread=threading.Thread(target=server.serve_forever,daemon=True);authority_thread.start()
    # The TLS server has loaded its key; these private files are no longer needed.
    (root/'authority.key').unlink();(root/'wrong-ca.key').unlink()
    for name in ['host','client','wrong-host']:
        command(['/usr/bin/ssh-keygen','-q','-t','ed25519','-N','','-f',root/name])
    (root/'authorized_keys').write_text('restrict '+(root/'client.pub').read_text());(root/'authorized_keys').chmod(0o600)
    probe=socket.socket();probe.bind(('127.0.0.1',0));ssh_port=probe.getsockname()[1];probe.close()
    for name,file in [('host','known_hosts'),('wrong-host','wrong_known_hosts')]:
        fields=(root/(name+'.pub')).read_text().split();(root/file).write_text(f'[127.0.0.1]:{ssh_port} {fields[0]} {fields[1]}\n')
    if args.vms:
        if not args.qemu or not args.image:parser.error('--vms requires --qemu and --image')
        kernel=args.image.resolve()/'Image';initrd=args.image.resolve()/'initramfs.cpio.gz'
    else:
        kernel=root/'kernel';initrd=root/'initramfs';kernel.write_bytes(b'fixture');initrd.write_bytes(b'fixture')
    qemu=str(args.qemu.resolve()) if args.qemu else 'qemu-system-aarch64'
    actions=['list','create','describe','start','pause','resume','stop','force-stop','remove','guest-info']
    if args.files:actions+=['file-list','file-stat','file-read','file-create','file-write','file-mkdir','file-move','file-remove']
    if args.processes:actions+=['process-launch','process-list','process-stat','process-read','process-write','process-close-input','process-stop','process-release','vm-monitor']
    config={'authority':{'endpoint':issuer+'/Account/GraphQL','ca_file':str(root/'authority.crt'),'timeout_ms':5000},
            'runtimes':[{'id':name,'root':str(folder),'qemu_aarch64':qemu,'images':[{'id':'iios','kernel':str(kernel),'initramfs':str(initrd),'architecture':'aarch64','accelerator':args.accel}]} for name,folder in [('lab',lab),('other',other)]],
            'grants':[{'subject':'alice','runtime':'lab','instance_prefix':'alice-','actions':actions,'max_cpus':2,'max_memory_mib':1024,'max_disk_mib':256},
                      {'subject':'bob','runtime':'lab','instance_prefix':'bob-','actions':['list','describe']},
                      {'subject':'bob','runtime':'other','instance_prefix':'bob-','actions':['list','create','describe','remove']}]}
    if not args.vms:
        for definition in config['runtimes']:
            definition['images'][0]['cpu_model']='max,pauth-impdef=on'
    if args.files:config['grants'].append({'subject':'reader','runtime':'lab','instances':['alice-a'],'actions':['file-list','file-stat','file-read']})
    if args.processes:config['grants'][-1]['actions']+=['process-list','process-stat','process-read','vm-monitor']
    server_file=root/'server.json';write(server_file,config)
    sshd_config=root/'sshd_config';user=pwd.getpwuid(os.getuid()).pw_name
    sshd_config.write_text(f'Port {ssh_port}\nListenAddress 127.0.0.1\nHostKey {root}/host\nPidFile {root}/sshd.pid\nAuthorizedKeysFile {root}/authorized_keys\nStrictModes no\nPasswordAuthentication no\nKbdInteractiveAuthentication no\nUsePAM no\nPubkeyAuthentication yes\nAllowUsers {user}\nDisableForwarding yes\nPermitTTY no\nPermitUserRC no\nLogLevel ERROR\nSubsystem iivirtualizer {gateway} agent {server_file}\nForceCommand {gateway} agent {server_file}\n')
    client={'host':'127.0.0.1','port':ssh_port,'user':user,'known_hosts':str(root/'known_hosts'),'identity_file':str(root/'client'),'ssh_executable':args.ssh,'runtime':'lab','issuer':issuer,'subject':'alice','timeout_ms':120000}
    client_file=root/'client.json';write(client_file,client)
    log=(root/'sshd.log').open('wb');daemon=subprocess.Popen([args.sshd,'-D','-e','-f',str(sshd_config)],stdout=log,stderr=log)
    result={'passed':False,'real_ssh':True,'real_tls_authority':True,'production_authenticated_account':False,'real_vms':args.vms,'records':records}
    def call(action,ids=None,create=None,expected=True,client_override=None,proof=None,runtime_id=None,file=None,process=None):
        request={'action':action}
        if ids is not None:request['instances']=ids
        if create is not None:request['create']=create
        if runtime_id is not None:request['runtime']=runtime_id
        if file is not None:request['file']=file;request['timeout_ms']=15000
        if process is not None:request['process']=process;request['timeout_ms']=15000
        request_file=root/'request.json';write(request_file,request)
        selected=client_file
        if client_override is not None:selected=root/'override.json';write(selected,{**client,**client_override})
        completed=subprocess.run([str(gateway),'call',str(selected),str(request_file),'--session-stdin'],input=(cookie('alice') if proof is None else proof).encode(),capture_output=True,timeout=150)
        output=completed.stdout.decode();error=completed.stderr.decode()
        if any(secret in output+error for secret in secret_values):raise AssertionError('Credential leaked in gateway output')
        value=json.loads(output) if output.strip() else None
        records.append({'action':action,'runtime':request.get('runtime','lab'),'ids':ids,'exit_code':completed.returncode,'reply':value,'stderr':error})
        if expected is True:assert completed.returncode==0 and value['ok'],(action,error,value)
        elif expected is False:assert completed.returncode!=0,(action,value)
        return value
    def pop(action,instance='alice-a',actor='alice',error=False,**values):
        reply=call('vm-monitor' if action=='monitor' else 'process-'+action,[instance],expected=None if error else True,
                   client_override={'subject':actor},proof=cookie(actor),process=values)
        if error:return reply
        assert reply['items'][0]['ok'],reply
        return reply['items'][0]['process']
    def create(id,port=0,cpus=1,memory=256,expected=True):
        value={'id':id,'image':'iios','cpus':cpus,'memory_mib':memory,'disk_mib':64,'network':bool(port),'forwards':[{'host':port,'guest':8080}] if port else []}
        return call('create',create=value,expected=expected)
    def denied(value,code):assert not value['ok'] and value['error']['code']==code,value
    def wait_guest(ids):
        for _ in range(20):
            value=call('guest-info',ids,expected=None)
            if value and value['ok'] and all(item['ok'] and 'volume=mounted' in item['guest'] for item in value['items']):return value
            time.sleep(.1)
        raise AssertionError('Remote guest readiness failed')
    ports=[]
    encode=lambda data:base64.b64encode(data).decode()
    def fop(action,path='',instance='alice-a',actor='alice',error=False,**values):
        value=call('file-'+action,[instance],file={'path':path,**values},expected=not error,
                   client_override={'subject':actor},proof=cookie(actor))
        if error:return value
        assert value['items'][0]['ok'] and 'file' in value['items'][0],value
        return value['items'][0]['file']
    def item_error(value,code):assert value and value['ok'] and not value['items'][0]['ok'] and value['items'][0]['error']['code']==code,value
    def fetch_http(index,boot,starting=False):
        # Guest-agent readiness precedes the separately launched HTTP workload.
        # Only startup read-only GETs wait for that service; steady-state checks
        # and all mutating gateway operations remain single attempts.
        deadline=time.monotonic()+15
        while True:
            try:body=urllib.request.urlopen(f'http://127.0.0.1:{ports[index]}/',timeout=5).read().decode();break
            except OSError:
                if not starting or time.monotonic()>=deadline:raise
                time.sleep(.1)
        assert f'boot={boot}\n' in body;return body
    try:
        for _ in range(100):
            if daemon.poll() is not None:raise RuntimeError('Test SSH server failed to start')
            try:
                with socket.create_connection(('127.0.0.1',ssh_port),timeout=.1):break
            except OSError:time.sleep(.02)
        if args.vms:
            bound=[socket.socket(),socket.socket()]
            for item in bound:item.bind(('127.0.0.1',0))
            ports=[item.getsockname()[1] for item in bound]
            for item in bound:item.close()
        create('alice-a',ports[0] if ports else 0)
        create('alice-b',ports[1] if ports else 0,2,384)
        denied(create('alice-over-limit',cpus=3,expected=False),2)
        assert not (lab/'alice-over-limit').exists()
        command([runtime,'create','bob-private','--kernel',kernel,'--initramfs',initrd,'--memory','64','--disk','16','--root',lab])
        listed=call('list');assert {item['id'] for item in listed['items']}=={'alice-a','alice-b'}
        if not args.vms:
            assert all(item['instance']['cpu_model']=='max,pauth-impdef=on' for item in listed['items'])
        bob=call('list',client_override={'subject':'bob'},proof=cookie('bob'));assert [item['id'] for item in bob['items']]==['bob-private']
        call('create',create={'id':'bob-remote','image':'iios','cpus':1,'memory_mib':64,'disk_mib':16},client_override={'subject':'bob'},proof=cookie('bob'),runtime_id='other')
        remote=call('list',client_override={'subject':'bob'},proof=cookie('bob'),runtime_id='other');assert [item['id'] for item in remote['items']]==['bob-remote']
        call('remove',['bob-remote'],client_override={'subject':'bob'},proof=cookie('bob'),runtime_id='other')
        denied(call('stop',['alice-a','bob-private'],expected=False),2)
        denied(call('list',expected=False,runtime_id='other'),2)
        denied(call('describe',['alice-a'],expected=False,client_override={'subject':'bob'}),1)
        with auth_lock:revoked.add('alice')
        denied(call('describe',['alice-a'],expected=False),1)
        with auth_lock:revoked.clear();partial_errors=True
        denied(call('describe',['alice-a'],expected=False),1)
        with auth_lock:partial_errors=False
        denied(call('describe',['alice-a'],expected=False,client_override={'issuer':'https://wrong.example.test'}),1)
        before=auth_count
        denied(call('describe',['alice-a'],expected=False,client_override={'known_hosts':str(root/'wrong_known_hosts')}),4)
        assert auth_count==before,'Account proof reached an untrusted host'
        config['authority']['ca_file']=str(root/'wrong-ca.crt');write(server_file,config)
        denied(call('describe',['alice-a'],expected=False),7)
        config['authority']['ca_file']=str(root/'authority.crt');write(server_file,config)
        if args.vms:
            call('start',['alice-a','alice-b']);ready=wait_guest(['alice-a','alice-b']);result['guest_ready']=ready
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:result['http_initial']=list(pool.map(lambda index:fetch_http(index,1,starting=True),range(2)))
            if args.files:
                assert all(' files_api=1 ' in item['guest'] for item in ready['items']),ready
                fop('mkdir','api-work');fop('mkdir','api-work/자료')
                payload=bytes(range(256))*64;path='api-work/자료/binary.dat';moved='api-work/자료/moved.dat'
                created=fop('create',path,data_base64=encode(payload));assert created['entry']['size']==len(payload)
                item_error(fop('create',path,data_base64=encode(b'overwrite'),error=True),3)
                tail=b'\x00tail\xff'+('추가 데이터'.encode()*32)
                fop('write',path,offset=len(payload),data_base64=encode(tail));expected=payload+tail
                observed=b''
                while len(observed)<len(expected):
                    page=fop('read',path,offset=len(observed),length=8192);observed+=base64.b64decode(page['data_base64'])
                    assert page['offset']==len(observed)-len(base64.b64decode(page['data_base64']))
                assert observed==expected and fop('stat',path)['entry']['size']==len(expected)
                fop('write',path,offset=5,data_base64=encode(b'edited'));expected=expected[:5]+b'edited'+expected[11:]
                assert base64.b64decode(fop('read',path,length=16384)['data_base64'])==expected[:16384]
                item_error(fop('write',path,offset=len(expected)+1,data_base64=encode(b'gap'),error=True),1)
                fop('create','api-work/자료/keep.dat',data_base64=encode(b'keep'))
                item_error(fop('move',path,destination='api-work/자료/keep.dat',error=True),3)
                assert base64.b64decode(fop('read','api-work/자료/keep.dat')['data_base64'])==b'keep'
                fop('move',path,destination=moved)
                item_error(fop('read',path,error=True),2)
                assert base64.b64decode(fop('read',moved,actor='reader')['data_base64'])==expected[:16384]
                denied(fop('write',moved,actor='reader',data_base64=encode(b'denied'),error=True),2)
                denied(fop('read',moved,actor='bob',error=True),2)
                denied(fop('read',moved,instance='alice-b',actor='reader',error=True),2)
                for unsafe in ['/etc/passwd','../.ii-runtime/boot-count','資料/../../etc/passwd','a//b','a/./b','C:\\Windows','a\nb']:
                    assert fop('read',unsafe,error=True) is None
                assert fop('remove','',error=True) is None
                assert fop('write',moved,data_base64=encode(b'x'*16385),error=True) is None
                assert fop('read',moved,operation='write',error=True) is None
                for index in range(5):fop('create','api-work/page-'+str(index))
                names=[];cursor=''
                while True:
                    page=fop('list','api-work',limit=2,cursor=cursor)
                    assert not page['data_base64'] and len(page['entries'])<=2
                    names.extend(item['name'] for item in page['entries']);cursor=page['next_cursor']
                    if not cursor:break
                assert len(names)==6 and names==sorted(names),names
                item_error(fop('remove','api-work',error=True),10)
                fop('write',moved,offset=0,data_base64=encode(b'persistent-file'),truncate=True)
                fop('mkdir','api-work',instance='alice-b');fop('mkdir','api-work/자료',instance='alice-b')
                fop('create',moved,instance='alice-b',data_base64=encode(b'vm-b-isolated'))
                assert base64.b64decode(fop('read',moved)['data_base64'])==b'persistent-file'
                assert base64.b64decode(fop('read',moved,instance='alice-b')['data_base64'])==b'vm-b-isolated'
                result['files']={'binary_roundtrip_sha256':hashlib.sha256(observed).hexdigest(),'binary_bytes':len(observed),'metadata':True,'chunk_read_write':True,'no_replace_create_move':True,'pagination':True,'read_only_grant':True,'vm_isolation':True,'path_boundary':True,'nonrecursive_delete':True}
            if args.processes:
                from process_remote import exercise_processes,finish_process_checks
                result['processes']=exercise_processes(pop,fop)
            call('pause',['alice-a']);result['http_during_a_pause']=fetch_http(1,1)
            if args.files:
                item_error(fop('read',moved,error=True),10)
                assert base64.b64decode(fop('read',moved,instance='alice-b')['data_base64'])==b'vm-b-isolated'
            if args.processes:
                assert pop('stat',id=result['processes']['a'],error=True)['items'][0]['error']['code']==10
                assert pop('stat',id=result['processes']['b'],instance='alice-b')['process']['state']=='running'
            call('resume',['alice-a'])
            with auth_lock:revoked.add('alice')
            denied(call('stop',['alice-a'],expected=False),1)
            if args.files:denied(fop('read',moved,error=True),1)
            if args.processes:denied(pop('stat',id=result['processes']['a'],error=True),1)
            result['http_after_revocation']=[fetch_http(0,1),fetch_http(1,1)]
            with auth_lock:revoked.clear()
            running_remove=call('remove',['alice-a'],expected=False);assert running_remove['ok'] and not running_remove['items'][0]['ok']
            call('stop',['alice-a']);result['http_after_a_stop']=fetch_http(1,1)
            if args.files:item_error(fop('read',moved,error=True),10)
            if args.processes:assert pop('stat',id=result['processes']['a'],error=True)['items'][0]['error']['code']==10
            call('start',['alice-a']);again=wait_guest(['alice-a','alice-b']);result['guest_after_restart']=again
            result['http_after_restart']=[fetch_http(0,2,starting=True),fetch_http(1,1)]
            if args.files:
                assert base64.b64decode(fop('read',moved)['data_base64'])==b'persistent-file'
                assert base64.b64decode(fop('read',moved,instance='alice-b')['data_base64'])==b'vm-b-isolated'
                result['files']['reboot_persistence']=True
                fop('remove',moved);fop('remove','api-work/자료/keep.dat');fop('remove','api-work/자료')
                for index in range(5):fop('remove','api-work/page-'+str(index))
                fop('remove','api-work');item_error(fop('stat','api-work',error=True),2)
                fop('remove',moved,instance='alice-b');fop('remove','api-work/자료',instance='alice-b');fop('remove','api-work',instance='alice-b')
                result['files']['delete']=True
            if args.processes:finish_process_checks(pop,fop,result['processes'])
            call('stop',['alice-a','alice-b'])
        removed=call('remove',['alice-a','alice-b']);assert all(item['removed'] for item in removed['items'])
        result.update({'passed':True,'subject_binding':True,'runtime_and_vm_grants':True,'multiple_remote_runtimes':True,'resource_limits':True,'revocation':True,'graphql_partial_errors_rejected':True,'tls_trust_failure_rejected':True,'ssh_host_key_mismatch_rejected':True,'no_secret_output':True,'authority_requests':auth_count})
    except Exception as error:
        result['error']=str(error);raise
    finally:
        for folder in [lab,other]:
            if not folder.exists():continue
            for entry in folder.iterdir():
                if not entry.is_dir() or not (entry/'instance.json').exists():continue
                common=['--root',str(folder),'--qemu-aarch64',qemu]
                stopped=subprocess.run([str(runtime),'stop',entry.name,'--force',*common],capture_output=True,text=True,timeout=30)
                console=entry/'console.log'
                if console.exists():(root/(entry.name+'.console.log')).write_bytes(console.read_bytes())
                if stopped.returncode==0:subprocess.run([str(runtime),'remove',entry.name,*common],capture_output=True,text=True,timeout=30)
        daemon.terminate();daemon.wait(timeout=10);log.close();server.shutdown();server.server_close();authority_thread.join(timeout=5)
        remove_private_keys();atexit.unregister(remove_private_keys)
        result['test_private_keys_removed']=all(not (root/name).exists() for name in ['host','client','wrong-host','authority.key','wrong-ca.key'])
        for record in records:
            encoded=json.dumps(record)
            assert all(secret not in encoded for secret in secret_values)
        write(root/'result.json',result)
        print(json.dumps({'passed':result['passed'],'real_ssh':True,'real_tls':True,'real_vms':args.vms,'requests':len(records),'evidence':str(root/'result.json')}),flush=True)

if __name__=='__main__':main()
