/*
 * nna_runner.cpp — generic NVDLA (V831 NPU) job executor.
 *
 * Copyright (C) 2026 KidBright uAI toolkit contributors
 * Built on v831-npu, Copyright (C) 2020-2021 Jasbir Matharu, <jasknuj@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * GPLv3 NOTE: this binary (and everything under board/nvdla/) is GPLv3 because
 * it links third_party/v831-npu. It is deliberately a SEPARATE executable from
 * the MIT kbrun; they talk through the same pack/manifest + JSON-lines protocol.
 *
 * Usage: nna_runner JOB.nvj IN.bin OUT.bin [repeat|serve]
 *        (IN.bin = packed int8 feature cube loaded at the job's in_offset
 *         each iteration; "-" if the input already ships in the job blobs)
 *
 * "serve" keeps the NPU set up and loops on stdin: each "go" line re-reads
 * IN.bin, runs the net, writes OUT.bin and prints {"event":"infer","ms":..}.
 * EOF or "q" exits. This is how the MIT kbrun drives the NPU: a separate GPL
 * process, talking only through pipes + tmpfs files (license boundary).
 *
 * Executes an "NVJ1" job file (emitted by py kbdk_convert.nvdla): one ION
 * allocation, blobs preloaded at fixed offsets, then per layer a fused
 * CONV -> SDP(bias/scale/relu) [-> PDP(maxpool)] pass on the NPU. The final
 * output region is written to OUT.bin. JSON-lines status on stdout.
 *
 * Job layout (little-endian; must match kbdk_convert/nvdla.py _LAYER_FMT):
 *   "NVJ1" u32 n_layers, ion_size, out_offset, out_size, in_offset, in_size
 *   n_layers x 64-byte layer records (struct nvj_layer)
 *   u32 n_blobs, then per blob: u32 offset, u32 size, size bytes
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>

#include "nna_hw.h"
#include "nna_interface.h"
#include "nna_config.h"
#include "ion_alloc.h"

struct __attribute__((packed)) nvj_layer {
    uint16_t in_w, in_h, in_c, out_c, kw, kh, stride, pad;
    uint16_t conv_out_w, conv_out_h;
    uint8_t  has_pdp, relu, bias_lshift, out_truncate;
    int32_t  out_scale;
    uint8_t  pool_w, pool_h, pool_stride, pool_pad;
    uint16_t pool_out_w, pool_out_h;
    uint32_t src_off, wt_off, bias_off, dst_off;
    uint8_t  reserved[12];
};

static void *gp_vaddr, *gp_paddr;

/* board musl 1.1.16: avoid any time64-redirected libc call */
static double now_ms(void) {
    FILE *f = fopen("/proc/uptime", "r");
    double t = 0;
    if (f) { if (fscanf(f, "%lf", &t) != 1) t = 0; fclose(f); }
    return t * 1000.0;
}

static void on_alarm(int s) {
    (void)s;
    const char m[] = "{\"event\":\"error\",\"msg\":\"NPU wait_done timed out\"}\n";
    write(1, m, sizeof m - 1);
    _exit(4);
}

/* generalized from the cifar10 example's set_conv (separate W/H, uneven pad) */
static int setup_conv(const nvj_layer *L, nna_conv_op_desc *op, nna_conv_surface_desc *sf) {
    memset(op, 0, sizeof *op);
    memset(sf, 0, sizeof *sf);

    sf->src_data.width = L->in_w;
    sf->src_data.height = L->in_h;
    sf->src_data.channel = L->in_c;
    sf->src_data.address = (uint32_t)(uintptr_t)gp_paddr + L->src_off;
    sf->src_data.line_stride = NNA_ATOMIC_K_SIZE * L->in_w;
    sf->src_data.surf_stride = NNA_ATOMIC_K_SIZE * L->in_w * L->in_h;

    sf->weight_data.width = L->kw;
    sf->weight_data.height = L->kh;
    sf->weight_data.channel = L->in_c;
    sf->weight_data.address = (uint32_t)(uintptr_t)gp_paddr + L->wt_off;

    op->data_format = FORMAT_FEATURE;
    op->input_width_csc = L->in_w;
    op->input_height_csc = L->in_h;
    op->input_channel_csc = L->in_c;
    op->stride_x = L->stride;
    op->stride_y = L->stride;
    op->dilation_x = 1;
    op->dilation_y = 1;

    op->pad_x_left = L->pad;
    op->pad_y_top = L->pad;
    /* uneven right/bottom pad for stride>1 'same'-style convs */
    int pr = (L->conv_out_w - 1) * L->stride + L->kw - L->in_w - L->pad;
    int pb = (L->conv_out_h - 1) * L->stride + L->kh - L->in_h - L->pad;
    op->pad_x_right = pr > 0 ? pr : 0;
    op->pad_y_bottom = pb > 0 ? pb : 0;

    op->kernel_width_csc = L->kw;
    op->kernel_height_csc = L->kh;
    op->kernel_channel_csc = L->in_c;

    sf->dst_data.width = L->conv_out_w;
    sf->dst_data.height = L->conv_out_h;
    sf->dst_data.channel = L->out_c;
    op->input_width_cmac = L->conv_out_w;
    op->input_height_cmac = L->conv_out_h;

    op->entry_per_slice = calculate_eps(op, sf);
    op->bytes_per_kernel = (uint32_t)L->in_c * L->kw * L->kh;
    sf->weight_data.size = (uint32_t)L->out_c * op->bytes_per_kernel + 31;

    op->data_bank = calculate_data_bank(op, sf);
    op->weight_bank = calculate_weight_bank(sf);
    op->release = 0;

    if (op->data_bank + op->weight_bank > NNA_CBUF_BANK_NUMBER) {
        printf("{\"event\":\"error\",\"msg\":\"layer exceeds CBUF: data_bank=%d weight_bank=%d\"}\n",
               op->data_bank, op->weight_bank);
        return -1;
    }
    return 0;
}

