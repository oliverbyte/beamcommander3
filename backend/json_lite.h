#pragma once
// Minimal JSON field readers for laser_daemon's own small request bodies and
// data files (lasers.json, cues.json, ...). Not a general JSON parser: each
// helper finds the first "key" that is followed by a colon (so a string
// *value* that happens to equal a key name - e.g. "shape":"text" - is never
// mistaken for the key "text") and reads the value after it, which is all
// our own flat formats need. Kept in a header (instead of
// inside laser_daemon.cpp) so the unit tests in backend/tests/ can use the
// exact same code without linking libera/httplib.

#include <sstream>
#include <string>
#include <vector>

// Position of the first "k" used as a key (followed by ':'), or npos.
inline std::size_t json_key_pos(const std::string& b, const std::string& k) {
    const std::string q = "\"" + k + "\"";
    for (auto p = b.find(q); p != std::string::npos; p = b.find(q, p + 1)) {
        auto e = p + q.size();
        while (e < b.size() && (b[e] == ' ' || b[e] == '\t' || b[e] == '\n' || b[e] == '\r')) ++e;
        if (e < b.size() && b[e] == ':') return p;
    }
    return std::string::npos;
}
inline bool json_has(const std::string& b, const std::string& k) { return json_key_pos(b, k) != std::string::npos; }

inline std::string json_str(const std::string& b, const std::string& k) {
    auto p=json_key_pos(b,k); if(p==std::string::npos) return "";
    p=b.find(':',p); if(p==std::string::npos) return "";
    p=b.find('"',p+1); if(p==std::string::npos) return "";
    auto e=b.find('"',p+1); if(e==std::string::npos) return "";
    return b.substr(p+1,e-p-1);
}
// For free text (e.g. laser text): a JSON string value with escapes. Writes
// the unescaped value (UTF-8) and returns true if `k` has a string value.
// Escaped quotes in a value can never look like a key to the readers above:
// a quote inside a value is always preceded by a backslash.
inline bool json_string(const std::string& b, const std::string& k, std::string& out) {
    auto p = json_key_pos(b, k); if (p == std::string::npos) return false;
    p = b.find(':', p); if (p == std::string::npos) return false;
    ++p; while (p < b.size() && b[p] == ' ') ++p;
    if (p >= b.size() || b[p] != '"') return false;
    std::string v;
    for (++p; p < b.size(); ++p) {
        char c = b[p];
        if (c == '"') { out = v; return true; }
        if (c != '\\') { v.push_back(c); continue; }
        if (++p >= b.size()) return false;
        switch (b[p]) {
            case 'n': v.push_back('\n'); break;
            case 't': v.push_back('\t'); break;
            case 'r': v.push_back('\r'); break;
            case 'b': case 'f': break;
            case 'u': {
                if (p + 4 >= b.size()) return false;
                unsigned cp = 0;
                try { cp = (unsigned)std::stoul(b.substr(p + 1, 4), nullptr, 16); } catch (...) { return false; }
                p += 4;
                if (cp < 0x80) v.push_back((char)cp);
                else if (cp < 0x800) { v.push_back((char)(0xC0 | (cp >> 6))); v.push_back((char)(0x80 | (cp & 0x3F))); }
                else { v.push_back((char)(0xE0 | (cp >> 12))); v.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); v.push_back((char)(0x80 | (cp & 0x3F))); }
                break;
            }
            default: v.push_back(b[p]); break; // \" \\ \/
        }
    }
    return false;
}
// A string as a JSON string literal (with quotes); control characters dropped.
inline std::string json_escape(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
        else if ((unsigned char)c >= 0x20) out.push_back(c);
    }
    return out + "\"";
}
inline float json_float(const std::string& b,const std::string& k,float d) {
    auto p=json_key_pos(b,k); if(p==std::string::npos) return d;
    p=b.find(':',p); if(p==std::string::npos) return d;
    ++p; while(p<b.size()&&b[p]==' ')++p;
    try{return std::stof(b.substr(p));}catch(...){return d;}
}
inline bool json_bool(const std::string& b,const std::string& k,bool d) {
    auto p=json_key_pos(b,k); if(p==std::string::npos) return d;
    p=b.find(':',p); if(p==std::string::npos) return d;
    ++p; while(p<b.size()&&b[p]==' ')++p;
    if(p<b.size()&&b.substr(p,4)=="true") return true;
    if(p<b.size()&&b.substr(p,5)=="false") return false;
    return d;
}
inline int json_int(const std::string& b,const std::string& k,int d){ return (int)json_float(b,k,(float)d); }
inline bool json_int_array(const std::string& body,const std::string& key,std::vector<int>& values) {
    auto keyPos = json_key_pos(body, key);
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
