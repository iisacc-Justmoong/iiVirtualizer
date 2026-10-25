#include <iiVirtualizer/Runtime.h>
#include <iiVirtualizer/Qemu.h>
#include <iostream>

int main(int argc,char** argv) {
    if(argc<2) {std::cerr<<"Usage: iiVirtualizer_example STATE_DIRECTORY [INSTANCE ...]\n";return 1;}
    ii::virtualizer::Runtime runtime(argv[1],ii::virtualizer::qemu_backend());
    if(argc>2) {
        std::vector<std::string> ids;for(int i=2;i<argc;++i)ids.emplace_back(argv[i]);
        auto results=runtime.apply(ii::virtualizer::Operation::start,std::move(ids));
        if(!results) {std::cerr<<results.error().message<<'\n';return 1;}
        for(const auto& row:*results)if(!row.result)std::cerr<<row.id<<": "<<row.result.error().message<<'\n';
    }
    auto inventory=runtime.snapshot();
    if(!inventory) {std::cerr<<inventory.error().message<<'\n';return 1;}
    for(const auto& row:*inventory) {
        if(!row.result) {std::cerr<<row.id<<": "<<row.result.error().message<<'\n';continue;}
        const auto& instance=*row.result;
        std::cout<<instance.spec.id<<' '<<ii::virtualizer::name(instance.state)<<' '<<instance.spec.memory_mib<<" MiB\n";
    }
}
