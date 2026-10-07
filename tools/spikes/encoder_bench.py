"""Spike 4: SigLIP2 vision encoder throughput on WebGPU vs CPU, and int8/fp16 fidelity vs fp32.

Run: .tools/venv/Scripts/python.exe tools/spikes/encoder_bench.py [image_dir]
Prints one line per (model, provider, batch) and a fidelity table. Dev-time only.
"""
import pathlib, sys, time
import numpy as np
import onnxruntime as ort
import onnxruntime_ep_webgpu as webgpu
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[2]
MODELS = ROOT / ".tools" / "models"
B16 = MODELS / "siglip2-base-patch16-224-ONNX" / "onnx"
B32 = MODELS / "siglip2-base-patch32-256-ONNX" / "onnx"

ort.register_execution_provider_library(webgpu.get_ep_name(), webgpu.get_library_path())


def session(path, provider):
    so = ort.SessionOptions()
    so.log_severity_level = 3
    if provider == "webgpu":
        # The plugin takes exactly one device: use the adapter Windows ranks as high-performance first.
        gpus = [d for d in ort.get_ep_devices() if d.ep_name == webgpu.get_ep_name()]
        best = min(gpus, key=lambda d: int(d.device.metadata.get("DxgiHighPerformanceIndex", "99")))
        so.add_provider_for_devices([best], {})
        return ort.InferenceSession(str(path), sess_options=so)
    return ort.InferenceSession(str(path), sess_options=so, providers=["CPUExecutionProvider"])


def preprocess(img, size):
    img = img.convert("RGB").resize((size, size), Image.BILINEAR)
    x = np.asarray(img, dtype=np.float32) / 255.0
    return ((x - 0.5) / 0.5).transpose(2, 0, 1)


def embed(sess, batch):
    out = sess.run(["pooler_output"], {"pixel_values": batch})[0].astype(np.float32)
    return out / np.linalg.norm(out, axis=1, keepdims=True)


def throughput(sess, size, bs, seconds=6.0):
    x = np.random.default_rng(0).uniform(-1, 1, (bs, 3, size, size)).astype(np.float32)
    embed(sess, x)  # warm-up
    n, t0 = 0, time.perf_counter()
    while time.perf_counter() - t0 < seconds:
        embed(sess, x)
        n += bs
    return n / (time.perf_counter() - t0)


def main():
    images = sorted(pathlib.Path(sys.argv[1]).glob("*.jpg"))[:96] if len(sys.argv) > 1 else []
    for name, folder, size in [("B16-224", B16, 224), ("B32-256", B32, 256)]:
        for variant in ["fp16", "int8"]:
            path = folder / f"vision_model_{variant}.onnx"
            if not path.exists():
                print(f"skip {name} {variant}: missing")
                continue
            for provider in ["webgpu", "cpu"]:
                if provider == "webgpu" and variant == "int8":
                    continue  # int8 is the CPU artifact
                t0 = time.perf_counter()
                try:
                    sess = session(path, provider)
                except Exception as e:  # noqa: BLE001 - spike: report and continue
                    print(f"{name} {variant} {provider}: FAILED to load: {e}")
                    continue
                cold = time.perf_counter() - t0
                for bs in ([1, 8, 32] if provider == "webgpu" else [1, 8]):
                    print(f"{name} {variant:5s} {provider:6s} bs={bs:2d} cold={cold:5.2f}s  {throughput(sess, size, bs):7.1f} img/s", flush=True)
    if images:
        fidelity(images)


def fidelity(images):
    x = np.stack([preprocess(Image.open(p), 224) for p in images])
    ref = embed(session(B16 / "vision_model.onnx", "cpu"), x)
    for variant, provider in [("fp16", "webgpu"), ("fp16", "cpu"), ("int8", "cpu")]:
        got = embed(session(B16 / f"vision_model_{variant}.onnx", provider), x)
        cos = np.sum(ref * got, axis=1)
        print(f"fidelity B16 {variant} {provider}: cosine to fp32 min={cos.min():.4f} mean={cos.mean():.4f} over {len(images)} images")


if __name__ == "__main__":
    main()
