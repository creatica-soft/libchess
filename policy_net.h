// Inference for the NNUE-representation policy head.
//
// The model is trained by nnue_policy_train.cpp and exported with EXPORT_WEIGHTS=<path>.
// Nothing here needs libtorch: the whole net is three matrix products, and the engine
// links only against libchess.
//
//   ctx     = relu( relu(x/127 @ W1 + b1) @ W2 + b2 )      once per position
//   score(m) = ctx . emb[row(m)]                           once per legal move
//
// row(m) is from*64 + to for a format-1 net, and (piece_type-1)*4096 + from*64 + to for a
// format-4 one. The piece type belongs in the index because without it a rook moving e1-e4 and a
// queen moving e1-e4 share a single embedding row, and the model can only tell them apart through
// the 128-dim global position code -- see the note on format 4 in policy_net_load().
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
  //THE LEGALITY TERM, [h2,h2]. Present in format 2 and 5. See policy_apply_legal_bias().
  std::vector<float> Wl;
  bool has_wl = false;
  bool loaded = false;
  //Set from the format version, not guessed from `out`: a wrong guess here silently reads the
  //wrong embedding row for every move, which scores at chance without any diagnostic.
  bool piece_indexed = false;
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
  //FORMAT 4 is format 1's tensor layout (W1, b1, W2, b2, emb) with the move table indexed by
  //(piece_type-1)*4096 + from*64 + to instead of from*64 + to, so emb has 24576 rows rather than
  //4096. It is a separate version number rather than something inferred from hdr[5] on purpose:
  //an engine that predates it must REFUSE such a net, and it does, because it tests for
  //hdr[1] != 1. Inferring from the output dimension would have let an old binary load it and
  //index the wrong row for every move.
  //Versions 1 and 4 carry the five tensors this engine applies. 2/3 and 5/6 append the
  //legality-bias term (Wl or Cl), which it does not implement; 4/5/6 are the piece-indexed
  //counterparts of 1/2/3.
  //1 and 4 carry the five base tensors; 2 and 5 append Wl, the legality term. 3 and 6 append Cl
  //instead -- a full [out,h2] table rather than an [h2,h2] map -- which this engine does not
  //implement. 4, 5 and 6 are the piece-indexed counterparts of 1, 2 and 3.
  if (hdr[1] < 1 || hdr[1] > 6 || hdr[1] == 3 || hdr[1] == 6) {
    snprintf(err, errlen, "%s: policy format version %d is not supported by this engine. "
                          "1/4 = base tensors, 2/5 = with the legality term; 3 and 6 use the "
                          "untied Cl table, which is not implemented",
             path, hdr[1]);
    std::fclose(f); return false;
  }
  net.piece_indexed = (hdr[1] >= 4);
  net.has_wl        = (hdr[1] == 2 || hdr[1] == 5);
  net.in = hdr[2]; net.h1 = hdr[3]; net.h2 = hdr[4]; net.out = hdr[5]; net.conv = hdr[6];
  //BEFORE the width check below, because a CONV_POLICY export has 4672 rows and would otherwise be
  //refused for its width -- a true statement that points at the wrong cause.
  if (net.conv != 0) {
    snprintf(err, errlen, "%s: CONV_POLICY=1 export is not supported by this engine", path);
    std::fclose(f); return false;
  }
  //The output dimension is implied by the format, so disagreement means the file is not what its
  //header claims and every row lookup below would be off. Refuse rather than read out of bounds.
  {
    const int want = net.piece_indexed ? 6 * 64 * 64 : 64 * 64;
    if (net.out != want) {
      snprintf(err, errlen, "%s: format %d implies %d move rows but the header says %d",
               path, hdr[1], want, net.out);
      std::fclose(f); return false;
    }
  }
  auto rd = [&](std::vector<float>& v, size_t n) {
    v.resize(n);
    return std::fread(v.data(), sizeof(float), n, f) == n;
  };
  bool ok = rd(net.W1, (size_t)net.in * net.h1) && rd(net.b1, net.h1)
         && rd(net.W2, (size_t)net.h1 * net.h2) && rd(net.b2, net.h2)
         && rd(net.emb, (size_t)net.out * net.h2);
  //Wl comes last in the file, so it reads straight on from emb.
  if (ok && net.has_wl) ok = rd(net.Wl, (size_t)net.h2 * net.h2);
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
//
// `pt` is the type of the piece standing on `from`, Pawn=1 .. King=6, and it is read from the
// board BEFORE the move is made -- after it, the from-square is empty. It is NOT oriented: a
// piece's type is the same enum value for either colour, so ^56 does not apply to it. It is
// ignored entirely by a format-1 net.
// The embedding row a move indexes. Shared by scoring and by the legality term, so the two can
// never disagree about where a move lives in the table.
inline size_t policy_row(const PolicyNet& net, int pt, int from, int to, bool flip) {
  if (flip) { from ^= 56; to ^= 56; }
  size_t row = (size_t)(from * 64 + to);
  if (net.piece_indexed) {
    if (pt < 1) pt = 1; else if (pt > 6) pt = 6;
    row += (size_t)(pt - 1) * 64 * 64;
  }
  return row;
}

