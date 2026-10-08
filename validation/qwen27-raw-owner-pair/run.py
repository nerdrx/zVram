import pathlib,json,os,hashlib,time
import check_vulkan_idle_model as m
from check_model import clean_environment
out=pathlib.Path('build/qwen27-raw-owner-pair')
env=clean_environment()
for k in list(env):
 if k.startswith('ROCPROFILER'):env.pop(k)
env.update(GGML_VK_VISIBLE_DEVICES='0',GGML_VK_DISABLE_HOST_VISIBLE_VIDMEM='1',GGML_VK_MAX_NODES_PER_SUBMIT='4',MALLOC_ARENA_MAX='2',MALLOC_MMAP_THRESHOLD_='131072',MALLOC_TRIM_THRESHOLD_='131072',VK_LOADER_LAYERS_DISABLE='~implicit~',VK_ICD_FILENAMES='/usr/share/vulkan/icd.d/radeon_icd.json')
os.nice(10)
m.Prompt=b'Write a 300-word story about a fox exploring a forest. Continue until at least 300 words.\n'
report={'scope':'27B Q4 RAW mapped owner OFF/ON/OFF/ON; same compiled binary; forced12GiB residency,8GiB cold/owners; desktop background uncontrolled','runs':{}}
for label in ['off1','on1','off2','on2']:
 command=json.loads((out/('automatic-command.json')).read_text());runenv=env.copy()
 if True:runenv.update(ZVRAM_VULKAN_CLEAN_CACHE_POLICY='lfu',ZVRAM_VULKAN_CLEAN_FIRST_EVICTION='1',ZVRAM_VULKAN_BP16_GPU_ENCODE='1',ZVRAM_VULKAN_GPU_PROFILE='1',ZVRAM_VULKAN_BP16_ALLOCATED_HOST_INPUT='1',ZVRAM_VULKAN_BP16_ALLOCATED_HOST_MIB='8192',ZVRAM_VULKAN_BP16_UPLOAD_WORKERS='8')
 if label.startswith('on'):runenv['ZVRAM_VULKAN_BP16_RAW_HOST_INPUT']='1'
 (out/(label+'-env.json')).write_text(json.dumps({k:v for k,v in runenv.items() if k.startswith(("ZVRAM_","GGML_","MALLOC_","VK_"))},indent=2))
 try:
  r=m.run_interactive(label,command,runenv,out,min(600,1791455388-time.time()),True,min_available_mib=16384,pressure_on_first_submit=True,max_swap_growth_mib=16384,reject_ollama_gpu=True)
  r['stdout_sha256']=hashlib.sha256(r.pop('stdout')).hexdigest();r.pop('stderr',None);report['runs'][label]=r
  print(json.dumps({'label':label,'performance':r.get('performance'),'offload':r.get('offload'),'sha':r['stdout_sha256']}),flush=True)
 except Exception as e:report['error']=str(e);break
 finally:(out/'result.json').write_text(json.dumps(report,indent=2))
report['output_matches']=len(report['runs'])==4 and len({r['stdout_sha256'] for r in report['runs'].values()})==1
(out/'result.json').write_text(json.dumps(report,indent=2))
