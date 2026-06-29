#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# All rights reserved. SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Export a torchvision ResNet *feature extractor* (final FC stripped) to ONNX.

The fully-connected classifier head is replaced with Identity, so the network
outputs the global-average-pooled feature vector (512 dims for resnet18/34,
2048 for resnet50/101/152). A dynamic batch axis lets one engine serve any
inference batch size. Run inside the BASE_IMAGE=torch container (torch +
torchvision pre-installed).

  python3 export_resnet_onnx.py --model resnet50 --output models/resnet50_features.onnx
"""

import argparse
import os

import torch
import torchvision

FEATURE_DIM = {
    "resnet18": 512,
    "resnet34": 512,
    "resnet50": 2048,
    "resnet101": 2048,
    "resnet152": 2048,
}


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--model", default="resnet50", choices=sorted(FEATURE_DIM))
    ap.add_argument("--output", default=None,
                    help="output .onnx path (default: models/<model>_features.onnx)")
    ap.add_argument("--weights", default="DEFAULT", choices=["DEFAULT", "none"],
                    help="DEFAULT = ImageNet-pretrained (meaningful features); "
                         "none = random init (offline)")
    ap.add_argument("--opset", type=int, default=18,
                    help="ONNX opset (>=18 avoids a noisy down-conversion in newer "
                         "torch.onnx exporters; TensorRT 10 handles opset 18)")
    ap.add_argument("--check", action="store_true", help="run onnx.checker on the result")
    args = ap.parse_args()

    out = args.output or os.path.join("models", f"{args.model}_features.onnx")
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)

    weights = None if args.weights == "none" else "DEFAULT"
    model = getattr(torchvision.models, args.model)(weights=weights)
    model.fc = torch.nn.Identity()  # output = pooled feature vector [N, feature_dim]
    model.eval()

    dummy = torch.randn(1, 3, 224, 224)
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
    print(f"Exported {args.model} feature extractor -> {out} "
          f"(feature_dim={FEATURE_DIM[args.model]}, weights={args.weights})")

    if args.check:
        import onnx
        m = onnx.load(out)
        onnx.checker.check_model(m)
        out_shape = [d.dim_value or d.dim_param
                     for d in m.graph.output[0].type.tensor_type.shape.dim]
        print(f"onnx.checker OK; output '{m.graph.output[0].name}' shape={out_shape}")


if __name__ == "__main__":
    main()
