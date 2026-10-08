import os,json,pathlib,subprocess,time,re,hashlib,datetime
root=pathlib.Path.cwd(); out=root/'build/local-owner-metadata-pair-retry'
cmd=json.loads((root/'validation/local-owner-metadata-control/command.json').read_text())
env=os.environ.copy(); env.update(json.loads((root/'validation/local-owner-metadata-control/env.json').read_text()))
hw=next(pathlib.Path('/sys/class/drm/card1/device/hwmon').glob('hwmon*'),None)
result={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'scope':'one-line source control; desktop background uncontrolled; fixture only, not model throughput','runs':[],'runtime_sha256':{p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in ['build/metadata-before-control/libzvram_layer.so','build/bp16-codec/libzvram_layer.so']}}
for i,variant in enumerate(['before','current','before','current']):
 c=cmd.copy()
 if variant=='before': c[c.index('--build-dir')+1]='build/metadata-before-control'
 clocks={p.name:p.read_text().strip() for p in hw.iterdir() if p.name in ['freq1_input','freq2_input','temp1_input','temp2_input','power1_average']} if hw else {}
 start=time.monotonic()
 try:
  p=subprocess.run(c,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=35)
  output=p.stdout; code=p.returncode
 except subprocess.TimeoutExpired as e: output=(e.stdout or b'').decode() if isinstance(e.stdout,bytes) else e.stdout or '';code=124
 (out/f'{i}-{variant}.log').write_text(output)
 match=re.findall(r'GPU BP16 encode calls=(\d+) raw-bytes=(\d+) host-ns=(\d+) fallbacks=(\d+) raw-snapshots=(\d+) final=1',output)
 row={'variant':variant,'exit':code,'wall_seconds':time.monotonic()-start,'command':c,'clocks':clocks,'encode_final':match[-1] if match else None,'pass':code==0 and 'PASS' in output,'vuid_count':len(re.findall('VUID-',output))}
 result['runs'].append(row); (out/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps(row),flush=True)
 if code:break
