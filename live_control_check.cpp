#include "live_control.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace zvram::control;
void check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
struct Sandbox {
    std::string path;
    Sandbox() { char name[]="/tmp/zvram-control-XXXXXX"; const auto result=mkdtemp(name); if(!result) throw std::runtime_error("mkdtemp"); path=result; }
    ~Sandbox() { std::error_code error; std::filesystem::remove_all(path,error); }
};
void writeRequest(const std::string& path,const std::string& text,mode_t mode=0600) {
    unlink(path.c_str()); int fd=open(path.c_str(),O_CREAT|O_EXCL|O_WRONLY,mode);
    check(fd>=0,"create fixture"); check(write(fd,text.data(),text.size())==static_cast<ssize_t>(text.size()),"write fixture"); close(fd);
}
int main() try {
    Request request;
    check(parse("version=1\nseq=7\nresident_mib=64\n",request),"valid request");
    check(request.seq==7&&request.residentMiB==64,"parsed numbers");
    for(const auto* bad:{"version=1\nseq=0\nresident_mib=1\n","version=2\nseq=1\nresident_mib=1\n",
        "version=1\nseq=1\nresident_mib=-1\n","version=1\nseq=1\nseq=2\nresident_mib=1\n",
        "version=1\nseq=1\nresident_mib=18446744073709551616\n","version=1\nseq=1\nresident_mib=1\nextra=1\n"})
        check(!parse(bad,request),"malformed request accepted");
    check(!parse(std::string(1025,'0'),request),"oversized parser input");
    State state; std::uint64_t limit=128*MiB;
    check(!state.accept({1,64},true,32*MiB,256*MiB,0,0,96*MiB,limit),"lower pending");
    check(state.result==1&&state.ack==0&&limit==128*MiB,"pending changed live cap");
    check(!state.settle(true,65*MiB,limit),"premature cap application");
    check(state.settle(true,64*MiB,limit)&&limit==64*MiB&&state.ack==1,"pending not settled");
    check(!state.accept({1,256},true,32*MiB,256*MiB,0,0,0,limit)&&limit==64*MiB,"replay accepted");
    check(!state.accept({2,1},true,32*MiB,256*MiB,0,0,0,limit)&&state.reason==2,"bounds");
    check(!state.accept({3,128},true,32*MiB,256*MiB,160*MiB,64*MiB,0,limit)&&state.reason==3,"local owner bound");
    check(!state.accept({4,128},false,32*MiB,256*MiB,0,0,0,limit)&&state.reason==1,"unsupported");
    check(state.accept({5,128},true,32*MiB,256*MiB,0,0,0,limit)&&state.ack==5&&limit==128*MiB,"raise cap");
    check(!state.accept({6,32},true,32*MiB,256*MiB,0,0,128*MiB,limit),"pending fixture");
    check(!state.settle(false,0,limit)&&state.reason==1&&state.ack==6,"disabled pending control");
    State overshoot; std::uint64_t overshootLimit=64*MiB;
    check(overshoot.accept({1,64},true,32*MiB,256*MiB,0,0,96*MiB,overshootLimit)&&
        overshoot.ack==1&&overshoot.result==0,"same cap overshoot incorrectly pending");
    check(overshoot.accept({2,128},true,32*MiB,256*MiB,0,0,192*MiB,overshootLimit)&&
        overshoot.ack==2&&overshootLimit==128*MiB,"raised cap overshoot incorrectly pending");
    Sandbox sandbox; Endpoint endpoint;
    check(!endpoint.init(nullptr,1),"unset environment activated IPC");
    check(endpoint.init(sandbox.path.c_str(),42),"endpoint init");
    const auto directory=sandbox.path+"/"+std::to_string(getpid())+"-"+std::to_string(processStart());
    const auto file=directory+"/42.request";
    writeRequest(file,"version=1\nseq=1\nresident_mib=64\n");
    check(endpoint.read(request),"valid private request rejected");
    chmod(file.c_str(),0644); check(!endpoint.read(request),"public request accepted");
    writeRequest(file,std::string(1025,'0')); check(!endpoint.read(request),"oversized file accepted");
    unlink(file.c_str()); symlink("/dev/zero",file.c_str()); check(!endpoint.read(request),"request symlink accepted");
    unlink(file.c_str()); mkfifo(file.c_str(),0600); check(!endpoint.read(request),"FIFO request accepted");
    unlink(file.c_str()); writeRequest(file,"version=1\nseq=1\nresident_mib=64\n");
    const auto hardlink=directory+"/other"; link(file.c_str(),hardlink.c_str()); check(!endpoint.read(request),"hardlink request accepted"); unlink(hardlink.c_str());
    if(getuid()==0) { chown(file.c_str(),65534,65534); check(!endpoint.read(request),"foreign request accepted"); }
    check(endpoint.publish(true,32*MiB,64*MiB,32*MiB,256*MiB),"status publish");
    struct stat st{}; check(stat((directory+"/42.status").c_str(),&st)==0&&(st.st_mode&0777)==0600,"status permissions");
    check(endpoint.due()&&!endpoint.due(),"poll throttle");
    endpoint.reset(); check(!std::filesystem::exists(directory),"teardown leaked files");
    const auto alias=sandbox.path+"-link"; symlink(sandbox.path.c_str(),alias.c_str());
    check(!endpoint.init(alias.c_str(),1),"base symlink accepted"); unlink(alias.c_str());
    chmod(sandbox.path.c_str(),0755); check(!endpoint.init(sandbox.path.c_str(),1),"public base accepted");
    std::cout<<"PASS: private live control IPC, sequence, bounds, pending application and cleanup\n";
    return 0;
} catch(const std::exception& error) { std::cerr<<"FAIL: "<<error.what()<<'\n'; return 1; }
