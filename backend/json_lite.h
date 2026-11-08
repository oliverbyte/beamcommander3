#pragma once
// Minimal JSON field readers for laser_daemon's own small request bodies and
// data files (lasers.json, cues.json, ...). Not a general JSON parser: each
// helper finds the first "key" in the string and reads the value after it,
// which is all our own flat formats need. Kept in a header (instead of
// inside laser_daemon.cpp) so the unit tests in backend/tests/ can use the
// exact same code without linking libera/httplib.

#include <sstream>
#include <string>
#include <vector>

inline std::string json_str(const std::string& b, const std::string& k) {
    auto p=b.find("\""+k+"\""); if(p==std::string::npos) return "";
    p=b.find(':',p); if(p==std::string::npos) return "";
    p=b.find('"',p+1); if(p==std::string::npos) return "";
    auto e=b.find('"',p+1); if(e==std::string::npos) return "";
    return b.substr(p+1,e-p-1);
}
inline float json_float(const std::string& b,const std::string& k,float d) {
    auto p=b.find("\""+k+"\""); if(p==std::string::npos) return d;
    p=b.find(':',p); if(p==std::string::npos) return d;
    ++p; while(p<b.size()&&b[p]==' ')++p;
    try{return std::stof(b.substr(p));}catch(...){return d;}
}
inline bool json_bool(const std::string& b,const std::string& k,bool d) {
    auto p=b.find("\""+k+"\""); if(p==std::string::npos) return d;
    p=b.find(':',p); if(p==std::string::npos) return d;
    ++p; while(p<b.size()&&b[p]==' ')++p;
    if(p<b.size()&&b.substr(p,4)=="true") return true;
    if(p<b.size()&&b.substr(p,5)=="false") return false;
    return d;
}
inline int json_int(const std::string& b,const std::string& k,int d){ return (int)json_float(b,k,(float)d); }
inline bool json_int_array(const std::string& body,const std::string& key,std::vector<int>& values) {
    auto keyPos = body.find("\"" + key + "\"");
    if (keyPos == std::string::npos) return false;
    auto begin = body.find('[', keyPos);
    auto end = begin == std::string::npos ? std::string::npos : body.find(']', begin);
    if (begin == std::string::npos || end == std::string::npos) return false;
    std::stringstream items(body.substr(begin + 1, end - begin - 1));
    values.clear();
    while (true) {
        items >> std::ws;
        if (items.eof()) return true;
        int value;
        if (!(items >> value)) return false;
        values.push_back(value);
        items >> std::ws;
        if (items.eof()) return true;
        char comma;
        if (!(items >> comma) || comma != ',') return false;
    }
}
