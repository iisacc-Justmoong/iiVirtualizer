#include <iiVirtualizer/Runtime.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace ii::virtualizer;
static void check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
int main(int argc,char** argv){
 try{
  check(argc==2,"explicit fixture root required");
  const auto root=fs::path(argv[1])/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  fs::create_directories(root/u8"원본");
  const auto kernel=root/u8"원본/kernel",initrd=root/u8"원본/initrd",base=root/u8"원본/base.raw";
  std::ofstream(kernel)<<"kernel fixture";std::ofstream(initrd)<<"initramfs fixture";std::ofstream(base)<<"preserved disk bytes";
  Backend backend{
   [](const Instance&)->Result<void>{return {};},
   [](const Instance&,Command)->Result<State>{return State::stopped;},
   [](const Instance&,std::string_view)->Result<std::string>{return std::string("not booted");}};
  const auto store=root/u8"저장소/nested";Runtime runtime(store,backend);
  InstanceSpec spec{.id="provider",.kernel=kernel,.initramfs=initrd,.base_disk=base};
  auto created=runtime.create(spec);check(bool(created),"provider-backed create failed");
  check(fs::file_size(created->paths.disk)==spec.disk_mib*1024*1024,"sparse volume size changed");
  std::ifstream input(created->paths.disk,std::ios::binary);std::string prefix(20,' ');input.read(prefix.data(),20);
  check(prefix=="preserved disk bytes","base disk prefix changed");
  check(fs::exists(kernel)&&fs::exists(initrd)&&fs::exists(base),"source was moved instead of copied");
  check(!runtime.create(spec),"existing instance overwritten");
  check(bool(runtime.remove(spec.id)),"provider-backed instance removal failed");
  const auto blocked=root/"blocked";std::ofstream(blocked)<<"keep";
  bool rejected=false;try{Runtime invalid(blocked/"nested",backend);}catch(const fs::filesystem_error&){rejected=true;}
  check(rejected&&fs::is_regular_file(blocked),"blocking parent was replaced");
  fs::remove_all(root);std::cout<<"Provider storage contract passed\n";return 0;
 }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
