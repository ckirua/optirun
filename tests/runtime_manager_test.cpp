#include "hybrid_python/runtime_manager.hpp"
#include <chrono>
#include <functional>
#include <iostream>
#include <thread>
using namespace hybrid_python;
using namespace std::chrono_literals;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
Runtime make_runtime(char** argv,std::size_t max_pending=64){RuntimeConfig config;config.gil={argv[1],argv[3]};config.free_threaded={argv[2],argv[3]};config.max_pending=max_pending;Runtime r(std::move(config));for(auto n:{"echo","identity","raise_value_error","sleep_then_echo"})r.register_handler(n,{Backend::gil,Backend::free_threaded});r.start();return r;}
int main(int argc,char** argv){if(argc!=4)return 2;try{
 {auto r=make_runtime(argv);for(auto b:{Backend::gil,Backend::free_threaded}){auto info=r.worker_info(b);require(info.version[0]==3&&info.version[1]==14,"wrong worker version");require(info.gil_enabled==(b==Backend::gil),"wrong GIL state");require(std::get<std::string>(r.submit(b,"echo",{std::string("route")}).get())=="route","routing failure");}r.shutdown();}
 {auto r=make_runtime(argv);try{r.submit(Backend::gil,"raise_value_error",{}).get();throw std::runtime_error("missing remote exception");}catch(const RemoteException& e){require(e.error().type_name=="ValueError","wrong remote exception");require(e.error().message=="expected handler failure","wrong remote message");require(e.error().traceback.find("raise_value_error")!=std::string::npos,"missing traceback");}}
 {auto r=make_runtime(argv,1);auto slow=r.submit(Backend::gil,"sleep_then_echo",{std::string("slow"),0.15});try{(void)r.submit(Backend::free_threaded,"echo",{std::string("full")});throw std::runtime_error("missing backpressure");}catch(const std::runtime_error&){}require(std::get<std::string>(slow.get())=="slow","slow result failure");}
 {auto r=make_runtime(argv);auto pending=r.submit(Backend::gil,"sleep_then_echo",{std::string("drain"),0.05});r.shutdown();require(std::get<std::string>(pending.get())=="drain","shutdown did not drain");try{(void)r.submit(Backend::gil,"echo",{});throw std::runtime_error("submit accepted after shutdown");}catch(const std::logic_error&){}}
 std::cout<<"runtime_manager_test: ok\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
