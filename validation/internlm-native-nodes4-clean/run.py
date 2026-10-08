import os,json,hashlib,time
from datetime import datetime
from zoneinfo import ZoneInfo
from pathlib import Path
from check_vulkan_idle_model import run_interactive
from check_model import clean_environment
r=Path.cwd(); out=r/'build/internlm-native-nodes4-clean'
command=json.loads((out/'command.json').read_text())
env=clean_environment(); env.pop('ROCPROFILER_REGISTER_LIBRARY',None); env.pop('ROCPROFILER_REGISTER_SECURE',None)
env.update(GGML_VK_VISIBLE_DEVICES='0',GGML_VK_DISABLE_HOST_VISIBLE_VIDMEM='1',GGML_VK_MAX_NODES_PER_SUBMIT='4',MALLOC_ARENA_MAX='2',VK_LOADER_LAYERS_DISABLE='~implicit~',VK_ICD_FILENAMES='/usr/share/vulkan/icd.d/radeon_icd.json')
os.nice(10)
remaining=datetime(2026,10,8,8,tzinfo=ZoneInfo('Europe/Berlin')).timestamp()-time.time()
if remaining<=0: raise SystemExit('GPU testing window expired')
report={'scope':'fresh native nodes4 reference after BP16 optimizations; desktop and clocks are not locked','source_sha256':{f:hashlib.sha256((r/f).read_bytes()).hexdigest() for f in ['layer.cpp','submission_tracking.hpp','command_hooks.inc','check_vulkan_idle_model.py']}}
try:
 result=run_interactive('native',command,env,out,min(600,remaining),False,min_available_mib=16384,pressure_on_first_submit=True,max_swap_growth_mib=16384,reject_ollama_gpu=True)
 result['stdout_sha256']=hashlib.sha256(result.pop('stdout')).hexdigest(); result.pop('stderr',None);report['result']=result
except Exception as error: report['error']=str(error)
text=(out/'native.stderr.txt').read_text();report['diagnostics']=[line for line in text.splitlines() if 'access tracking fallback' in line or 'resident admission refused' in line or 'llama_perf' in line]
(out/'native-result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({'error':report.get('error'),'performance':report.get('result',{}).get('performance'),'offload':report.get('result',{}).get('offload'),'stdout_sha256':report.get('result',{}).get('stdout_sha256'),'diagnostics':report.get('diagnostics')},indent=2))
