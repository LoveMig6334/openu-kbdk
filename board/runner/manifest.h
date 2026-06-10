// manifest.h - just enough JSON for kbdk manifest.json (flat, known unique keys).
// Schema uses in_blob/out_blob/labels_file specifically so every key this parser
// looks for is unique in the whole document (see crates/kbdk-core/src/pack.rs).
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct KbManifest {
    std::string name, task, backbone, quant, in_blob, out_blob, param, bin, labels_file;
    int w = 0, h = 0;
    float mean[3] = {0, 0, 0}, norm[3] = {0, 0, 0};
    std::vector<std::string> labels;
};

// find "key" : <value> ; returns pointer past the colon or nullptr
static const char* mf_find(const std::string& s, const char* key){
    std::string pat = std::string("\"") + key + "\"";
    size_t p = s.find(pat);
    if(p == std::string::npos) return nullptr;
    p = s.find(':', p + pat.size());
    return p == std::string::npos ? nullptr : s.c_str() + p + 1;
}
static std::string mf_str(const std::string& s, const char* key){
    const char* p = mf_find(s, key); if(!p) return "";
    const char* a = strchr(p, '"'); if(!a) return "";
    const char* b = strchr(a + 1, '"'); if(!b) return "";
    return std::string(a + 1, b);
}
static double mf_num(const std::string& s, const char* key){
    const char* p = mf_find(s, key); return p ? atof(p) : 0;
}
static void mf_floats3(const std::string& s, const char* key, float out[3]){
    const char* p = mf_find(s, key); if(!p) return;
    p = strchr(p, '['); if(!p) return;
    char* q = nullptr;
    for(int i = 0; i < 3; i++){
        out[i] = strtof(p + 1, &q);
        p = strchr(q, ',');
        if(!p && i < 2) return;
    }
}
static std::vector<std::string> mf_strlist(const std::string& s, const char* key){
    std::vector<std::string> v;
    const char* p = mf_find(s, key); if(!p) return v;
    p = strchr(p, '['); if(!p) return v;
    const char* end = strchr(p, ']'); if(!end) return v;
    while(true){
        const char* a = strchr(p, '"'); if(!a || a > end) break;
        const char* b = strchr(a + 1, '"'); if(!b || b > end) break;
        v.emplace_back(a + 1, b); p = b + 1;
    }
    return v;
}

static bool mf_load(const char* path, KbManifest& m){
    FILE* f = fopen(path, "rb"); if(!f) return false;
    std::string s; char buf[4096]; size_t n;
    while((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    m.name = mf_str(s, "name"); m.task = mf_str(s, "task");
    m.backbone = mf_str(s, "backbone"); m.quant = mf_str(s, "quant");
    m.w = (int)mf_num(s, "width"); m.h = (int)mf_num(s, "height");
    mf_floats3(s, "mean", m.mean); mf_floats3(s, "norm", m.norm);
    m.in_blob = mf_str(s, "in_blob");
    m.out_blob = mf_str(s, "out_blob");
    m.param = mf_str(s, "param"); m.bin = mf_str(s, "bin");
    m.labels_file = mf_str(s, "labels_file");
    m.labels = mf_strlist(s, "labels");
    return m.w > 0 && m.h > 0 && !m.param.empty() && !m.in_blob.empty();
}
