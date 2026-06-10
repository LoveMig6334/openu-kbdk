// kbrun.cpp — kbdk model-pack runner for the V831.
// Stage 1: kbrun PACK_DIR --image RAW.rgb   (raw RGB888 at the manifest's WxH)
//          -> one {"event":"result",...} JSON line with top-5 + latency.
// Stage 2 (camera mode) is added on top of this in the next plan task.
#include "net.h" // ncnn
#include "manifest.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

static double now_ms(){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// flatten a 1x1xC (or wide) ncnn output, softmax in place, return probabilities
static std::vector<float> softmax_flat(const ncnn::Mat& out){
    std::vector<float> v(out.c > 1 ? out.c : out.w);
    if(out.c > 1)
        for(int i = 0; i < out.c; i++) v[i] = out.channel(i)[0];
    else
        for(int i = 0; i < out.w; i++) v[i] = ((const float*)out)[i];
    float mx = *std::max_element(v.begin(), v.end()), sum = 0;
    for(auto& x : v){ x = expf(x - mx); sum += x; }
    for(auto& x : v) x /= sum;
    return v;
}

static void print_result(const std::vector<float>& v, const KbManifest& m, double ms){
    std::vector<int> idx(v.size());
    for(size_t i = 0; i < idx.size(); i++) idx[i] = (int)i;
    size_t k5 = std::min<size_t>(5, idx.size());
    std::partial_sort(idx.begin(), idx.begin() + k5, idx.end(),
                      [&](int a, int b){ return v[a] > v[b]; });
    printf("{\"event\":\"result\",\"ms\":%.1f,\"top\":[", ms);
    for(size_t k = 0; k < k5; k++){
        int i = idx[k];
        printf("%s{\"label\":\"%s\",\"index\":%d,\"conf\":%.4f}", k ? "," : "",
               i < (int)m.labels.size() ? m.labels[i].c_str() : "?", i, v[i]);
    }
    printf("]}\n");
    fflush(stdout);
}

int main(int argc, char** argv){
    if(argc < 4 || std::string(argv[2]) != "--image"){
        fprintf(stderr, "usage: %s PACK_DIR --image RAW.rgb\n", argv[0]);
        return 2;
    }
    std::string dir = argv[1];
    KbManifest m;
    if(!mf_load((dir + "/manifest.json").c_str(), m)){
        printf("{\"event\":\"error\",\"msg\":\"manifest load failed\"}\n"); return 1;
    }

    ncnn::Net net;
    net.opt.num_threads = 1;
    if(net.load_param((dir + "/" + m.param).c_str()) ||
       net.load_model((dir + "/" + m.bin).c_str())){
        printf("{\"event\":\"error\",\"msg\":\"model load failed\"}\n"); return 1;
    }

    size_t need = (size_t)m.w * m.h * 3;
    std::vector<unsigned char> rgb(need);
    FILE* f = fopen(argv[3], "rb");
    if(!f || fread(rgb.data(), 1, need, f) != need){
        printf("{\"event\":\"error\",\"msg\":\"image read failed (want %zu bytes)\"}\n", need);
        return 1;
    }
    fclose(f);

    ncnn::Mat in = ncnn::Mat::from_pixels(rgb.data(), ncnn::Mat::PIXEL_RGB, m.w, m.h);
    in.substract_mean_normalize(m.mean, m.norm);
    ncnn::Mat out;
    double t0 = now_ms();
    {
        ncnn::Extractor ex = net.create_extractor();
        ex.input(m.in_blob.c_str(), in);
        if(ex.extract(m.out_blob.c_str(), out)){
            printf("{\"event\":\"error\",\"msg\":\"extract failed\"}\n"); return 1;
        }
    }
    print_result(softmax_flat(out), m, now_ms() - t0);
    return 0;
}