// THE LEGALITY TERM. Adds Wl * (mean embedding row over the legal moves) to ctx, which is what
// the trainer's LEGAL_BIAS=1 computes:
//
//     score(i) = dot(ctx + Wl * mean_{j legal} emb[j], emb[i])
//
// It is NOT a constant offset, which is the mistake that made me want to delete it. Expanding the
// second term, move i picks up dot(Wl * mean_j emb[j], emb[i]) -- a learned interaction between
// each move and the SET of moves that happen to be available, so it reorders them. Measured on a
// matched pair: +0.97 Top-4 and +1.19 Top-6 for 16,384 numbers, 2% of the model.
//
// Must be applied ONCE, after policy_context() and before any move is scored, with the complete
// legal move list. A partial list changes the mean and therefore every score.
//
// Cost: one row add per legal move (about 30 x h2) plus one h2 x h2 matrix-vector product. Against
// the trunk's ~74K operations that is small, and nothing is added per move at scoring time.
inline void policy_apply_legal_bias(const PolicyNet& net, float* ctx,
                                    const size_t* rows, int n) {
  if (!net.has_wl || n <= 0) return;
  const int H2 = net.h2;
  float* m = (float*)alloca(sizeof(float) * H2);
  std::memset(m, 0, sizeof(float) * H2);
  for (int j = 0; j < n; ++j) {
    const float* e = &net.emb[rows[j] * H2];
    for (int k = 0; k < H2; ++k) m[k] += e[k];
  }
  const float inv = 1.0f / (float)n;
  for (int k = 0; k < H2; ++k) m[k] *= inv;
  // ctx += m @ Wl, with Wl stored row-major [h2][h2] exactly as the trainer exported it.
  for (int h = 0; h < H2; ++h) {
    const float mh = m[h];
    if (mh == 0.0f) continue;
    const float* w = &net.Wl[(size_t)h * H2];
    for (int k = 0; k < H2; ++k) ctx[k] += mh * w[k];
  }
}

inline float policy_score(const PolicyNet& net, const float* ctx, int pt, int from, int to, bool flip) {
  if (flip) { from ^= 56; to ^= 56; }
  size_t row = (size_t)(from * 64 + to);
  if (net.piece_indexed) {
    //CLAMP RATHER THAN TRUST. Every caller reads the mailbox of a generated move's source square,
    //so pt is 1..6 by construction -- but the `& 7` idiom they use also produces 0 from PieceWhite
    //and PieceBlack and 7 from PieceNone, and pt=0 would make (pt-1)*4096 a near-maximal unsigned
    //offset while pt=7 lands exactly one row past the end. Both are silent out-of-bounds reads of a
    //multi-megabyte table. The trainer already refuses these values in policy_index(); this is the
    //same guard on the inference side.
    if (pt < 1) pt = 1; else if (pt > 6) pt = 6;
    row += (size_t)(pt - 1) * 64 * 64;
  }
  const float* e = &net.emb[row * net.h2];
  float s = 0.0f;
  for (int k = 0; k < net.h2; ++k) s += ctx[k] * e[k];
  return s;
}

#endif
