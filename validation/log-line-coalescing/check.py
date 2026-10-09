#!/usr/bin/env python3
"""Check production logf output and short-line write cost without a GPU."""

import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def extract_logf(source):
    marker = "void logf(const char* fmt,...) {"
    start = source.index(marker)
    brace = source.index("{", start)
    depth = 0
    for end in range(brace, len(source)):
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
            if depth == 0:
                function = source[start:end + 1]
                break
    else:
        raise RuntimeError("could not find the end of production logf")
    function = function.replace("void logf(", "void logfActual(", 1)
    buffer_match = re.search(r"char line\[(\d+)\]", function)
    prefix_match = re.search(r'constexpr char prefix\[\]="([^"\\]*)";', function)
    if not buffer_match or not prefix_match:
        raise RuntimeError("production logf buffer/prefix format changed")
    return function, int(buffer_match.group(1)), prefix_match.group(1)


def build_harness(actual, buffer_bytes, prefix):
    prefix_cpp = json.dumps(prefix)
    reference = f'''void logfReference(const char* fmt,...) {{
    std::fputs({prefix_cpp},stderr);
    va_list ap; va_start(ap,fmt);
    std::vfprintf(stderr,fmt,ap);
    va_end(ap);
    std::fputc('\\n',stderr);
}}'''
    return f'''#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <clocale>
#include <chrono>
#include <fstream>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <cstdint>
#include <iostream>
{actual}
{reference}

using LogFunction=void(*)(const char*,...);
static std::uint64_t syscw() {{
    std::ifstream input("/proc/self/io");
    std::string key; std::uint64_t value=0;
    while(input>>key) {{
        if(key=="syscw:") {{ input>>value; return value; }}
        std::string rest; std::getline(input,rest);
    }}
    return 0;
}}
static void emit(LogFunction fn,const char* which) {{
    const std::size_t payload={buffer_bytes}-std::strlen({prefix_cpp})-1;
    if(!std::strcmp(which,"common")) fn("snapshot state event=restore resident=%llu failures=%llu",123456ull,0ull);
    else if(!std::strcmp(which,"boundary")) {{ std::string s(payload,'B'); fn("%s",s.c_str()); }}
    else if(!std::strcmp(which,"above")) {{ std::string s(payload+1,'A'); fn("%s",s.c_str()); }}
    else if(!std::strcmp(which,"long")) {{ std::string s(10000,'L'); fn("%s",s.c_str()); }}
    else if(!std::strcmp(which,"width")) fn("width=%1000s hex=%08x float=%-12.4f","x",42u,3.125);
    else if(!std::strcmp(which,"unicode")) fn("unicode=%s","paw 🐾 snowman ☃ café");
    else if(!std::strcmp(which,"negative")) {{ std::setlocale(LC_CTYPE,"C"); fn("bad-wide=%lc",static_cast<wint_t>(0x100)); }}
    else std::exit(2);
}}
int main(int argc,char** argv) {{
    if(argc<3) return 2;
    const bool useNew=std::strcmp(argv[1],"new")==0;
    const auto fn=useNew?logfActual:logfReference;
    if(std::strcmp(argv[2],"bench")==0) {{
        const int fd=open("/dev/null",O_WRONLY|O_CLOEXEC);
        if(fd<0 || dup2(fd,STDERR_FILENO)<0) return 3;
        close(fd);
        std::setvbuf(stderr,nullptr,_IONBF,0);
        const std::string payload(384,'P');
        constexpr unsigned lines=1000;
        const auto beforeWrites=syscw();
        const auto started=std::chrono::steady_clock::now();
        for(unsigned i=0;i<lines;i++)
            fn("snapshot state event=freeze resident=%llu cold-logical=%llu payload=%s sequence=%u",
               123456ull,654321ull,payload.c_str(),i);
        const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-started).count();
        const auto writes=syscw()-beforeWrites;
        std::printf("lines=%u syscw=%llu elapsed_ns=%lld\\n",lines,
                    static_cast<unsigned long long>(writes),static_cast<long long>(elapsed));
        return 0;
    }}
    emit(fn,argv[2]);
    return 0;
}}
'''


def main():
    source = (ROOT / "layer.cpp").read_text()
    actual, buffer_bytes, prefix = extract_logf(source)
    harness = build_harness(actual, buffer_bytes, prefix)
    with tempfile.TemporaryDirectory(prefix="zvram-log-coalescing-") as temp:
        cpp = Path(temp) / "logf_check.cpp"
        binary = Path(temp) / "logf_check"
        cpp.write_text(harness)
        compiler = os.environ.get("CXX", "g++")
        command = [compiler, "-std=c++17", "-O2", str(cpp), "-o", str(binary)]
        built = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
        if built.returncode:
            raise RuntimeError(f"CPU harness compile failed:\n{built.stderr}")

        cases = ("common", "boundary", "above", "long", "width", "unicode", "negative")
        for case in cases:
            old = subprocess.run([str(binary), "old", case], capture_output=True)
            new = subprocess.run([str(binary), "new", case], capture_output=True)
            if old.returncode or new.returncode:
                raise RuntimeError(f"output case failed: {case}")
            if old.stdout != new.stdout or old.stderr != new.stderr:
                raise RuntimeError(f"output mismatch for {case}: old={len(old.stderr)} new={len(new.stderr)}")

        timing = {"old_ns": [], "new_ns": [], "old_syscw": [], "new_syscw": []}
        for _ in range(5):
            for mode in ("old", "new"):
                result = subprocess.run([str(binary), mode, "bench"], capture_output=True, text=True)
                if result.returncode:
                    raise RuntimeError(f"{mode} benchmark failed: {result.stderr}")
                fields = dict(item.split("=") for item in result.stdout.split())
                timing[f"{mode}_ns"].append(int(fields["elapsed_ns"]))
                timing[f"{mode}_syscw"].append(int(fields["syscw"]))
        summary = {
            "output_cases_exact": list(cases),
            "benchmark_lines": 1000,
            "median_old_syscw": statistics.median(timing["old_syscw"]),
            "median_new_syscw": statistics.median(timing["new_syscw"]),
            "median_old_elapsed_ns": statistics.median(timing["old_ns"]),
            "median_new_elapsed_ns": statistics.median(timing["new_ns"]),
            "syscw_samples": timing,
        }
        print(json.dumps(summary, indent=2))
        if summary["median_new_syscw"] >= summary["median_old_syscw"]:
            raise RuntimeError("coalesced path did not reduce /proc/self/io syscw")


if __name__ == "__main__":
    main()