static void setup_sdp(const nvj_layer *L, nna_sdp_op_desc *op, nna_sdp_surface_desc *sf) {
    memset(op, 0, sizeof *op);
    memset(sf, 0, sizeof *sf);

    sf->src_data.address = 0; /* fused: input straight from conv core */
    sf->src_data.width = L->conv_out_w;
    sf->src_data.height = L->conv_out_h;
    sf->src_data.channel = L->out_c;
    sf->src_data.line_stride = NNA_ATOMIC_K_SIZE * L->conv_out_w;
    sf->src_data.surf_stride = NNA_ATOMIC_K_SIZE * L->conv_out_w * L->conv_out_h;

    /* dst: memory, or 0 when PDP consumes the SDP stream */
    sf->dst_data.address = L->has_pdp ? 0 : (uint32_t)(uintptr_t)gp_paddr + L->dst_off;
    sf->dst_data.width = L->conv_out_w;
    sf->dst_data.height = L->conv_out_h;
    sf->dst_data.channel = L->out_c;
    sf->dst_data.line_stride = NNA_ATOMIC_K_SIZE * L->conv_out_w;
    sf->dst_data.surf_stride = NNA_ATOMIC_K_SIZE * L->conv_out_w * L->conv_out_h;

    op->out_cvt.scale = (int16_t)L->out_scale;
    op->out_cvt.truncate = L->out_truncate;

    /* x1: per-kernel int16 bias add (+ optional ReLU) */
    op->x1_op.enable = 1;
    op->x1_op.type = SDP_OP_ADD;
    op->x1_op.alu_type = SDP_ALU_OP_SUM;
    op->x1_op.shift_value = L->bias_lshift;
    op->x1_op.truncate = 0;
    op->x1_op.mode = SDP_OP_PER_KERNEL;
    op->x1_op.act = L->relu ? ACTIVATION_RELU : ACTIVATION_NONE;
    sf->x1_data.address = (uint32_t)(uintptr_t)gp_paddr + L->bias_off;
    sf->x1_data.line_stride = sf->src_data.line_stride * 2;  /* int16 operands */
    sf->x1_data.surf_stride = sf->src_data.surf_stride * 2;

    op->x2_op.enable = 0;
    op->y_op.enable = 0;
}

static void setup_pdp(const nvj_layer *L, nna_pdp_op_desc *op, nna_pdp_surface_desc *sf) {
    memset(op, 0, sizeof *op);
    memset(sf, 0, sizeof *sf);

    op->split_num = 1;
    op->pool_mode = POOL_MODE_MAX;
    op->pool_width = L->pool_w;
    op->pool_height = L->pool_h;
    op->stride_x = L->pool_stride;
    op->stride_y = L->pool_stride;
    op->pad_left = L->pool_pad;
    op->pad_top = L->pool_pad;
    /* floor-mode pooling can leave a remainder column/row -> negative "pad";
     * clamp to 0 (u8 fields would wrap and PDP reads garbage) */
    int pr = (L->pool_out_w - 1) * L->pool_stride + L->pool_w - (L->conv_out_w + L->pool_pad);
    int pb = (L->pool_out_h - 1) * L->pool_stride + L->pool_h - (L->conv_out_h + L->pool_pad);
    op->pad_right = pr > 0 ? pr : 0;
    op->pad_bottom = pb > 0 ? pb : 0;

    sf->src_data.address = 0; /* fused: from SDP */
    sf->src_data.width = L->conv_out_w;
    sf->src_data.height = L->conv_out_h;
    sf->src_data.channel = L->out_c;
    sf->src_data.line_stride = NNA_ATOMIC_K_SIZE * L->conv_out_w;
    sf->src_data.surf_stride = NNA_ATOMIC_K_SIZE * L->conv_out_w * L->conv_out_h;

    sf->dst_data.address = (uint32_t)(uintptr_t)gp_paddr + L->dst_off;
    sf->dst_data.width = L->pool_out_w;
    sf->dst_data.height = L->pool_out_h;
    sf->dst_data.channel = L->out_c;
    sf->dst_data.line_stride = NNA_ATOMIC_K_SIZE * L->pool_out_w;
    sf->dst_data.surf_stride = NNA_ATOMIC_K_SIZE * L->pool_out_w * L->pool_out_h;
}

