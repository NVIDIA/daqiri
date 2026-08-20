#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# All rights reserved. SPDX-License-Identifier: Apache-2.0
"""Export a torchvision ResNet *feature extractor* to ONNX with in-model norm.

FP16 path (default): ONNX input is FLOAT16; features output is FLOAT32.
DAQIRI does int8→fp16 before TRT. INT8 path (--input-dtype int8): ONNX input
is INT8; DAQIRI reorders int8 passthrough and TRT casts+norms inside the graph
(drops the reorder convert). Features stay FLOAT32 for FeatureSink.
  python3 export_resnet_onnx.py --model resnet50 --output models/resnet50_features.onnx
  python3 export_resnet_onnx.py --model resnet50 --input-dtype int8 \
      --output models/resnet50_features.int8in.onnx
"""

import argparse
import os

import torch
import torchvision

INT8_OFFSET = 128.0
IMAGENET_MEAN = (0.485, 0.456, 0.406)
IMAGENET_STD = (0.229, 0.224, 0.225)

FEATURE_DIM = {
    "resnet18": 512,
    "resnet34": 512,
    "resnet50": 2048,
    "resnet101": 2048,
    "resnet152": 2048,
}


class ResNetFeaturesWithNorm(torch.nn.Module):
    """Pixels in (fp16 or int8), ImageNet-normalized FP32 features out.

    For fp16 input: x is already (pixel_uint8 - INT8_OFFSET) as half.
    For int8 input: cast to half first, then same affine.
      a_c = 1 / (255 * std_c)
      b_c = (INT8_OFFSET / 255 - mean_c) / std_c

    Backbone runs in fp16; features are cast to float32 so the ONNX/TRT
    output binding matches FeatureSink (host float32).
    """

    def __init__(self, backbone: torch.nn.Module, input_dtype: str):
        super().__init__()
        self.backbone = backbone
        self.input_dtype = input_dtype
        a = [1.0 / (255.0 * s) for s in IMAGENET_STD]
        b = [(INT8_OFFSET / 255.0 - m) / s for m, s in zip(IMAGENET_MEAN, IMAGENET_STD)]
        self.register_buffer("scale", torch.tensor(a, dtype=torch.float16).view(1, 3, 1, 1))
        self.register_buffer("bias", torch.tensor(b, dtype=torch.float16).view(1, 3, 1, 1))

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        if self.input_dtype == "int8":
            x = x.to(dtype=torch.float16)
        x = x * self.scale + self.bias
        return self.backbone(x).float()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--model", default="resnet50", choices=sorted(FEATURE_DIM))
    ap.add_argument("--output", default=None,
                    help="output .onnx path")
    ap.add_argument("--weights", default="DEFAULT", choices=["DEFAULT", "none"])
    ap.add_argument("--opset", type=int, default=18)
    ap.add_argument("--input-dtype", default="fp16", choices=["fp16", "int8"])
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    default_name = (f"{args.model}_features.int8in.onnx" if args.input_dtype == "int8"
                    else f"{args.model}_features.onnx")
    out = args.output or os.path.join("models", default_name)
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)

    weights = None if args.weights == "none" else "DEFAULT"
    backbone = getattr(torchvision.models, args.model)(weights=weights)
    backbone.fc = torch.nn.Identity()
    model = ResNetFeaturesWithNorm(backbone, args.input_dtype)
    model.eval()
    model.half()

    if args.input_dtype == "int8":
        dummy = torch.randint(-128, 128, (1, 3, 224, 224), dtype=torch.int8)
    else:
        dummy = torch.randn(1, 3, 224, 224, dtype=torch.float16)

    torch.onnx.export(
        model,
        dummy,
        out,
        export_params=True,
        opset_version=args.opset,
        do_constant_folding=True,
        input_names=["input"],
        output_names=["features"],
        dynamic_axes={"input": {0: "batch"}, "features": {0: "batch"}},
    )
    print(f"Exported {args.model} feature extractor "
          f"({args.input_dtype} in + in-model norm) -> {out} "
          f"(feature_dim={FEATURE_DIM[args.model]}, weights={args.weights})")

    if args.check:
        import onnx
        m = onnx.load(out)
        onnx.checker.check_model(m)
        inp = m.graph.input[0]
        outp = m.graph.output[0]
        in_elem = inp.type.tensor_type.elem_type
        out_elem = outp.type.tensor_type.elem_type
        # FLOAT=1, FLOAT16=10, INT8=3
        print(f"onnx.checker OK; input elem_type={in_elem} (10=FLOAT16, 3=INT8); "
              f"name={inp.name}")
        print(f"output elem_type={out_elem} (1=FLOAT); name={outp.name}")
        if out_elem != 1:
            raise SystemExit(
                f"features output must be FLOAT (1), got elem_type={out_elem}; "
                "ensure forward() ends with .float()")


if __name__ == "__main__":
    main()
