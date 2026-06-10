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
    std::string name, task, backbone, quant, runtime, in_blob, out_blob, param, bin, labels_file;
    int w = 0, h = 0;
    float mean[3] = {0, 0, 0}, norm[3] = {0, 0, 0};
    std::vector<std::string> labels;
    /* nvdla runtime (runtime == "nvdla"): logits dequant + cube sizes */
    float logit_scale = 0;
    int nv_in_size = 0, nv_out_c = 0;
    /* detection (YOLOv2 head); grid==0 means "not a detection pack" */
    int grid = 0;
    std::vector<float> anchors; /* flat (w,h) pairs in grid units */
    float conf_threshold = 0.5f, nms_threshold = 0.45f;
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
static std::vector<float> mf_floatlist(const std::string& s, const char* key){
    std::vector<float> v;
    const char* p = mf_find(s, key); if(!p) return v;
    p = strchr(p, '['); if(!p) return v;
    const char* end = strchr(p, ']'); if(!end) return v;
    char* q = nullptr;
    p++;
    while(p < end){
        float f = strtof(p, &q);
        if(q == p) break;
        v.push_back(f);
        p = strchr(q, ','); if(!p || p > end) break;
        p++;
    }
    return v;
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
    m.runtime = mf_str(s, "runtime");
    if(m.runtime.empty()) m.runtime = "ncnn";
    m.w = (int)mf_num(s, "width"); m.h = (int)mf_num(s, "height");
    mf_floats3(s, "mean", m.mean); mf_floats3(s, "norm", m.norm);
    m.in_blob = mf_str(s, "in_blob");
    m.out_blob = mf_str(s, "out_blob");
    m.param = mf_str(s, "param"); m.bin = mf_str(s, "bin");
    m.labels_file = mf_str(s, "labels_file");
    m.labels = mf_strlist(s, "labels");
    if(m.runtime == "nvdla"){
        m.logit_scale = (float)mf_num(s, "logit_scale");
        m.nv_in_size = (int)mf_num(s, "nv_in_size");
        m.nv_out_c = (int)mf_num(s, "nv_out_c");
        if(m.logit_scale <= 0 || m.nv_in_size <= 0 || m.nv_out_c <= 0) return false;
    }
    if(m.task == "detection"){
        m.grid = (int)mf_num(s, "grid");
        m.anchors = mf_floatlist(s, "anchors");
        double ct = mf_num(s, "conf_threshold"), nt = mf_num(s, "nms_threshold");
        if(ct > 0) m.conf_threshold = (float)ct;
        if(nt > 0) m.nms_threshold = (float)nt;
        if(m.grid <= 0 || m.anchors.size() < 2 || m.anchors.size() % 2) return false;
    }
    return m.w > 0 && m.h > 0 && !m.param.empty() && !m.in_blob.empty();
}
