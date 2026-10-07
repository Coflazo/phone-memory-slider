"""Spikes 4b + 5: encoder fidelity, zero-shot tag accuracy, and which taste head to ship.

Ground truth = COCO human annotations (dominant supercategory), never encoder output.
A persona "loves" one supercategory; Favorites are n_P of those items; everything else is unlabeled.
We score held-out items and report ROC-AUC (loved vs not) with a bootstrap 95% CI over 30 resamples.
Run: .tools/venv/Scripts/python.exe tools/spikes/taste_spike.py
"""
import json, pathlib, time
import numpy as np
import onnxruntime as ort
import onnxruntime_ep_webgpu as webgpu
from PIL import Image
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import roc_auc_score
from tokenizers import Tokenizer

ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOLS = ROOT / ".tools"
COCO = TOOLS / "datasets" / "coco"
CACHE = TOOLS / "spikes"
CACHE.mkdir(exist_ok=True)
ENCODERS = {
    "B16-224": (TOOLS / "models" / "siglip2-base-patch16-224-ONNX", 224),
    "B32-256": (TOOLS / "models" / "siglip2-base-patch32-256-ONNX", 256),
}
ort.register_execution_provider_library(webgpu.get_ep_name(), webgpu.get_library_path())


def session(path, gpu):
    so = ort.SessionOptions()
    so.log_severity_level = 3
    if gpu:
        gpus = [d for d in ort.get_ep_devices() if d.ep_name == webgpu.get_ep_name()]
        so.add_provider_for_devices([min(gpus, key=lambda d: int(d.device.metadata.get("DxgiHighPerformanceIndex", "99")))], {})
        return ort.InferenceSession(str(path), sess_options=so)
    return ort.InferenceSession(str(path), sess_options=so, providers=["CPUExecutionProvider"])


def unit(x):
    x = x.astype(np.float32)
    return x / np.linalg.norm(x, axis=1, keepdims=True)


def pixels(files, size):
    out = []
    for f in files:
        img = Image.open(f).convert("RGB").resize((size, size), Image.BILINEAR)
        out.append(((np.asarray(img, dtype=np.float32) / 255.0 - 0.5) / 0.5).transpose(2, 0, 1))
    return np.stack(out)


def embed_images(name, variant, gpu, files, size, bs=16):
    suffix = f"_{variant}" if variant else ""
    sess = session(ENCODERS[name][0] / "onnx" / f"vision_model{suffix}.onnx", gpu)
    chunks = [sess.run(["pooler_output"], {"pixel_values": pixels(files[i:i + bs], size)})[0] for i in range(0, len(files), bs)]
    return unit(np.concatenate(chunks))


def embed_texts(name, prompts):
    folder = ENCODERS[name][0]
    tok = Tokenizer.from_file(str(folder / "tokenizer.json"))
    pad = tok.token_to_id("<pad>")
    eos = tok.token_to_id("<eos>")
    ids = np.full((len(prompts), 64), pad, dtype=np.int64)
    for i, p in enumerate(prompts):
        t = tok.encode(p.lower(), add_special_tokens=False).ids[:63]
        t = t + [eos]
        ids[i, :len(t)] = t
    sess = session(folder / "onnx" / "text_model_fp16.onnx", gpu=False)
    return unit(sess.run(["pooler_output"], {"input_ids": ids})[0])


TAG_PROMPTS = {
    "person": ["a photo of people", "a photo of a person"],
    "animal": ["a photo of an animal", "a photo of a pet"],
    "vehicle": ["a photo of a vehicle", "a photo of a car, bus, train or plane"],
    "food": ["a photo of food", "a photo of a meal"],
    "furniture": ["a photo of furniture", "a photo of a bed, couch or table"],
    "electronic": ["a photo of electronics", "a photo of a computer, phone or tv"],
}


def tag_accuracy(name, emb, rows):
    keep = [i for i, r in enumerate(rows) if r["supercategory"] in TAG_PROMPTS]
    classes = list(TAG_PROMPTS)
    t = np.stack([embed_texts(name, TAG_PROMPTS[c]).mean(0) for c in classes])
    t = unit(t)
    pred = np.argmax(emb[keep] @ t.T, axis=1)
    truth = np.array([classes.index(rows[i]["supercategory"]) for i in keep])
    return float((pred == truth).mean()), len(keep)


def heads(train_pos, unlabeled, test):
    centroid = unit(train_pos.mean(0, keepdims=True))[0]
    sims = test @ train_pos.T
    knn = np.sort(sims, axis=1)[:, -5:].mean(1)
    x = np.vstack([train_pos, unlabeled])
    y = np.r_[np.ones(len(train_pos)), np.zeros(len(unlabeled))]
    lr = LogisticRegression(C=1.0, max_iter=2000).fit(x, y)
    return {"centroid": test @ centroid, "knn5": knn, "logistic": lr.decision_function(test)}


def persona_eval(emb, rows, loved, n_p, reps=30, seed=0):
    labels = np.array([r["supercategory"] == loved for r in rows])
    pos_idx = np.flatnonzero(labels)
    scores = {"centroid": [], "knn5": [], "logistic": []}
    rng = np.random.default_rng(seed)
    for _ in range(reps):
        fav = rng.choice(pos_idx, size=n_p, replace=False)
        rest = np.setdiff1d(np.arange(len(rows)), fav)
        # unlabeled pool = 70% of the rest (what the model sees), test = the other 30% (scored)
        rng.shuffle(rest)
        cut = int(0.7 * len(rest))
        unl, test = rest[:cut], rest[cut:]
        out = heads(emb[fav], emb[unl], emb[test])
        for k, s in out.items():
            scores[k].append(roc_auc_score(labels[test], s))
    return {k: (float(np.mean(v)), float(np.percentile(v, 2.5)), float(np.percentile(v, 97.5))) for k, v in scores.items()}


def main():
    rows = json.loads((COCO / "subset.json").read_text())
    files = [COCO / "val2017" / r["file"] for r in rows]
    report = {}
    for name, (_, size) in ENCODERS.items():
        # fp32 on the GPU: the WebGPU EP's fp16 kernels and every int8 export we tried distort SigLIP
        # embeddings (cosine 0.55-0.76 vs fp32), see docs/spikes.md.
        cache = CACHE / f"emb_{name}_fp32.npy"
        if cache.exists():
            emb = np.load(cache)
        else:
            t0 = time.perf_counter()
            emb = embed_images(name, "", True, files, size)
            print(f"{name}: embedded {len(files)} images on GPU in {time.perf_counter() - t0:.1f}s (incl. JPEG decode)")
            np.save(cache, emb)
        acc, n = tag_accuracy(name, emb, rows)
        print(f"{name}: zero-shot coarse tag top-1 = {acc:.3f} on {n} images (6 classes)")
        report[name] = {"tag_top1": acc}
        for loved in ["animal", "food", "person", "vehicle"]:
            n_loved = sum(r["supercategory"] == loved for r in rows)
            for n_p in [20, 50, 100]:
                if n_p > n_loved - 30:
                    continue
                res = persona_eval(emb, rows, loved, n_p)
                report[name][f"{loved}@{n_p}"] = res
                line = "  ".join(f"{k}={m:.3f}[{lo:.3f},{hi:.3f}]" for k, (m, lo, hi) in res.items())
                print(f"{name} persona={loved:8s} n_P={n_p:3d}  {line}", flush=True)
    (CACHE / "taste_spike_report.json").write_text(json.dumps(report, indent=1))


if __name__ == "__main__":
    main()
