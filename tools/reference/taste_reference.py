"""Writes core/tests/fixtures/taste_reference.bin: X (200x16 float32), y (200 float32), sklearn w (16), b (1)."""
import pathlib, numpy as np
from sklearn.linear_model import LogisticRegression
rng = np.random.default_rng(0)
X = rng.normal(size=(200, 16)).astype(np.float32)
X /= np.linalg.norm(X, axis=1, keepdims=True)
true = rng.normal(size=16)
y = (X @ true + 0.3 * rng.normal(size=200) > 0.4).astype(np.float32)
lr = LogisticRegression(C=1.0, tol=1e-10, max_iter=10000).fit(X, y)
out = pathlib.Path(__file__).resolve().parents[2] / "core/tests/fixtures/taste_reference.bin"
out.parent.mkdir(parents=True, exist_ok=True)
np.concatenate([X.ravel(), y, lr.coef_.ravel().astype(np.float32), lr.intercept_.astype(np.float32)]).astype("<f4").tofile(out)
print(out, out.stat().st_size)
