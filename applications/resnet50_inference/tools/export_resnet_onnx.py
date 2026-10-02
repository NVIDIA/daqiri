#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# SPDX-License-Identifier: Apache-2.0
"""Export catalogued vision feature extractors to ONNX with in-model normalization."""

import argparse
import json
from pathlib import Path

INT8_OFFSET = 128.0
IMAGENET_MEAN = (0.485, 0.456, 0.406)
IMAGENET_STD = (0.229, 0.224, 0.225)
CATALOG_PATH = Path(__file__).resolve().parents[1] / "model_catalog.json"


def load_catalog(path: Path) -> dict[str, dict[str, object]]:
    with path.open() as catalog_file:
        return json.load(catalog_file)


class FeaturesWithNorm:
    """Construct the Torch module only after argument-only catalog operations."""

    @staticmethod
    def build(profile: dict[str, object], input_dtype: str):
        import torch
        import torchvision

        architecture = str(profile["architecture"])
        backbone = getattr(torchvision.models, architecture)(weights="DEFAULT")
        if architecture.startswith("resnet"):
            backbone.fc = torch.nn.Identity()
        else:
            backbone.classifier = torch.nn.Identity()

        class NormalizedFeatures(torch.nn.Module):
            def __init__(self):
                super().__init__()
                self.backbone = backbone
                scale = [1.0 / (255.0 * stddev) for stddev in IMAGENET_STD]
                bias = [(INT8_OFFSET / 255.0 - mean) / stddev
                        for mean, stddev in zip(IMAGENET_MEAN, IMAGENET_STD)]
                self.register_buffer("scale", torch.tensor(scale, dtype=torch.float16).view(1, 3, 1, 1))
                self.register_buffer("bias", torch.tensor(bias, dtype=torch.float16).view(1, 3, 1, 1))

            def forward(self, image):
                if input_dtype == "int8":
                    image = image.to(dtype=torch.float16)
                return self.backbone(image * self.scale + self.bias).float()

        return NormalizedFeatures().eval().half()


def main() -> None:
    catalog = load_catalog(CATALOG_PATH)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", default="resnet50", choices=sorted(catalog))
    parser.add_argument("--output", help="output .onnx path; defaults to catalog path")
    parser.add_argument("--input-dtype", choices=["fp16", "int8"], help="defaults to catalog input_type")
    parser.add_argument("--opset", type=int, default=18)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--list-models", action="store_true")
    parser.add_argument("--print-field", choices=["onnx_path", "engine_path", "feature_dim", "input_type"])
    args = parser.parse_args()

    if args.list_models:
        print(" ".join(sorted(catalog)))
        return

    profile = catalog[args.model]
    if args.print_field:
        print(profile[args.print_field])
        return

    input_dtype = args.input_dtype or str(profile["input_type"])
    output = Path(args.output or str(profile["onnx_path"]))
    output.parent.mkdir(parents=True, exist_ok=True)

    import torch

    model = FeaturesWithNorm.build(profile, input_dtype)
    if input_dtype == "int8":
        dummy = torch.randint(-128, 128, (1, 3, 224, 224), dtype=torch.int8)
    else:
        dummy = torch.randn(1, 3, 224, 224, dtype=torch.float16)

    torch.onnx.export(
        model, dummy, output, export_params=True, opset_version=args.opset,
        do_constant_folding=True, input_names=["input"], output_names=["features"],
        dynamic_axes={"input": {0: "batch"}, "features": {0: "batch"}},
    )
    print(f"Exported {args.model} ({input_dtype} input) -> {output} "
          f"(feature_dim={profile['feature_dim']})")

    if args.check:
        import onnx
        exported = onnx.load(output)
        onnx.checker.check_model(exported)
        input_type = exported.graph.input[0].type.tensor_type.elem_type
        output_type = exported.graph.output[0].type.tensor_type.elem_type
        expected_input = 3 if input_dtype == "int8" else 10
        if input_type != expected_input or output_type != 1:
            raise SystemExit(f"unexpected ONNX types: input={input_type}, output={output_type}")
        print(f"onnx.checker OK; input elem_type={input_type}; output elem_type={output_type}")


if __name__ == "__main__":
    main()
