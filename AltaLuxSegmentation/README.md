# AltaLux AI selection add-on

The optional x64 add-on consists of these files, all installed directly beside
`AltaLux.dll` in the IrfanView `Plugins` directory:

- `AltaLuxSegmentation.dll`
- `onnxruntime.dll`
- `onnxruntime_providers_shared.dll`
- `AltaLuxMobileSAMEncoder.onnx`
- `AltaLuxMobileSAMDecoder.onnx`
- `AltaLuxSegmentation.models.json`
- `AltaLuxSegmentation-THIRD-PARTY-NOTICES.txt`

AltaLux loads the module by absolute path from its own directory. It does not
search the process working directory or `PATH`. If any component is absent,
the normal whole-image filter remains available.

The models are generated with `tools/export_mobilesam_onnx.py` from MobileSAM
commit `f706ad9c4eb7f219c00d9050e46328518ffb65d2`. The script exports both the
TinyViT image encoder and SAM prompt/mask decoder, validates their tensor
contract with ONNX Runtime, and writes the checksummed manifest.
