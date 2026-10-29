#include "BinaryWire.h"
#include <stdexcept>
namespace ii::virtualizer::wire_detail {
std::string encode_binary(core::ByteView data) {
    constexpr char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;result.reserve((data.size()+2)/3*4);
    for(std::size_t i=0;i<data.size();i+=3){unsigned n=std::to_integer<unsigned>(data[i])<<16;
        if(i+1<data.size())n|=std::to_integer<unsigned>(data[i+1])<<8;
        if(i+2<data.size())n|=std::to_integer<unsigned>(data[i+2]);
        result+=alphabet[n>>18];result+=alphabet[(n>>12)&63];result+=i+1<data.size()?alphabet[(n>>6)&63]:'=';result+=i+2<data.size()?alphabet[n&63]:'=';}
    return result;
}
core::Bytes decode_binary(std::string_view value,std::size_t limit) {
    if(value.size()%4||value.size()>(limit+2)/3*4)throw std::invalid_argument("Invalid binary base64 length");
    auto number=[](char c)->int{if(c>='A'&&c<='Z')return c-'A';if(c>='a'&&c<='z')return c-'a'+26;if(c>='0'&&c<='9')return c-'0'+52;if(c=='+')return 62;if(c=='/')return 63;return -1;};
    core::Bytes output;output.reserve(value.size()/4*3);
    for(std::size_t i=0;i<value.size();i+=4) {
        const int a=number(value[i]),b=number(value[i+1]),c=number(value[i+2]),d=number(value[i+3]);
        const bool pad2=value[i+2]=='=',pad1=value[i+3]=='=';
        if(a<0||b<0||(!pad2&&c<0)||(!pad1&&d<0)||(pad2&&!pad1)||((pad1||pad2)&&i+4!=value.size())||(pad2&&(b&15))||(!pad2&&pad1&&(c&3)))throw std::invalid_argument("Noncanonical binary base64");
        const auto n=(static_cast<unsigned>(a)<<18)|(static_cast<unsigned>(b)<<12)|(pad2?0U:static_cast<unsigned>(c)<<6)|(pad1?0U:static_cast<unsigned>(d));
        output.push_back(static_cast<std::byte>(n>>16));if(!pad2)output.push_back(static_cast<std::byte>((n>>8)&255));if(!pad1)output.push_back(static_cast<std::byte>(n&255));
    }
    if(output.size()>limit)throw std::invalid_argument("Binary data exceeds its chunk limit");
    return output;
}
}
