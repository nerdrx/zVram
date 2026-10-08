import os,json,hashlib,time,re
from datetime import datetime
from zoneinfo import ZoneInfo
from pathlib import Path
import check_vulkan_idle_model as m
from check_model import clean_environment
r=Path.cwd();out=r/'build/internlm-bp16-gpu-spin-cold26-owner26-resident19-lfu'
m.Prompt=b"Write a 300-word story about a fox exploring a forest. Continue the story until you have written at least 300 words.\n"
env=clean_environment();env.pop('ROCPROFILER_REGISTER_LIBRARY',None);env.pop('ROCPROFILER_REGISTER_SECURE',None)
env.update(GGML_VK_VISIBLE_DEVICES='0',GGML_VK_DISABLE_HOST_VISIBLE_VIDMEM='1',GGML_VK_MAX_NODES_PER_SUBMIT='4',MALLOC_ARENA_MAX='2',VK_LOADER_LAYERS_DISABLE='~implicit~',VK_ICD_FILENAMES='/usr/share/vulkan/icd.d/radeon_icd.json')
env['ZVRAM_VULKAN_CLEAN_CACHE_POLICY']='lfu'
env.update(MALLOC_MMAP_THRESHOLD_='131072',MALLOC_TRIM_THRESHOLD_='131072')
os.nice(10)
report={'scope':'Opt-in bounded 1ms fence polling plus clean-first resident victim ranking plus synchronous GPU BP16 encoding; bothpasses fenced, finalcanonicalownedframe; Immutable-owner validation cache plus26GiB cold/owner ceiling (samequota); restored knownsafe19GiB resident hardcap and2.5GiB reserve;LFU;32encoder/8upload;fixed128KiB childallocator;16GiB availableRAMguard;sequential unlocked clocks;not isolated attribution','prompt_utf8':m.Prompt.decode(),'source_sha256':{f:hashlib.sha256((r/f).read_bytes()).hexdigest() for f in ['clean_cache_policy.hpp','bp16_codec.hpp','layer.cpp','gdeflate_gpu.hpp','submission_hooks.inc','check_vulkan_idle_model.py']},'runs':{}}
for label,automatic in [('automatic',True)]:
 remaining=datetime(2026,10,8,8,tzinfo=ZoneInfo('Europe/Berlin')).timestamp()-time.time()
 if remaining<=0:report['error']='GPU testing window expired';break
 runenv=env.copy()
 if automatic:runenv.update(ZVRAM_VULKAN_BP16_SPIN_WAIT='1',ZVRAM_VULKAN_CLEAN_FIRST_EVICTION='1',ZVRAM_VULKAN_BP16_RESTORE_BATCH='0',ZVRAM_VULKAN_BP16_GPU_ENCODE='1',ZVRAM_VULKAN_GPU_PROFILE='1',ZVRAM_VULKAN_BP16_ALLOCATED_HOST_INPUT='1',ZVRAM_VULKAN_BP16_ALLOCATED_HOST_MIB='26624',ZVRAM_VULKAN_BP16_UPLOAD_WORKERS='8')
 command=json.loads((out/(label+'-command.json')).read_text())
 try:
  result=m.run_interactive(label,command,runenv,out,min(1800,remaining),automatic,min_available_mib=16384,pressure_on_first_submit=automatic,max_swap_growth_mib=16384,reject_ollama_gpu=True)
  result['stdout_sha256']=hashlib.sha256(result.pop('stdout')).hexdigest();result.pop('stderr',None);report['runs'][label]=result
  if automatic:
   text=(out/(label+'.stderr.txt')).read_text();report['allocated_input_counters']=[dict(allocations=int(a),reuses=int(b),bytes=int(c)) for a,b,c in re.findall(r'GPU BP16 allocated input allocations=(\d+) reuses=(\d+) bytes=(\d+)',text)]
  print(json.dumps({'label':label,'performance':result.get('performance'),'offload':result.get('offload'),'sha':result['stdout_sha256']},indent=2),flush=True)
 except Exception as error:report['error']=str(error);break
 finally:(out/'automatic-result.json').write_text(json.dumps(report,indent=2)+'\n')
if len(report['runs'])==2:
 report['output_matches']=report['runs']['native']['stdout_sha256']==report['runs']['automatic']['stdout_sha256'];(out/'automatic-result.json').write_text(json.dumps(report,indent=2)+'\n');print('Output matches:',report['output_matches'],flush=True)
