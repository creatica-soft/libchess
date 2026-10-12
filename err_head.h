#pragma once
// THE PER-MOVE ERROR HEAD, flat, no libtorch. Written by moveerr_export.cpp.
//
// WHAT IT PREDICTS. For a move m at a node, the ABSOLUTE DIFFERENCE, in pawns, between the static
// NNUE evaluation of the child and what a depth-8 search of that child says -- both expressed in
// the parent's frame. It is a prediction of how wrong a static leaf value is about to be, not a
// prediction of how good the move is. A large value means "the cheap estimate of this move cannot
// be trusted", which is what the search uses to decide where to spend a leaf search.
//
// WHY IT IS A SEPARATE FILE AND MAGIC. The architecture is policy_net.h's trunk and move table
// exactly -- it was chosen so the feature vector the accumulator stack already maintains could be
// reused and the cost would be the policy head's cost -- but the net is not interchangeable with a
// policy net: it carries a per-row bias no policy format has a slot for, and its output is a
// magnitude in pawns rather than a logit. Reading one as the other would run silently and score
// every move with a monotone-wrong quantity, so the two formats refuse each other on the magic.
//
// MEASURED (held-out, game-level split, 3.6M moves):
//     predict the global mean .................. 161.3 cp
//     predict each POSITION's own mean ......... 125.7 cp   <- oracle move-blind ceiling
//     THIS HEAD (NNUE features) ................ 101.0 cp, r = 0.65, recall at 10% = 44.8%
//     the same head on 21 CNN planes ...........  80.4 cp, r = 0.78, recall at 10% = 61.9%
// The CNN predicts the error substantially better, but on the only metric the search consumes --
// which child the node ranks first after the flagged moves are resolved -- the two are nearly
// level: 32.3% against 32.9%, where ranking by static alone is 27.2% and resolving EVERY move is
// 52.5%. Predicting the error well and ranking suspicion well are not the same job, and the cheap
// representation is enough for the second.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>

struct ErrHead {
  int in = 0, h1 = 0, h2 = 0, out = 0;
  std::vector<float> W1, b1, W2, b2, emb, eb;
  bool loaded = false;
};

// Returns false and leaves head.loaded == false if the file is missing or malformed, so the caller
// can run without the head rather than flag moves with a random net.
inline bool err_head_load(ErrHead& head, const char* path, char* err, size_t errlen) {
  head.loaded = false;
  FILE* f = std::fopen(path, "rb");
  if (!f) { snprintf(err, errlen, "cannot open %s", path); return false; }
  int32_t hdr[6];
  if (std::fread(hdr, sizeof(int32_t), 6, f) != 6) {
    snprintf(err, errlen, "%s: short header", path); std::fclose(f); return false;
  }
  if (hdr[0] != 0x48525245) {
    snprintf(err, errlen, "%s: bad magic %08x -- a policy net is not an error head", path, hdr[0]);
    std::fclose(f); return false;
  }
  if (hdr[1] != 1) {
    snprintf(err, errlen, "%s: error-head format version %d is not supported", path, hdr[1]);
    std::fclose(f); return false;
  }
  head.in = hdr[2]; head.h1 = hdr[3]; head.h2 = hdr[4]; head.out = hdr[5];
  if (head.out != 64 * 64) {
    snprintf(err, errlen, "%s: expected %d move rows, header says %d", path, 64 * 64, head.out);
    std::fclose(f); return false;
  }
  if (head.in <= 0 || head.h1 <= 0 || head.h2 <= 0) {
    snprintf(err, errlen, "%s: nonsense dimensions %d/%d/%d", path, head.in, head.h1, head.h2);
    std::fclose(f); return false;
  }
  auto rd = [&](std::vector<float>& v, size_t n) {
    v.resize(n);
    return std::fread(v.data(), sizeof(float), n, f) == n;
  };
  const bool ok = rd(head.W1, (size_t)head.in * head.h1) && rd(head.b1, head.h1)
               && rd(head.W2, (size_t)head.h1 * head.h2) && rd(head.b2, head.h2)
               && rd(head.emb, (size_t)head.out * head.h2) && rd(head.eb, head.out);
  // Anything after eb means the file is not what its header claims.
  unsigned char extra;
  const bool trailing = (ok && std::fread(&extra, 1, 1, f) == 1);
  std::fclose(f);
  if (!ok)       { snprintf(err, errlen, "%s: short tensor data", path); return false; }
  if (trailing)  { snprintf(err, errlen, "%s: trailing bytes after the last tensor", path); return false; }
  head.loaded = true;
  return true;
}

// The trunk, once per position. ctx must have room for head.h2 floats. x is head.in bytes of NNUE
// features -- the SAME buffer policy_context() reads, so a node that already computed it for the
// policy can pass it straight in.
inline void err_context(const ErrHead& head, const uint8_t* x, float* ctx) {
  const int H1 = head.h1, H2 = head.h2;
  std::vector<float> h(head.b1);
  for (int i = 0; i < head.in; ++i) {
    const uint8_t xi = x[i];
    if (!xi) continue;                             // most features are zero
    const float s = xi * (1.0f / 127.0f);
    const float* w = &head.W1[(size_t)i * H1];
    for (int j = 0; j < H1; ++j) h[j] += s * w[j];
  }
  for (int j = 0; j < H1; ++j) if (h[j] < 0.0f) h[j] = 0.0f;
  std::memcpy(ctx, head.b2.data(), sizeof(float) * H2);
  for (int j = 0; j < H1; ++j) {
    const float hj = h[j];
    if (hj == 0.0f) continue;                      // relu leaves about half of h at exactly zero
    const float* w = &head.W2[(size_t)j * H2];
    for (int k = 0; k < H2; ++k) ctx[k] += hj * w[k];
  }
  for (int k = 0; k < H2; ++k) if (ctx[k] < 0.0f) ctx[k] = 0.0f;
}

// One move's predicted error, IN PAWNS, from the trunk output.
//
// `flip` must be true exactly when Black is to move: the head was trained on squares mirrored by
// ^56 for Black, so passing raw squares looks up an unrelated row and predicts noise. This is a
// required argument for the same reason policy_score()'s is.
//
// Promotions collapse onto their from-to row, which is what the labels did: the four promotion
// pieces of one pawn push share a predicted error.
//
// The readout as the trainer computed it, unclamped. Exposed separately ONLY so the export can be
// verified against libtorch's own predictions, which are unclamped: comparing against the clamped
// value would hide a sign error on every row the model puts below zero.
inline float err_score_raw(const ErrHead& head, const float* ctx, int from, int to, bool flip) {
  if (flip) { from ^= 56; to ^= 56; }
  const size_t row = (size_t)(from * 64 + to);
  const float* e = &head.emb[row * head.h2];
  float s = head.eb[row];
  for (int k = 0; k < head.h2; ++k) s += ctx[k] * e[k];
  return s;
}

// What the search should use. Clamped at zero because the target is an absolute value and a linear
// readout can land below it; a negative predicted error is not a weaker flag than zero, it is
// nonsense, and leaving it negative would make such moves sort BELOW the confident ones.
inline float err_score(const ErrHead& head, const float* ctx, int from, int to, bool flip) {
  const float s = err_score_raw(head, ctx, from, to, flip);
  return s > 0.0f ? s : 0.0f;
}
