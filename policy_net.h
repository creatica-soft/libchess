// Inference for the NNUE-representation policy head.
//
// The model is trained by nnue_policy_train.cpp and exported with EXPORT_WEIGHTS=<path>.
// Nothing here needs libtorch: the whole net is three matrix products, and the engine
// links only against libchess.
//
//   ctx     = relu( relu(x/127 @ W1 + b1) @ W2 + b2 )      once per position
//   score(m) = ctx . emb[from*64 + to]                     once per legal move
//
// x is the NNUE feature transformer's post-activation output for the position, which the
// search already maintains incrementally -- so the expensive part of the input is free.
// Only 12-16% of those 1024 values are non-zero, and the first layer skips the rest, which
// is what makes this ~8x cheaper than its dense cost.
#ifndef POLICY_NET_H
#define POLICY_NET_H

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

struct PolicyNet {
  int in = 0, h1 = 0, h2 = 0, out = 0, conv = 0;
  std::vector<float> W1, b1, W2, b2, emb;
  bool loaded = false;
};

// Returns false and leaves net.loaded == false if the file is missing or malformed, so the
// caller can fall back to NNUE priors rather than play with a random net.
inline bool policy_net_load(PolicyNet& net, const char* path, char* err, size_t errlen) {
  net.loaded = false;
  FILE* f = std::fopen(path, "rb");
  if (!f) { snprintf(err, errlen, "cannot open %s", path); return false; }
  int32_t hdr[7];
  if (std::fread(hdr, sizeof(int32_t), 7, f) != 7) {
    snprintf(err, errlen, "%s: short header", path); std::fclose(f); return false;
  }
  if (hdr[0] != 0x4C4F5043) {
    snprintf(err, errlen, "%s: bad magic %08x", path, hdr[0]); std::fclose(f); return false;
  }
  //REFUSE A FORMAT THIS ENGINE CANNOT APPLY. hdr[1] is the format version and was previously read
  //into nothing. Version 2 appends Wl and version 3 appends Cl -- the LEGAL_BIAS term, which
  //biases the move readout by the legal move set. Loading such a file with the code below would
  //succeed, because the extra tensor simply sits unread at the end of the file, and the engine
  //would then play with a net that is NOT the one that was trained: every weight was fitted with
  //that term contributing, and dropping it silently changes every score. Fall back to the NNUE
  //prior with a message rather than play a net we are only partly running.
  if (hdr[1] != 1) {
    snprintf(err, errlen, "%s: policy format version %d needs the legality-bias term, which this "
                          "engine does not implement; rebuild with it or export a v1 net",
             path, hdr[1]);
    std::fclose(f); return false;
  }
  net.in = hdr[2]; net.h1 = hdr[3]; net.h2 = hdr[4]; net.out = hdr[5]; net.conv = hdr[6];
  if (net.conv != 0) {
    snprintf(err, errlen, "%s: CONV_POLICY=1 export is not supported by this engine", path);
    std::fclose(f); return false;
  }
  auto rd = [&](std::vector<float>& v, size_t n) {
    v.resize(n);
    return std::fread(v.data(), sizeof(float), n, f) == n;
  };
  const bool ok = rd(net.W1, (size_t)net.in * net.h1) && rd(net.b1, net.h1)
               && rd(net.W2, (size_t)net.h1 * net.h2) && rd(net.b2, net.h2)
               && rd(net.emb, (size_t)net.out * net.h2);
  std::fclose(f);
  if (!ok) { snprintf(err, errlen, "%s: truncated tensor data", path); return false; }
  //The engine scores into fixed-size stack buffers. Refuse anything that would not fit
  //rather than letting a mismatched export overrun them.
#ifdef POLICY_MAX_IN
  if (net.in > POLICY_MAX_IN || net.h2 > POLICY_MAX_H2) {
    snprintf(err, errlen, "%s: %dx%d exceeds the engine's %dx%d buffers",
             path, net.in, net.h2, POLICY_MAX_IN, POLICY_MAX_H2);
    return false;
  }
#endif
  net.loaded = true;
  return true;
}

// Trunk. ctx must have room for net.h2 floats. x is net.in bytes of NNUE features.
inline void policy_context(const PolicyNet& net, const uint8_t* x, float* ctx) {
  const int H1 = net.h1, H2 = net.h2;
  float* h = (float*)alloca(sizeof(float) * H1);
  std::memcpy(h, net.b1.data(), sizeof(float) * H1);
  // Sparse over the input: a zero feature contributes nothing, and most of them are zero.
  for (int i = 0; i < net.in; ++i) {
    const uint8_t xi = x[i];
    if (!xi) continue;
    const float s = xi * (1.0f / 127.0f);
    const float* w = &net.W1[(size_t)i * H1];
    for (int j = 0; j < H1; ++j) h[j] += s * w[j];
  }
  for (int j = 0; j < H1; ++j) if (h[j] < 0.0f) h[j] = 0.0f;
  std::memcpy(ctx, net.b2.data(), sizeof(float) * H2);
  // Sparse again: relu leaves roughly half of h at exactly zero.
  for (int j = 0; j < H1; ++j) {
    const float hj = h[j];
    if (hj == 0.0f) continue;
    const float* w = &net.W2[(size_t)j * H2];
    for (int k = 0; k < H2; ++k) ctx[k] += hj * w[k];
  }
  for (int k = 0; k < H2; ++k) if (ctx[k] < 0.0f) ctx[k] = 0.0f;
}

// One move's logit. Promotions share their queen-move slot, which is what the trainer's
// policy_index() does when CONV_POLICY is 0.
//
// `flip` must be true exactly when Black is to move. The trainer orients the board to the
// side to move and indexes the move table with squares mirrored by ^56, so the model only
// ever learned one colour's geometry. Passing raw squares for Black looks up an unrelated
// embedding row and scores at chance -- which is why this is a required argument and not a
// convenience the caller can leave out.
inline float policy_score(const PolicyNet& net, const float* ctx, int from, int to, bool flip) {
  if (flip) { from ^= 56; to ^= 56; }
  const float* e = &net.emb[(size_t)(from * 64 + to) * net.h2];
  float s = 0.0f;
  for (int k = 0; k < net.h2; ++k) s += ctx[k] * e[k];
  return s;
}

#endif
