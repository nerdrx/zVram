import os,json,hashlib,time
from pathlib import Path
from datetime import datetime
from zoneinfo import ZoneInfo
import check_vulkan_idle_model as m
from check_model import clean_environment
r=Path.cwd();out=r/'build/internlm-native-92-current-conditions'
m.Prompt=b"Write a 300-word story about a fox exploring a forest. Continue the story until you have written at least 300 words.\n"
env=clean_environment()
for k in ['ROCPROFILER_REGISTER_LIBRARY','ROCPROFILER_REGISTER_SECURE']:env.pop(k,None)
env.update(GGML_VK_VISIBLE_DEVICES='0',GGML_VK_DISABLE_HOST_VISIBLE_VIDMEM='1',GGML_VK_MAX_NODES_PER_SUBMIT='4',MALLOC_ARENA_MAX='2',MALLOC_MMAP_THRESHOLD_='131072',MALLOC_TRIM_THRESHOLD_='131072',VK_LOADER_LAYERS_DISABLE='~implicit~',VK_ICD_FILENAMES='/usr/share/vulkan/icd.d/radeon_icd.json')
os.nice(10)
report={'scope':'Native current-machine-condition reference after optionalbudget trials;92-token story, sequential unlocked clocks; no compression/paging snapshots/resident cap','prompt_utf8':m.Prompt.decode(),'runs':{}}
for label in ['native']:
 remaining=datetime(2026,10,8,10,4,31,tzinfo=ZoneInfo('Europe/Berlin')).timestamp()-time.time()
 if remaining<=0:report['error']='GPU testing window expired';break
 try:
  command=json.loads((out/(label+'-command.json')).read_text())
  result=m.run_interactive(label,command,env,out,min(1800,remaining),False,min_available_mib=16384,max_swap_growth_mib=16384,reject_ollama_gpu=True)
  result['stdout_sha256']=hashlib.sha256(result.pop('stdout')).hexdigest();result.pop('stderr',None);report['runs'][label]=result
  print(json.dumps({'label':label,'performance':result['performance'],'sha':result['stdout_sha256'],'offload':result['offload']},indent=2),flush=True)
 except Exception as error:report['error']=str(error);break
 finally:(out/'result.json').write_text(json.dumps(report,indent=2)+'\n')
