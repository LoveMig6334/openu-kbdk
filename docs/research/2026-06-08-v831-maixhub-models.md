# V831 / MaixHub downloadable models — deep-research report

_Produced by the `deep-research` workflow: 5 search angles, 18 sources fetched, 76 claims extracted, 19 confirmed / 6 killed by 3-vote adversarial verification. Date: 2026-06-08._

> Scope note: answers "what pretrained models can I download/run on the KidBright µAI (Allwinner V831, AWNN/libmaix_nn) and how do I bring my own." See `src/nnacam.cpp` / `src/nncls.c` for the on-board wiring this informed.

## Question

Downloadable pretrained AI models for the Sipeed MaixII-Dock / KidBright µAI board (Allwinner V831, NVDLA nv_small NPU, ~0.2 TOPS, runs Sipeed's AWNN runtime = libmaix_nn.so, a quantized-ncnn fork using .param + .bin files with magic 7767517).

I want to find SPECIFIC, CONCRETELY DOWNLOADABLE models so I can wire one into a C program (nncls.c) that dlopens libmaix_nn.so and runs forward inference. For each model found, report: (1) exact download URL or repo/MaixHub link, (2) architecture (MobileNetV1/V2, ResNet, YOLOv2, etc.), (3) input resolution and channels, (4) the file format (.param/.bin AWNN-ncnn? or .mud + .param/.bin? or awnn-specific?), (5) the input/output BLOB names and preprocessing (mean/norm) if documented, (6) number of classes / dataset (ImageNet, VOC, COCO), (7) license, (8) whether it's compiled for the V831 NPU specifically vs CPU.

Focus areas to investigate:
- The Sipeed MaixHub model zoo (maixhub.com) — what classification + detection models are published for the V831 / MaixII / "v831" target, and how to download them.
- Sipeed's libmaix repo (github.com/sipeed/libmaix) — its example models (the fe_res18 / resnet / mobilenet / yolo demos), the models/ directory, and any download scripts.
- The MaixPy3 / maixpy v831 stack — what model files it ships (e.g. the classifier, face detection, object detection .param/.bin), and where they live on the rootfs or are downloaded from.
- The Sipeed "v831 model conversion" / awnn toolchain (the ncnn->awnn quantization tool, "ncnn2awnn" or similar, and "v831 nbg" / NPU compile flow) — can a user convert their own MobileNetV2/YOLO to this format, and is the converter public?
- Any MobileNetV2 ImageNet classifier and YOLOv2/YOLObbox VOC-20 detector specifically known to run on the V831 NPU.
- Whether models from MaixHub are NPU-compiled (.nbg / awnn-quantized for nv_small) or generic ncnn that AWNN quantizes at load.

Also clarify the practical question: to run a newer/larger model on the V831 NPU, must models be pre-compiled by Sipeed's (possibly closed) NPU toolchain, or can an arbitrary trained model be brought to this board, and via what concrete steps/tools.

## Executive summary

For the KidBright µAI / Sipeed MaixII (V831 NVDLA nv_small NPU running AWNN), there are concretely downloadable AWNN .param/.bin models reachable today through three channels: (1) the MaixPy3 factory rootfs ships a ready-to-use YOLOv2 face detector at /home/model/face/yolo2_face_awnn.param + .bin (no download needed); (2) Sipeed's libmaix examples expect models downloaded from MaixHub by modelId (e.g. nn_resnet -> modelId=25, a 1000-class ResNet ImageNet classifier placed in /root/models/) and cover classification (resnet, mobilenet, shufflenet) plus detection (YOLOv2 VOC-20, person, traffic, number) demos that do NOT bundle weights; (3) you can convert your own model via the public PyTorch -> ONNX -> NCNN (onnx2ncnn) pipeline, but the final NCNN->AWNN int8 NPU-compile step is closed-source and runs ONLY through MaixHub's online tool (Toolbox -> Model Conversion -> V831), because Allwinner keeps AWNN proprietary. All AWNN-quantized models keep the .param/.bin extensions (magic 7767517 ncnn-derived) and are NPU-targeted; the AWNN backbone runs on the NPU while decoders/softmax/NMS run on the CPU. The most directly wireable models for an nncls.c that dlopens libmaix_nn.so are the bundled yolo2_face_awnn (blobs input0/output0, 224x224x3, mean=127.5 norm=0.0078125) and the MaixHub resnet18 1000-class classifier (224x224x3, blobs input0/output0).

## Findings

### A ready-to-run YOLOv2 face detector is pre-installed on the MaixPy3 V831 factory rootfs at /home/model/face/yolo2_face_awnn.param and yolo2_face_awnn.bin (AWNN .param/.bin), no download needed.

- **Confidence:** high (votes 3-0)
- **Sources:** https://github.com/sipeed/sipeed_wiki/blob/main/news/MaixPy3/v831_usage/v831_usage.md
- **Evidence:** Sipeed wiki C++ example hardcodes model_path_param="/home/model/face/yolo2_face_awnn.param" and .bin, and states the factory image already includes the face-detection model so no user download is needed. Caveat: a reflashed/custom rootfs may lack the file.

### The bundled V831 face model is a single-class (face) YOLOv2: 224x224x3 RGB input, 7x7x30 output, mean=127.5, norm=0.0078125, blobs input0/output0, threshold 0.5, NMS 0.3, anchors {1.19,1.98,2.79,4.59,4.53,8.92,8.06,5.29,10.32,10.65}; decoder/NMS run on CPU.

- **Confidence:** high (votes 3-0)
- **Sources:** https://github.com/sipeed/sipeed_wiki/blob/main/news/MaixPy3/v831_usage/v831_usage.md
- **Evidence:** Primary Sipeed wiki confirms verbatim: input 224x224x3, output 7x7x30 = (1 class+5)*5 anchors, mean=127.5, norm=0.0078125, inputs_names={"input0"}, outputs_names={"output0"}, classes_num=1(face), threshold=0.5, nms=0.3, and the exact anchor array. Independent web search reproduced the same anchors/dims. CPU decode is corroborated by the project's own CLAUDE.md note. This is the most directly wireable model for the dlopen-libmaix_nn workflow.

### libmaix's nn_resnet example is an ImageNet-style 1000-class ResNet classifier; its model is downloaded from MaixHub modelId=25 and placed in /root/models/ on the board (not bundled in the repo).

- **Confidence:** high (votes 3-0 (merged with two 2-1 corroborating claims))
- **Sources:** https://github.com/sipeed/libmaix/tree/release/examples/nn_resnet; https://github.com/sipeed/libmaix; https://maixhub.com/modelInfo?modelId=25
- **Evidence:** README states 'Resnet 1000 classes classifier demo' and instructs downloading the model at maixhub modelId=25 into /root/models/. main.c loads ./resnet.mud with output .c=1000 at 224x224, and main.h carries the canonical ImageNet-1000 label list (tench, goldfish, great white shark...). GitHub directory listing shows no committed .param/.bin, confirming MaixHub-only distribution. Caveat: README says 'face model' (copy-paste artifact); the maixhub.com modelId=25 page is JS/auth-gated and could not be independently rendered, so the linked payload itself is unverified.

### The nn_resnet network input is 224x224x3 INT8 with load-time quantization (need_quantization=true); output is a 1000-element float CHW (1x1x1000) vector that the demo softmaxes on the CPU.

- **Confidence:** high (votes 3-0)
- **Sources:** https://github.com/sipeed/libmaix/tree/release/examples/nn_resnet
- **Evidence:** Primary main.c: input struct {.w=224,.h=224,.c=3,.dtype=INT8,.need_quantization=true}; output {.w=1,.h=1,.c=1000,.layout=CHW,.dtype=FLOAT}; a C softmax(out_fmap.data,1000) runs post-forward. RGB888/HWC labels are AWNN-convention inferences (no .layout field on input).

### Sipeed libmaix ships a broad set of dlopen-libmaix_nn demos: classification (nn_resnet, nn_r329_mobilenet2, nn_r329_shufflenet, self_learn_classifier) and detection (nn_yolo2_card_mdsc, nn_yolo2_person_mdsc, nn_yolo_20class_mdsc, nn_yolo_number, nn_yolo_person, nn_yolo_traffic, nn_retinaface, plus nn_LPR, nn_mask, nn_pose).

- **Confidence:** high (votes 3-0)
- **Sources:** https://github.com/sipeed/libmaix/tree/release/examples
- **Evidence:** Direct fetch of the examples directory confirmed all listed nn_ subdirectories exist. Caveats: examples use libmaix's CMake/link build (not literally a dlopen pattern); nn_r329_* are named for the R329 chip; repo is archived read-only since Jan 2024 but still downloadable.

### The nn_yolo_20class_mdsc example is a YOLOv2 PASCAL VOC-20 detector (224x224x3 RGB UINT8 input, 7x7 output), with the standard VOC class list hardcoded; despite the README's 'find face' label the code targets VOC-20.

- **Confidence:** high (votes 3-0 (merged two claims))
- **Sources:** https://github.com/sipeed/libmaix/tree/release/examples/nn_yolo_20class_mdsc
- **Evidence:** Primary main.c: class_num=20 with labels {aeroplane,bicycle,bird,boat,bottle,bus,car,cat,chair,cow,diningtable,dog,horse,motorbike,person,pottedplant,sheep,sofa,train,tvmonitor}; res_w=res_h=224, net_in_width/height=224, input .c=3 UINT8; uses libmaix_nn_decoder_yolo2_*. README title 'YOLO2 find face demo' is a runtime model-swap mismatch, not a code contradiction.

### libmaix examples load models via a .mud descriptor file (e.g. nn_yolo_20class_mdsc loads /root/mud/v831_yolo_voc.mud through libmaix_mud_load_model()); the .mud is a text descriptor that references the underlying AWNN .param/.bin.

- **Confidence:** medium (votes 3-0 (but a directly contradictory claim about nn_resnet's .mud was refuted 0-3))
- **Sources:** https://github.com/sipeed/libmaix/tree/release/examples/nn_yolo_20class_mdsc
- **Evidence:** main.c line 69 mud_path="/root/mud/v831_yolo_voc.mud" loaded via libmaix_mud_load_model(). The .mud references rather than embeds the .param/.bin. Confidence reduced because parallel claims that nn_resnet uses the same .mud/libmaix_mud_load_model() route were REFUTED 0-3 — so the .mud loading path appears example-specific (the mdsc examples use it; nn_resnet's loader path is disputed). For nncls.c you can still load bare .param/.bin directly with libmaix_nn rather than the .mud wrapper.

### To bring an arbitrary trained model to the V831 NPU, the public pipeline is PyTorch -> ONNX (torch.onnx.export) -> NCNN (Tencent onnx2ncnn, producing .param/.bin) -> AWNN int8 .param/.bin via MaixHub's online conversion tool.

- **Confidence:** high (votes 3-0)
- **Sources:** https://wiki.sipeed.com/ai/en/deploy/v831.html; https://wiki.sipeed.com/news/others/v831_resnet18/v831_resnet18.html
- **Evidence:** Two first-party Sipeed wiki pages document the exact 3-step pipeline and that the final output is AWNN .param/.bin. Caveat: 'arbitrary' overreaches — the same docs note operator limits (view/flatten/reshape unsupported, image inputs only, identical per-channel normalization required) and that custom-model conversions often fail on unsupported operators.

### The NCNN->AWNN int8 NPU-compile/quantization step is closed-source and runs ONLY through MaixHub's online tool (Toolbox -> Model Conversion -> V831); there is no downloadable local AWNN converter, because Allwinner keeps AWNN proprietary.

- **Confidence:** high (votes 3-0 (merged two claims))
- **Sources:** https://wiki.sipeed.com/ai/en/deploy/v831.html; https://wiki.sipeed.com/news/others/v831_resnet18/v831_resnet18.html; https://neucrack.com/p/358
- **Evidence:** Both EN and ZH Sipeed wiki pages plus the Sipeed dev (neucrack) blog state the conversion is done online at maixhub.com/maix.sipeed.com and that '全志要求不开放 awnn' (Allwinner requires AWNN not be open-sourced), so the web tool is currently the only way. ncnn's own ncnn2int8 is unrelated. This is the key practical constraint: you cannot NPU-compile locally.

### The conversion pipeline produces NCNN-format .param/.bin via onnx2ncnn, and the final AWNN-quantized int8 model keeps the same .param/.bin extensions (e.g. resnet_awnn.param/.bin).

- **Confidence:** high (votes 3-0)
- **Sources:** https://wiki.sipeed.com/news/others/v831_resnet18/v831_resnet18.html; https://wiki.sipeed.com/ai/en/deploy/v831.html
- **Evidence:** Sipeed wiki: onnx2ncnn '得到一个 .param 文件和一个 .bin 文件', then awnn quantizes to int8 keeping .param/.bin; the deploy page gives concrete filenames resnet_awnn.param/.bin. Confirms the AWNN format is ncnn-derived (magic 7767517) with the same extensions, NPU-targeted.

### A PyTorch ResNet18 ImageNet classifier (224x224x3, 1000 classes) can be deployed on the V831 NPU via AWNN, with blobs input0/output0 and preprocessing (input-mean)*norm; the ImageNet-correct values are mean=123.675, norm=0.017125 per channel, though the demo code may use the simpler mean=127.5, norm=0.0078125.

- **Confidence:** medium (votes 2-1)
- **Sources:** https://wiki.sipeed.com/news/others/v831_resnet18/v831_resnet18.html
- **Evidence:** Sipeed ResNet18 tutorial shows inputs {input0:(224,224,3)}, outputs {output0:(1,1,1000)}, formula (input-mean)*norm with mean=0.485*255=123.675 and norm=1/(0.229*255)=0.017125. Caveat (the 2-1 split): the same page's working MaixPy3 code block uses the simpler [-1,1] scaling mean=127.5/norm=0.00784, so two preprocessing options coexist — pick per how the model was actually quantized.

### Sipeed's maix_train repo documents only MobileNetV1 (classification) and YOLOv2 (detection) as trainable example architectures — no MobileNetV2 or ResNet path — and its documented local compile toolchain is nncase/ncc (kmodel output), not an AWNN/.param/.bin/.nbg step.

- **Confidence:** medium (votes 2-1 each (two related claims))
- **Sources:** https://github.com/sipeed/maix_train
- **Evidence:** maix_train README documents 'Object classification(Mobilenet V1)' and 'Object detection(YOLO v2)' only; train/ dir has classifier, detector, mobilenet_sipeed (still V1). README's local converter is nncase ncc v0.1-rc5 emitting kmodel (that is the K210 path); no awnn/ncnn/nbg step in master README — for V831 it points users to online MaixHub. Note ResNet18 (fe_res18) lives in the separate libmaix repo, not maix_train.

## Caveats

Time-sensitivity and source quality: (1) The sipeed/libmaix repo has been ARCHIVED read-only since Jan 2024 — examples still download but are unmaintained. (2) maixhub.com/modelInfo?modelId=25 (and other modelId pages) are JS/auth-gated and could NOT be independently rendered by any verifier — the architecture/class-count/placement facts are proven from libmaix source code, but the actual downloadable payload behind each modelId link is unverified; you may need a MaixHub login to fetch them. (3) The .mud loading path is example-inconsistent: the mdsc examples (yolo_20class) verifiably use /root/mud/*.mud via libmaix_mud_load_model(), but claims that nn_resnet uses the same .mud route were refuted 0-3, so do not assume a uniform .mud distribution format — bare .param/.bin loading via libmaix_nn is the safer assumption for nncls.c. (4) Several README labels are stale copy-paste artifacts ('face model' in nn_resnet, 'find face' in nn_yolo_20class) — trust the C source labels/dims over the README prose. (5) Preprocessing mean/norm differs between sources (123.675/0.017125 ImageNet-correct vs 127.5/0.0078125 [-1,1] scaling); the correct set depends on how the specific model was quantized at conversion time, so verify per-model. (6) AWNN requires identical normalization values across all 3 channels and supports a limited operator set — 'arbitrary' model conversion is aspirational, not guaranteed. (7) No open-source local AWNN/NPU compiler exists; this is a hard dependency on Sipeed's online MaixHub service (consistent with the project's own CLAUDE.md note that no open compiler turns an arbitrary model into NVDLA descriptors).

## Refuted / rejected claims

- **0-3:** The model is loaded through a .mud descriptor file (resnet.mud) via libmaix_mud_load_model(), confirming the V831/AWNN distribution format is .mud (which references the underlying .param/.bin), not a bare .param/.bin pair.
  - source: https://github.com/sipeed/libmaix/tree/release/examples/nn_resnet
- **1-2:** The V831 toolchain supports a defined set of architectures (ResNet18, MobileNet, YOLO v2, FOMO) with limited operator support, so arbitrary models are constrained by the supported-operator list.
  - source: https://wiki.sipeed.com/ai/en/deploy/v831.html
- **1-2:** The MaixHub platform explicitly supports the V831 board as a training/deployment target (alongside k210, mobile, computer, MCU), implying V831 models are produced through MaixHub rather than only a local toolchain.
  - source: https://wiki.sipeed.com/soft/maixpy/en/course/ai/train/maixhub.html
- **1-2:** MaixHub provides one-click deployment of trained models directly to the device, indicating the NPU compilation/conversion step is performed online by Sipeed's service rather than by a public local toolchain.
  - source: https://wiki.sipeed.com/soft/maixpy/en/course/ai/train/maixhub.html
- **1-2:** The concrete toolchain to bring a custom trained model to the V831 is PyTorch -> ONNX (torch.onnx.export) -> NCNN (onnx2ncnn from ncnn/tools/onnx) -> AWNN (online quantization), and the resulting model is compiled for the V831 NPU rather than CPU.
  - source: https://wiki.sipeed.com/news/others/v831_resnet18/v831_resnet18.html
- **0-3:** The libmaix runtime loads models from a .mud file via libmaix_mud_load_model() (a .mud descriptor that wraps the underlying ncnn-style .param/.bin), not by directly opening .param/.bin in user code.
  - source: https://github.com/sipeed/libmaix

## Open questions

- What are the exact downloadable file names, sizes, and AWNN blob names for MaixHub modelId=25 (resnet) and modelId=26 (yolo face) — these pages are auth/JS-gated and could not be rendered; does downloading require a MaixHub account?
- Are there published MobileNetV2 ImageNet or YOLOv2 VOC-20 AWNN .param/.bin files already compiled for the V831 NPU on MaixHub (vs only the resnet18 1000-class and yolo2_face shown in docs), and what are their direct URLs?
- Can the closed-source MaixHub online converter still accept new uploads given libmaix is archived and the V831 is end-of-life — i.e., is the NCNN->AWNN web tool still operational in 2026?
- Does the on-board libmaix_nn.so accept bare .param/.bin via a direct load API (the path nncls.c needs), and what is its exact symbol signature, versus requiring the .mud descriptor wrapper used by the mdsc examples?

## Sources fetched

- [unreliable] https://maixhub.com/modelConvert  _(angle: broad/primary — MaixHub model zoo)_
- [primary] https://github.com/sipeed/libmaix/tree/release/examples/nn_resnet  _(angle: broad/primary — MaixHub model zoo)_
- [blog] https://www.hackster.io/Yukio/how-to-train-a-custom-resnet18-model-for-maix-ii-dock-board-241d52  _(angle: broad/primary — MaixHub model zoo)_
- [primary] https://github.com/sipeed/libmaix/tree/release/examples  _(angle: implementation — libmaix example models)_
- [primary] https://github.com/sipeed/libmaix/tree/release/examples/nn_yolo_20class_mdsc  _(angle: implementation — libmaix example models)_
- [primary] https://github.com/sipeed/maix_train/blob/master/README.md  _(angle: implementation — libmaix example models)_
- [primary] https://wiki.sipeed.com/ai/en/deploy/v831.html  _(angle: rootfs/stack — MaixPy3 v831 bundled models)_
- [primary] https://github.com/sipeed/sipeed_wiki/blob/main/news/MaixPy3/v831_usage/v831_usage.md  _(angle: rootfs/stack — MaixPy3 v831 bundled models)_
- [primary] https://github.com/sipeed/maix_train  _(angle: rootfs/stack — MaixPy3 v831 bundled models)_
- [unreliable] https://maixhub.com/model/zoo  _(angle: rootfs/stack — MaixPy3 v831 bundled models)_
- [forum] https://github.com/sipeed/MaixPy3/issues/9  _(angle: rootfs/stack — MaixPy3 v831 bundled models)_
- [primary] https://wiki.sipeed.com/soft/maixpy/en/course/ai/train/maixhub.html  _(angle: rootfs/stack — MaixPy3 v831 bundled models)_
- [forum] https://github.com/sipeed/MaixPy3/issues/27  _(angle: toolchain/conversion — ncnn to AWNN / NPU compile)_
- [blog] http://jas-hacks.blogspot.com/2021/04/reverse-engineering-v831-npu-neural.html  _(angle: toolchain/conversion — ncnn to AWNN / NPU compile)_
- [primary] https://wiki.sipeed.com/news/others/v831_resnet18/v831_resnet18.html  _(angle: specific architectures — ImageNet/VOC detectors on V831)_
- [primary] https://github.com/sipeed/libmaix  _(angle: specific architectures — ImageNet/VOC detectors on V831)_
- [blog] https://lemariva.com/blog/2020/01/maixpy-object-detector-mobilenet-and-yolov2-sipeed-maix-dock  _(angle: specific architectures — ImageNet/VOC detectors on V831)_
- [primary] https://wiki.sipeed.com/ai/en/index.html  _(angle: specific architectures — ImageNet/VOC detectors on V831)_

## Run stats

```
angles: 5
sourcesFetched: 18
claimsExtracted: 76
claimsVerified: 25
confirmed: 19
killed: 6
afterSynthesis: 12
urlDupes: 9
budgetDropped: 3
agentCalls: 100
```

