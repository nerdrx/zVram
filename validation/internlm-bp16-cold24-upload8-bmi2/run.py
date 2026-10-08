import os,json,hashlib,time,re
from datetime import datetime
from zoneinfo import ZoneInfo
from pathlib import Path
from check_vulkan_idle_model import run_interactive
from check_model import clean_environment
r=Path.cwd(); out=r/'build/internlm-bp16-cold24-upload8-bmi2'
command=json.loads((out/'command.json').read_text())
env=clean_environment(); env.pop('ROCPROFILER_REGISTER_LIBRARY',None); env.pop('ROCPROFILER_REGISTER_SECURE',None)
env.update(GGML_VK_VISIBLE_DEVICES='0',GGML_VK_DISABLE_HOST_VISIBLE_VIDMEM='1',GGML_VK_MAX_NODES_PER_SUBMIT='4',MALLOC_ARENA_MAX='2',VK_LOADER_LAYERS_DISABLE='~implicit~',VK_ICD_FILENAMES='/usr/share/vulkan/icd.d/radeon_icd.json')
env['ZVRAM_VULKAN_GPU_PROFILE']='1'
env['ZVRAM_VULKAN_BP16_UPLOAD_WORKERS']='8'
env['ZVRAM_VULKAN_BP16_ALLOCATED_HOST_INPUT']='1'
env['ZVRAM_VULKAN_BP16_ALLOCATED_HOST_MIB']='8192'
os.nice(10)
remaining=datetime(2026,10,8,8,tzinfo=ZoneInfo('Europe/Berlin')).timestamp()-time.time()
if remaining<=0: raise SystemExit('GPU testing window expired')
report={'scope':'full wrapped large-model trial; GPU BP16 MRU19GiB,2.5GiB reserve,32 encoding workers,host+device phase profiling,BP16 no output prefill,8GiB bounded cached ordinary Vulkan allocated coherent/cached host input plus direct-host fallback; inlined BMI2 block packer;24GiB total compressed cold/clean-cache quota; 8 upload workers; hashes retained','source_sha256':{f:hashlib.sha256((r/f).read_bytes()).hexdigest() for f in ['bp16_codec.hpp','layer.cpp','gdeflate_gpu.hpp','submission_hooks.inc','submission_tracking.hpp','command_hooks.inc','check_vulkan_idle_model.py']}}
try:
 result=run_interactive('automatic',command,env,out,min(1800,remaining),True,min_available_mib=16384,pressure_on_first_submit=True,max_swap_growth_mib=16384,reject_ollama_gpu=True)
 result['stdout_sha256']=hashlib.sha256(result.pop('stdout')).hexdigest(); result.pop('stderr',None);report['result']=result
except Exception as error: report['error']=str(error)
text=(out/'automatic.stderr.txt').read_text();report['allocated_input_counters']=[dict(allocations=int(a),reuses=int(b),bytes=int(c)) for a,b,c in re.findall(r'GPU BP16 allocated input allocations=(\d+) reuses=(\d+) bytes=(\d+)',text)];report['diagnostics']=[line for line in text.splitlines() if 'access tracking fallback' in line or 'resident admission refused' in line or 'llama_perf' in line]
(out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({'error':report.get('error'),'performance':report.get('result',{}).get('performance'),'offload':report.get('result',{}).get('offload'),'stdout_sha256':report.get('result',{}).get('stdout_sha256'),'diagnostics':report.get('diagnostics')},indent=2))
