#pragma once
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace zvram::control {
constexpr std::uint64_t MiB=1024ull*1024ull;
struct Request { std::uint64_t seq{}, residentMiB{}; };
inline bool number(const std::string& text,std::uint64_t& value) {
    if(text.empty()) return false;
    value=0;
    for(char c:text) {
        if(c<'0'||c>'9'||value>(UINT64_MAX-static_cast<unsigned>(c-'0'))/10) return false;
        value=value*10+static_cast<unsigned>(c-'0');
    }
    return true;
}
inline bool parse(const std::string& text,Request& request) {
    if(text.size()>1024) return false;
    std::istringstream input(text); std::string line; unsigned fields=0;
    while(std::getline(input,line)) {
        const auto split=line.find('='); if(split==std::string::npos) return false;
        const auto key=line.substr(0,split); std::uint64_t value=0;
        if(!number(line.substr(split+1),value)) return false;
        unsigned bit=0;
        if(key=="version") { bit=1; if(value!=1) return false; }
        else if(key=="seq") { bit=2; request.seq=value; }
        else if(key=="resident_mib") { bit=4; request.residentMiB=value; }
        else return false;
        if(fields&bit) return false; fields|=bit;
    }
    return fields==7 && request.seq;
}
inline std::uint64_t processStart() {
    std::ifstream input("/proc/self/stat"); std::string text; std::getline(input,text);
    const auto end=text.rfind(')'); if(end==std::string::npos) return 0;
    std::istringstream fields(text.substr(end+1)); std::string field;
    for(unsigned i=3;i<=22;i++) if(!(fields>>field)) return 0;
    std::uint64_t result=0; return number(field,result)?result:0;
}
inline bool privateDirectory(int fd) {
    struct stat st{}; return fstat(fd,&st)==0 && S_ISDIR(st.st_mode) &&
        st.st_uid==getuid() && (st.st_mode&07777)==0700;
}
// Walk every component without following symlinks; ancestors may be system directories.
inline int openBase(const std::string& path) {
    if(path.empty()||path[0]!='/') return -1;
    int fd=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    std::istringstream parts(path); std::string part;
    while(std::getline(parts,part,'/')) {
        if(part.empty()) continue;
        if(part=="."||part=="..") { close(fd); return -1; }
        const int next=openat(fd,part.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        close(fd); fd=next; if(fd<0) return -1;
    }
    if(!privateDirectory(fd)) { close(fd); return -1; }
    return fd;
}
struct State {
    std::uint64_t seq{},ack{},requestedMiB{};
    unsigned result{},reason{};
    bool accept(const Request& request,bool capable,std::uint64_t minimumBytes,
                std::uint64_t maximumBytes,std::uint64_t combinedBytes,std::uint64_t ownerBytes,
                std::uint64_t residentBytes,std::uint64_t& limitBytes) {
        if(request.seq<=seq) return false;
        seq=request.seq; requestedMiB=request.residentMiB; reason=0;
        if(!capable) reason=1;
        else if(!requestedMiB || requestedMiB>UINT64_MAX/MiB ||
                requestedMiB*MiB<minimumBytes) reason=2;
        else if(combinedBytes && (ownerBytes>combinedBytes || requestedMiB*MiB>combinedBytes-ownerBytes)) reason=3;
        else if(requestedMiB*MiB>maximumBytes) reason=2;
        if(reason) { result=2; ack=seq; return false; }
        result=requestedMiB*MiB<limitBytes && residentBytes>requestedMiB*MiB?1:0;
        if(!result) { limitBytes=requestedMiB*MiB; ack=seq; }
        return !result;
    }
    bool settle(bool capable,std::uint64_t residentBytes,std::uint64_t& limitBytes) {
        if(result!=1) return false;
        if(!capable) { result=2; reason=1; ack=seq; }
        else if(residentBytes<=requestedMiB*MiB) {
            limitBytes=requestedMiB*MiB; result=0; ack=seq; return true;
        }
        return false;
    }
};
class Endpoint {
    int base_{-1},directory_{-1};
    std::string process_,device_,status_,request_,temporary_;
    std::uint64_t start_{};
    std::chrono::steady_clock::time_point next_{};
public:
    State state;
    Endpoint()=default;
    Endpoint(const Endpoint&)=delete;
    Endpoint& operator=(const Endpoint&)=delete;
    ~Endpoint() { reset(); }
    void reset() noexcept {
        if(directory_>=0) {
            unlinkat(directory_,status_.c_str(),0); unlinkat(directory_,request_.c_str(),0);
            unlinkat(directory_,temporary_.c_str(),0); close(directory_); directory_=-1;
        }
        if(base_>=0) { unlinkat(base_,process_.c_str(),AT_REMOVEDIR); close(base_); base_=-1; }
    }
    bool init(const char* base,std::uint64_t device) {
        if(!base||!*base) return false;
        start_=processStart(); if(!start_) return false;
        base_=openBase(base); if(base_<0) return false;
        process_=std::to_string(getpid())+"-"+std::to_string(start_);
        if(mkdirat(base_,process_.c_str(),0700)!=0 && errno!=EEXIST) { reset(); return false; }
        directory_=openat(base_,process_.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(directory_<0 || !privateDirectory(directory_)) { reset(); return false; }
        device_=std::to_string(device); status_=device_+".status";
        request_=device_+".request"; temporary_=device_+".status.tmp";
        return true;
    }
    bool active() const { return directory_>=0; }
    bool due() {
        if(!active()) return false;
        const auto now=std::chrono::steady_clock::now(); if(now<next_) return false;
        next_=now+std::chrono::milliseconds(250); return true;
    }
    bool read(Request& request) {
        const int fd=openat(directory_,request_.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
        if(fd<0) return false;
        struct stat st{}; bool valid=fstat(fd,&st)==0 && S_ISREG(st.st_mode) &&
            st.st_uid==getuid() && (st.st_mode&07777)==0600 && st.st_nlink==1 && st.st_size<=1024;
        char bytes[1025]; ssize_t size=-1;
        if(valid) { do { size=::read(fd,bytes,sizeof(bytes)); } while(size<0&&errno==EINTR); }
        close(fd);
        return size>0 && size<=1024 && parse(std::string(bytes,static_cast<std::size_t>(size)),request);
    }
    bool publish(bool capable,std::uint64_t residentBytes,std::uint64_t limitBytes,
                 std::uint64_t minimumBytes,std::uint64_t maximumBytes) {
        if(!active()) return false;
        const auto text=std::string("version=1\npid=")+std::to_string(getpid())+
            "\nstart="+std::to_string(start_)+"\ndevice="+device_+
            "\ncapable="+std::to_string(capable)+"\nresident_mib="+std::to_string(residentBytes/MiB+(residentBytes%MiB!=0))+
            "\ncurrent_limit_mib="+std::to_string(limitBytes/MiB)+
            "\nmin_limit_mib="+std::to_string(minimumBytes/MiB+(minimumBytes%MiB!=0))+
            "\nmax_limit_mib="+std::to_string(maximumBytes/MiB)+
            "\nrequested_mib="+std::to_string(state.requestedMiB)+"\nseq="+std::to_string(state.seq)+
            "\nack="+std::to_string(state.ack)+"\nresult="+std::to_string(state.result)+
            "\nreason="+std::to_string(state.reason)+"\n";
        const int fd=openat(directory_,temporary_.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if(fd<0) return false;
        std::size_t offset=0;
        while(offset<text.size()) {
            const auto count=write(fd,text.data()+offset,text.size()-offset);
            if(count<0&&errno==EINTR) continue;
            if(count<=0) break;
            offset+=static_cast<std::size_t>(count);
        }
        close(fd);
        const bool ok=offset==text.size() && renameat(directory_,temporary_.c_str(),directory_,status_.c_str())==0;
        if(!ok) unlinkat(directory_,temporary_.c_str(),0);
        return ok;
    }
};
}
