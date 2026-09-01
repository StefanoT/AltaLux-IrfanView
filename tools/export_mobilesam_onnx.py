#!/usr/bin/env python3
"""Export and validate the two ONNX graphs consumed by AltaLuxSegmentation.dll."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

import numpy as np
import onnxruntime as ort
import torch

MOBILE_SAM_COMMIT = "f706ad9c4eb7f219c00d9050e46328518ffb65d2"
MOBILE_SAM_CHECKPOINT_SHA256 = "6dbb90523a35330fedd7f1d3dfc66f995213d81b29a5ca8108dbcdd4e37d6c2f"
MODEL_SIZE = 1024
OPSET = 16


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require_pinned_checkout(root: Path) -> None:
    actual = subprocess.check_output(
        ["git", "-C", str(root), "rev-parse", "HEAD"], text=True
    ).strip()
    if actual != MOBILE_SAM_COMMIT:
        raise RuntimeError(
            f"MobileSAM must be checked out at {MOBILE_SAM_COMMIT}; found {actual}"
        )


def export_models(root: Path, checkpoint: Path, output: Path) -> None:
    checkpoint_hash = sha256(checkpoint)
    if checkpoint_hash != MOBILE_SAM_CHECKPOINT_SHA256:
        raise RuntimeError(
            f"MobileSAM checkpoint hash mismatch: expected {MOBILE_SAM_CHECKPOINT_SHA256}, "
            f"found {checkpoint_hash}"
        )
    sys.path.insert(0, str(root))
    from mobile_sam import sam_model_registry  # type: ignore
    from mobile_sam.utils.onnx import SamOnnxModel  # type: ignore

    sam = sam_model_registry["vit_t"](checkpoint=str(checkpoint)).eval()

    class Encoder(torch.nn.Module):
        def __init__(self, model: torch.nn.Module) -> None:
            super().__init__()
            self.model = model

        def forward(self, input_image: torch.Tensor) -> torch.Tensor:
            return self.model.image_encoder(input_image)

    encoder_path = output / "AltaLuxMobileSAMEncoder.onnx"
    decoder_path = output / "AltaLuxMobileSAMDecoder.onnx"
    dummy_image = torch.randn(1, 3, MODEL_SIZE, MODEL_SIZE)
    torch.onnx.export(
        Encoder(sam),
        dummy_image,
        encoder_path,
        opset_version=OPSET,
        do_constant_folding=True,
        input_names=["input_image"],
        output_names=["image_embeddings"],
        dynamo=False,
    )

    decoder = SamOnnxModel(model=sam, return_single_mask=False)
    embed_size = sam.prompt_encoder.image_embedding_size
    dummy_inputs = {
        "image_embeddings": torch.randn(1, sam.prompt_encoder.embed_dim, *embed_size),
        "point_coords": torch.tensor([[[512.0, 512.0]]]),
        "point_labels": torch.tensor([[1.0]]),
        "mask_input": torch.zeros(1, 1, *(4 * value for value in embed_size)),
        "has_mask_input": torch.tensor([0.0]),
        "orig_im_size": torch.tensor([768.0, 1024.0]),
    }
    torch.onnx.export(
        decoder,
        tuple(dummy_inputs.values()),
        decoder_path,
        opset_version=OPSET,
        do_constant_folding=True,
        input_names=list(dummy_inputs),
        output_names=["masks", "iou_predictions", "low_res_masks"],
        dynamic_axes={
            "point_coords": {1: "num_points"},
            "point_labels": {1: "num_points"},
            "masks": {2: "mask_height", 3: "mask_width"},
        },
        dynamo=False,
    )

    encoder_session = ort.InferenceSession(str(encoder_path), providers=["CPUExecutionProvider"])
    embeddings = encoder_session.run(None, {"input_image": dummy_image.numpy()})[0]
    decoder_inputs = {name: value.numpy() for name, value in dummy_inputs.items()}
    decoder_inputs["image_embeddings"] = embeddings
    decoder_session = ort.InferenceSession(str(decoder_path), providers=["CPUExecutionProvider"])
    masks, scores, low_res = decoder_session.run(None, decoder_inputs)
    if masks.shape[:2] != scores.shape or masks.shape[2:] != (768, 1024):
        raise RuntimeError(f"Unexpected decoder outputs: {masks.shape}, {scores.shape}")
    if low_res.shape[2:] != (256, 256):
        raise RuntimeError(f"Unexpected low-resolution mask shape: {low_res.shape}")

    manifest = {
        "schemaVersion": 1,
        "mobileSamCommit": MOBILE_SAM_COMMIT,
        "checkpointSha256": checkpoint_hash,
        "opset": OPSET,
        "encoder": {"file": encoder_path.name, "sha256": sha256(encoder_path)},
        "decoder": {"file": decoder_path.name, "sha256": sha256(decoder_path)},
    }
    (output / "AltaLuxSegmentation.models.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mobile-sam-root", type=Path, required=True)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    require_pinned_checkout(args.mobile_sam_root)
    args.output.mkdir(parents=True, exist_ok=True)
    export_models(args.mobile_sam_root, args.checkpoint, args.output)


if __name__ == "__main__":
    main()