static int run_layer(const nvj_layer *L) {
    nna_conv_op_desc conv_op;
    nna_conv_surface_desc conv_sf;
    nna_sdp_op_desc sdp_op;
    nna_sdp_surface_desc sdp_sf;
    nna_pdp_op_desc pdp_op;
    nna_pdp_surface_desc pdp_sf;

    if (setup_conv(L, &conv_op, &conv_sf)) return -1;
    setup_sdp(L, &sdp_op, &sdp_sf);

    nna_conv_set_producer(0, 0);
    nna_sdp_set_producer(0, 0);
    nna_conv_program(&conv_op, &conv_sf);
    nna_sdp_program(&sdp_op, &sdp_sf);

    if (L->has_pdp) {
        setup_pdp(L, &pdp_op, &pdp_sf);
        nna_pdp_set_producer(0, 0);
        nna_pdp_program(&pdp_op, &pdp_sf);
    }

    nna_conv_enable(0, 0);
    nna_sdp_enable(0, 1);
    if (L->has_pdp) {
        nna_pdp_enable(0, 1);
        nna_wait_done(0x150011, 0x150011);
    } else {
        nna_wait_done(0x150001, 0x150001);
    }
    nna_reset();
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 4) {
        fprintf(stderr, "usage: %s JOB.nvj IN.bin|- OUT.bin [repeat]\n", argv[0]);
        return 2;
    }
    const char *in_path = strcmp(argv[2], "-") ? argv[2] : NULL;
    const char *out_path = argv[3];
    int serve = argc > 4 && !strcmp(argv[4], "serve");
    int repeat = (!serve && argc > 4) ? atoi(argv[4]) : 1;
    if (repeat < 1) repeat = 1;
    if (serve && !in_path) {
        printf("{\"event\":\"error\",\"msg\":\"serve mode needs an input file\"}\n");
        return 2;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("{\"event\":\"error\",\"msg\":\"open job failed\"}\n"); return 1; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *job = (uint8_t *)malloc(fsz);
    if (fread(job, 1, fsz, f) != (size_t)fsz) { printf("{\"event\":\"error\",\"msg\":\"read job failed\"}\n"); return 1; }
    fclose(f);

    if (fsz < 28 || memcmp(job, "NVJ1", 4)) {
        printf("{\"event\":\"error\",\"msg\":\"bad job magic\"}\n");
        return 1;
    }
    uint32_t n_layers, ion_size, out_off, out_size, in_off, in_size;
    memcpy(&n_layers, job + 4, 4);
    memcpy(&ion_size, job + 8, 4);
    memcpy(&out_off, job + 12, 4);
    memcpy(&out_size, job + 16, 4);
    memcpy(&in_off, job + 20, 4);
    memcpy(&in_size, job + 24, 4);
    const nvj_layer *layers = (const nvj_layer *)(job + 28);
    const uint8_t *p = job + 28 + (size_t)n_layers * sizeof(nvj_layer);

    uint8_t *in_data = NULL;
    if (in_path) {
        if (!in_size) { printf("{\"event\":\"error\",\"msg\":\"job has no input region\"}\n"); return 1; }
        in_data = (uint8_t *)malloc(in_size);
        /* serve mode reads the file fresh per "go"; one-shot reads it now */
        if (!serve) {
            FILE *fi = fopen(in_path, "rb");
            if (!fi || fread(in_data, 1, in_size, fi) != in_size) {
                printf("{\"event\":\"error\",\"msg\":\"read input failed (want %u bytes)\"}\n", in_size);
                return 1;
            }
            fclose(fi);
        }
    }

    signal(SIGALRM, on_alarm);

    /* The NPU is single-tenant: a second nna_runner interleaving CONV/SDP
     * programming and ION buffers corrupts results SILENTLY (learned the hard
     * way — a live kbrun serve child turned one-shot verify runs into ghost
     * outputs). Refuse to start if another instance is alive. */
    {
        FILE *pf = popen("pidof nna_runner", "r");
        if (pf) {
            char buf[128] = {0};
            if (fgets(buf, sizeof buf, pf)) {
                int others = 0;
                for (char *tok = strtok(buf, " \n"); tok; tok = strtok(NULL, " \n"))
                    if (atoi(tok) != getpid()) others++;
                if (others) {
                    pclose(pf);
                    printf("{\"event\":\"error\",\"msg\":\"another nna_runner is using the NPU (stop kbrun first)\"}\n");
                    return 5;
                }
            }
            pclose(pf);
        }
    }

    nna_configure(nna_cmd_clk, 400);
    nna_on();
    void *r = xreg_open();
    if (!r) { printf("{\"event\":\"error\",\"msg\":\"xreg_open failed (need root)\"}\n"); return 1; }
    nna_reset();

    sunxi_ion_alloc_open();
    sunxi_ion_alloc_palloc(ion_size, &gp_vaddr, &gp_paddr);
    if (!gp_paddr) { printf("{\"event\":\"error\",\"msg\":\"ion alloc failed\"}\n"); return 1; }

    uint32_t n_blobs;
    memcpy(&n_blobs, p, 4); p += 4;
    for (uint32_t b = 0; b < n_blobs; b++) {
        uint32_t off, sz;
        memcpy(&off, p, 4); memcpy(&sz, p + 4, 4); p += 8;
        sunxi_ion_loadin((char *)p, sz, (uint32_t)(uintptr_t)gp_paddr + off);
        p += sz;
    }

    printf("{\"event\":\"loaded\",\"layers\":%u,\"ion\":%u}\n", n_layers, ion_size);

    if (serve) {
        char line[64];
        uint8_t *out = (uint8_t *)malloc(out_size);
        while (fgets(line, sizeof line, stdin)) {
            if (line[0] == 'q') break;
            if (line[0] != 'g') continue;
            double t0 = now_ms();
            FILE *fi = fopen(in_path, "rb");
            if (!fi || fread(in_data, 1, in_size, fi) != in_size) {
                if (fi) fclose(fi);
                printf("{\"event\":\"error\",\"msg\":\"input read failed\"}\n");
                continue;
            }
            fclose(fi);
            sunxi_ion_loadin((char *)in_data, in_size, (uint32_t)(uintptr_t)gp_paddr + in_off);
            int err = 0;
            for (uint32_t i = 0; i < n_layers && !err; i++) {
                alarm(10);
                err = run_layer(&layers[i]);
                alarm(0);
            }
            if (err) { printf("{\"event\":\"error\",\"msg\":\"layer failed\"}\n"); continue; }
            sunxi_ion_loadout((uint32_t)(uintptr_t)gp_paddr + out_off, out_size, (char *)out);
            FILE *fo = fopen(out_path, "wb");
            if (fo) { fwrite(out, 1, out_size, fo); fclose(fo); }
            printf("{\"event\":\"infer\",\"ms\":%.1f}\n", now_ms() - t0);
        }
        free(out);
        sunxi_ion_alloc_free();
        sunxi_ion_alloc_close();
        xreg_close();
        nna_off();
        return 0;
    }

    double t_all0 = now_ms();
    double lms[64];
    int rc = 0;
    for (int it = 0; it < repeat && rc == 0; it++) {
        /* per-iteration input load: realistic per-frame cost in serve use */
        if (in_data)
            sunxi_ion_loadin((char *)in_data, in_size, (uint32_t)(uintptr_t)gp_paddr + in_off);
        for (uint32_t i = 0; i < n_layers; i++) {
            double t0 = now_ms();
            alarm(10);
            if (run_layer(&layers[i])) { rc = 3; break; }
            alarm(0);
            if (i < 64) lms[i] = now_ms() - t0;
        }
    }
    double total = (now_ms() - t_all0) / repeat;

    if (rc == 0) {
        uint8_t *out = (uint8_t *)malloc(out_size);
        sunxi_ion_loadout((uint32_t)(uintptr_t)gp_paddr + out_off, out_size, (char *)out);
        FILE *o = fopen(out_path, "wb");
        if (o) { fwrite(out, 1, out_size, o); fclose(o); }
        else { printf("{\"event\":\"error\",\"msg\":\"write out failed\"}\n"); rc = 1; }
        free(out);

        printf("{\"event\":\"done\",\"ms\":%.1f,\"repeat\":%d,\"layer_ms\":[", total, repeat);
        for (uint32_t i = 0; i < n_layers && i < 64; i++)
            printf("%s%.1f", i ? "," : "", lms[i]);
        printf("]}\n");
    }

    sunxi_ion_alloc_free();
    sunxi_ion_alloc_close();
    xreg_close();
    nna_off();
    free(job);
    return rc;
}
