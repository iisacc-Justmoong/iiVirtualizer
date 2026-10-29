#include "../src/guest/Workspace.h"
#include <iiFileProviderStorage.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace g=ii::virtualizer::guest;
namespace fs=std::filesystem;
static void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
static void write(const fs::path& path,std::string_view text) {iiFileProvider::native::OutputFile file(path);file<<text;}
int main(int argc,char** argv) {
    try {
        check(argc==2,"state root required");
        const auto root=fs::absolute(argv[1])/std::to_string(::getpid());
        iiFileProvider::native::createDirectories(root);
        write(root/"sibling","keep");write(root/"boot-count","7\n");
        auto workspace=g::prepare_workspace(root);check(bool(workspace),"canonical Society package was not prepared");
        check(workspace->container==root/"Society.societycontainer","workspace escaped the canonical package");
        check(workspace->files==workspace->container/"Files"&&workspace->state.parent_path()==workspace->container,"workspace layout mismatch");
        auto first=g::advance_boot(*workspace);check(first&&*first==8,"legacy boot count was not preserved");
        auto again=g::prepare_workspace(root);check(again&&again->container==workspace->container,"package reopen failed");
        auto second=g::advance_boot(*again);check(second&&*second==9,"boot count did not persist inside the package");
        std::string value;std::ifstream(root/"boot-count")>>value;check(value=="7","legacy sibling file was modified");
        std::ifstream(root/"sibling")>>value;check(value=="keep","sibling user data was modified");
        auto redirected=root/"redirected";iiFileProvider::native::createDirectories(redirected);
        fs::create_directory_symlink(root/"Society.societycontainer",redirected/"Society.societycontainer");
        check(!g::prepare_workspace(redirected),"redirected package accepted");
        auto bad=root/"bad";iiFileProvider::native::createDirectories(bad/"Society.societycontainer");
        fs::create_directory_symlink(root,bad/"Society.societycontainer"/"Files");
        check(!g::prepare_workspace(bad),"redirected Files root accepted");
        check(!g::prepare_workspace(root/".."/root.filename()),"noncanonical parent accepted");
        check(!g::prepare_workspace(workspace->files),"nested container accepted");
        auto outside=root/"outside";write(outside,"external");
        fs::create_hard_link(outside,workspace->files/"outside-link");
        check(!g::prepare_workspace(root),"hard link to a sibling file accepted");
        fs::remove(workspace->files/"outside-link");
        write(workspace->files/"one","internal");fs::create_hard_link(workspace->files/"one",workspace->files/"two");
        check(bool(g::prepare_workspace(root)),"internal hard links rejected");
        write(workspace->container/"metadata","outside Files");fs::create_hard_link(workspace->container/"metadata",workspace->files/"metadata-alias");
        check(!g::prepare_workspace(root),"hard link across the Files boundary accepted");fs::remove(workspace->files/"metadata-alias");
        const auto count=workspace->state/"boot-count";fs::remove(count);fs::create_symlink(outside,count);
        check(!g::advance_boot(*workspace),"redirected trusted boot metadata accepted");
        std::cout<<"Society package, metadata, redirects, hard-link boundaries and sibling preservation passed\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
