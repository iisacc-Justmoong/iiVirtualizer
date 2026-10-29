#include "../src/guest/FilesStore.h"
#include <iiFileProviderStorage.h>
#include <atomic>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>
namespace v=ii::virtualizer;
namespace fs=std::filesystem;
static void check(bool value,const char* label){if(!value)throw std::runtime_error(label);}
template<class T>static T take(ii::core::Result<T> result){if(!result)throw std::runtime_error(result.error().message);return std::move(*result);}
static ii::core::Bytes bytes(std::string_view text){auto value=ii::core::byte_view(text);return {value.begin(),value.end()};}
int main(int argc,char** argv) {
    try {
        check(argc==2,"test root required");auto root=fs::absolute(argv[1])/std::to_string(::getpid());
        iiFileProvider::native::createDirectories(root/"Files");
        v::guest::FilesStore store(root/"Files");
        auto run=[&](v::FileAction action,std::string path,std::string destination={},ii::core::Bytes data={}){
            v::FileRequest request;request.action=action;request.path=std::move(path);request.destination=std::move(destination);request.data=std::move(data);return store.execute(request);};
        take(run(v::FileAction::make_directory,"자료"));
        const auto payload=bytes(std::string_view("hello\0binary",12));
        auto created=take(run(v::FileAction::create,"자료/a.bin",{},payload));check(created.entry&&created.entry->size==payload.size(),"create metadata");
        auto duplicate=run(v::FileAction::create,"자료/a.bin",{},bytes("overwrite"));check(!duplicate&&duplicate.error().code==ii::core::ErrorCode::already_exists,"exclusive create");
        check(take(run(v::FileAction::read,"자료/a.bin")).data==payload,"binary read");
        v::FileRequest patch;patch.action=v::FileAction::write;patch.path="자료/a.bin";patch.offset=5;patch.data=bytes("!");
        take(store.execute(patch));auto expected=payload;expected[5]=std::byte{'!'};
        check(take(run(v::FileAction::read,"자료/a.bin")).data==expected,"offset patch");
        patch.offset=0;patch.data=bytes("short");patch.truncate=true;take(store.execute(patch));
        check(take(run(v::FileAction::read,"자료/a.bin")).data==bytes("short"),"explicit truncate");
        patch.offset=100;check(!store.execute(patch),"sparse write rejected");
        take(run(v::FileAction::create,"자료/b.bin",{},bytes("keep")));
        auto collision=run(v::FileAction::move,"자료/a.bin","자료/b.bin");check(!collision&&collision.error().code==ii::core::ErrorCode::already_exists,"move must not overwrite");
        take(run(v::FileAction::move,"자료/a.bin","자료/c.bin"));
        check(!run(v::FileAction::read,"자료/a.bin")&&take(run(v::FileAction::read,"자료/c.bin")).data==bytes("short"),"move persisted");
        for(unsigned i=0;i<25;++i)take(run(v::FileAction::create,"file-"+std::to_string(i)));
        std::vector<std::string> names;v::FileRequest listing;listing.limit=5;
        do {auto page=take(store.execute(listing));check(page.entries.size()<=5,"page bounded");for(const auto& entry:page.entries)names.push_back(entry.name);listing.cursor=page.next_cursor;}while(!listing.cursor.empty());
        check(names.size()==26,"directory pagination completeness");
        for(const auto& path:{"/etc/passwd","../outside","자료/../../outside","자료//c.bin","자료/./c.bin","C:\\Windows","a/", "a\nb"})
            check(!run(v::FileAction::create,path),"path escape rejected");
        iiFileProvider::native::OutputFile(root/"outside")<<"outside";
        fs::create_directory_symlink(root,root/"Files"/"escape");fs::create_symlink(root/"outside",root/"Files"/"leaf");
        check(!run(v::FileAction::read,"escape/outside")&&!run(v::FileAction::write,"leaf",{},bytes("bad")),"symlink traversal rejected");
        check(take(run(v::FileAction::stat,"leaf")).entry->kind==v::FileKind::symlink,"link metadata is no-follow");
        take(run(v::FileAction::remove,"leaf"));check(fs::exists(root/"outside"),"unlink did not follow link");
        check(!run(v::FileAction::remove,"자료"),"nonempty directory deletion rejected");
        take(run(v::FileAction::remove,"자료/b.bin"));take(run(v::FileAction::remove,"자료/c.bin"));take(run(v::FileAction::remove,"자료"));
        check(!run(v::FileAction::remove,""),"root deletion rejected");
        // The parent component repeatedly changes between a directory and an
        // outside symlink; no read may return the outside sentinel.
        iiFileProvider::native::createDirectory(root/"Files/race");
        iiFileProvider::native::OutputFile(root/"Files/race/outside")<<"inside";
        std::atomic<bool> done=false;std::atomic<unsigned> leaked=0;
        std::thread racer([&]{for(unsigned i=0;i<300;++i){std::error_code error;fs::rename(root/"Files/race",root/"Files/held",error);if(error)continue;fs::create_directory_symlink(root,root/"Files/race",error);fs::remove(root/"Files/race",error);fs::rename(root/"Files/held",root/"Files/race",error);}done=true;});
        do{auto value=run(v::FileAction::read,"race/outside");if(value&&value->data==bytes("outside"))++leaked;}while(!done);
        racer.join();check(leaked==0,"directory redirect race escaped Files");
        std::cout<<"Files CRUD, binary chunks, pagination, no-replace move, redirects and race boundary passed\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
