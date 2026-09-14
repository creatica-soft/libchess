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

// The spatial term's weights and shape, read from a format 7/8 file's spatial block. Defined here,
// ahead of PolicyNet, because a net carries one; what it computes is described at THE SPATIAL TERM.
struct PolicySpatial {
  int planes = 0, ch = 0, dim = 0;
  //How many 3x3 convolutions. The second is the expensive one -- its cost grows with the SQUARE of
  //the channel count, the first only linearly -- so this is the largest single lever on the term's
  //price. See the frontier table in ENGINE_SETTINGS.md.
  int layers = 2;
  //1: planes enter the convolution as 0/1 (the trainer's SPATIAL_SCALE). 0: as 0 or 1/127, which is
  //what the whole-input divide by 127 did before SPATIAL_SCALE existed.
  int scale = 1;
  //Where plane 12's legal-move-source marks go. 1 = at plane 12 + (piece_type-1), which is what the
  //trainer did under PIECE_INDEX before SPATIAL_FROM_PLANE_FIX; 0 = all in plane 12. Reproduced
  //exactly, because a model fed the planes it was not trained on is a different model.
  int layout = 0;
  std::vector<float> Wc1, bc1, Wc2, bc2, Wu, Wv;   // Wc1[ch][planes][9], Wc2[ch][ch][9], Wu/Wv[ch][dim]
  bool loaded = false;
};

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
  //THE SPATIAL TERM. Present in formats 7 and 8; spatial.loaded says whether this net has one.
  PolicySpatial spatial;
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
  //FORMATS 7 AND 8 are 2 and 5 followed by a spatial block -- see policy_load_spatial(). An engine
  //that predates them refuses them, which is the point: a spatial net read as a 2 or a 5 would run
  //without its spatial term and score every move differently from how it was trained.
  if (hdr[1] < 1 || hdr[1] > 8 || hdr[1] == 3 || hdr[1] == 6) {
    snprintf(err, errlen, "%s: policy format version %d is not supported by this engine. "
                          "1/4 = base tensors, 2/5 = with the legality term, 7/8 = with the "
                          "legality and spatial terms; 3 and 6 use the untied Cl table, which is "
                          "not implemented",
             path, hdr[1]);
    std::fclose(f); return false;
  }
  const bool spatial = (hdr[1] == 7 || hdr[1] == 8);
  net.piece_indexed = (hdr[1] >= 4 && hdr[1] <= 6) || hdr[1] == 8;
  net.has_wl        = (hdr[1] == 2 || hdr[1] == 5 || spatial);
  net.spatial       = PolicySpatial{};
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
  //Wl comes after emb, so it reads straight on from it.
  if (ok && net.has_wl) ok = rd(net.Wl, (size_t)net.h2 * net.h2);
  if (!ok) { snprintf(err, errlen, "%s: truncated tensor data", path); std::fclose(f); return false; }
  //The spatial block follows Wl: an 8-int header, then Wc1, bc1, [Wc2, bc2], Wu, Wv.
  if (spatial) {
    int32_t sh[8];
    if (std::fread(sh, sizeof(int32_t), 8, f) != 8 || sh[0] != 0x54415053) {
      snprintf(err, errlen, "%s: format %d but no spatial block after Wl", path, hdr[1]);
      std::fclose(f); return false;
    }
    PolicySpatial& sp = net.spatial;
    sp.planes = sh[1]; sp.ch = sh[2]; sp.dim = sh[3]; sp.layers = sh[4];
    sp.scale  = sh[5]; sp.layout = sh[6];
    const int attack = sh[7];
    //Every bound here is a fixed-size buffer in PolicySpatialScratch or a plane layout the builder
    //implements. Refuse rather than overrun or silently build different planes.
    if (!((sp.planes == 14 && attack == 0) || (sp.planes == 26 && attack == 12))
        || sp.ch < 1 || sp.ch > 64 || sp.dim < 1 || sp.dim > 32
        || sp.layers < 1 || sp.layers > 2 || (sp.scale != 0 && sp.scale != 1)
        || (sp.layout != 0 && sp.layout != 1)) {
      snprintf(err, errlen, "%s: unsupported spatial block (planes %d, attack %d, channels %d, dim %d, "
                            "layers %d, scale %d, layout %d)", path, sp.planes, attack, sp.ch, sp.dim,
               sp.layers, sp.scale, sp.layout);
      std::fclose(f); return false;
    }
    ok = rd(sp.Wc1, (size_t)sp.ch * sp.planes * 9) && rd(sp.bc1, sp.ch);
    if (ok && sp.layers >= 2) ok = rd(sp.Wc2, (size_t)sp.ch * sp.ch * 9) && rd(sp.bc2, sp.ch);
    if (ok) ok = rd(sp.Wu, (size_t)sp.ch * sp.dim) && rd(sp.Wv, (size_t)sp.ch * sp.dim);
    if (!ok) { snprintf(err, errlen, "%s: truncated spatial block", path); std::fclose(f); return false; }
    sp.loaded = true;
  }
  //Nothing may follow: trailing bytes mean the file is not laid out the way its header says.
  {
    char extra;
    if (std::fread(&extra, 1, 1, f) == 1) {
      snprintf(err, errlen, "%s: unexpected data after the last tensor", path);
      std::fclose(f); return false;
    }
  }
  std::fclose(f);
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

// ---------------------------------------------------------------------------------------------
// THE SPATIAL TERM
//
// The trunk above scores a move purely from the position's NNUE embedding and the move's own
// identity row. It never sees the board as a board -- it cannot know that the destination square is
// defended twice, or that the piece standing next to the source is pinned. The spatial term adds
// exactly that, and nothing else:
//
//     score(m) += dot(u[from(m)], v[to(m)])
//
// where u and v are small per-square vectors computed from a stack of 8x8 binary planes by two
// 3x3 convolutions. The convolution is what makes it spatial: a square's vector depends on its
// neighbourhood, so "defended by the pawn one file over" is representable.
//
// The planes, in order, all oriented from the side to move's point of view (mirrored by ^56 for
// Black) exactly as policy_row() orients its squares:
//     0-5    own pawn..king
//     6-11   enemy pawn..king
//     12     squares a legal move starts from
//     13     squares a legal move lands on
//     14-25  squares attacked, by own pawn..king then enemy pawn..king   (SPATIAL_ATTACK == 12)
//
// Planes 14-25 are the ones the NNUE features cannot supply: NNUE's threat features only fire on
// occupied squares, so an attack on an EMPTY square -- which is most of what controls a position --
// is invisible to the trunk.
//
// Cost is dominated by the two convolutions: 26x32 and 32x32 channels, 3x3, over 64 squares, so
// about 1.07 million multiply-accumulates per position against roughly 0.3 million for the whole
// existing trunk. That ratio is the reason bench_policy_spatial exists; do not ship this on a
// parameter count.
//PolicySpatial is defined above PolicyNet, which carries one; see THE SPATIAL TERM below.

// The nine 3x3 taps as offsets into a 10x10 zero-padded board. Padding rather than bounds-checking
// keeps the innermost loop a contiguous run of 8 floats, which is what lets it vectorise.
static const int POLICY_TAP[9] = { -11, -10, -9, -1, 0, 1, 9, 10, 11 };

// Build the planes for one position, EXACTLY as the trainer's get() builds them. For each of the
// n legal moves, from[i] and to[i] are ORIENTED squares (mirrored by ^56 for Black) and pt[i] the
// moving piece's type, Pawn=1..King=6 -- eval_and_expand() has all three in hand, so this needs no
// second move generation. Duplicates, such as the four promotions of one pawn move, are harmless:
// every write sets a cell to 1. `pl` must have room for planes*64 bytes.
inline void policy_build_planes(const Board& board, const PolicySpatial& sp,
                                const int* from, const int* to, const int* pt, int n_legal,
                                uint8_t* pl) {
  const bool is_black = (board.sideToMove == ColorBlack);
  std::memset(pl, 0, (size_t)sp.planes * 64);
  for (int sq = 0; sq < 64; ++sq) {
    const int pc = board.piecesOnSquares[sq];
    const int ty = pc & 7;
    if (ty < Pawn || ty > King) continue;
    const bool own = ((pc >> 3) & 1) == (int)board.sideToMove;
    pl[((own ? 0 : 6) + (ty - Pawn)) * 64 + (is_black ? (sq ^ 56) : sq)] = 1;
  }
  for (int i = 0; i < n_legal; ++i) {
    //Layout 1 reproduces the trainer writing pl[12*64 + (legal_idx >> 6)] with a piece-indexed
    //legal_idx, which puts the mark in plane 12 + (pt-1). See SPATIAL_FROM_PLANE_FIX there.
    int src_plane = 12;
    if (sp.layout == 1) {
      const int p = pt[i] < 1 ? 1 : (pt[i] > 6 ? 6 : pt[i]);
      src_plane = 12 + (p - 1);
    }
    pl[src_plane * 64 + from[i]] = 1;
    pl[13 * 64 + to[i]] = 1;
  }
  if (sp.planes <= 14) return;
  const uint64_t occ = board.side[ColorWhite] | board.side[ColorBlack];
  for (int c = 0; c < 2; ++c) {
    const bool own = ((Color)c == board.sideToMove);
    for (PieceType pt = Pawn; pt <= King; pt = (PieceType)(pt + 1)) {
      uint64_t bb = board.side[c] & board.pieceTypes[pt - 1];
      uint64_t att = 0;
      if (pt == Pawn) {
        att = (c == ColorWhite) ? (((bb & ~files_bb[7]) << 9) | ((bb & ~files_bb[0]) << 7))
                                : (((bb & ~files_bb[0]) >> 9) | ((bb & ~files_bb[7]) >> 7));
      } else {
        while (bb) {
          const int sq = lsBit(bb); bb &= bb - 1;
          switch (pt) {
            case Knight: att |= Stockfish::attacks_bb<Stockfish::KNIGHT>((Stockfish::Square)sq); break;
            case Bishop: att |= Stockfish::attacks_bb<Stockfish::BISHOP>((Stockfish::Square)sq, occ); break;
            case Rook:   att |= Stockfish::attacks_bb<Stockfish::ROOK>((Stockfish::Square)sq, occ); break;
            case Queen:  att |= Stockfish::attacks_bb<Stockfish::BISHOP>((Stockfish::Square)sq, occ)
                              | Stockfish::attacks_bb<Stockfish::ROOK>((Stockfish::Square)sq, occ); break;
            default:     att |= Stockfish::attacks_bb<Stockfish::KING>((Stockfish::Square)sq); break;
          }
        }
      }
      const int plane = 14 + (own ? 0 : 6) + (pt - Pawn);
      while (att) {
        const int sq = lsBit(att); att &= att - 1;
        pl[plane * 64 + (is_black ? (sq ^ 56) : sq)] = 1;
      }
    }
  }
}

// Scratch for one forward. One per search thread; about 14 KB, so give it a thread_local rather
// than putting it on the simulation's stack.
struct PolicySpatialScratch {
  float pad_in[26 * 100];      // zero-padded planes,  [planes][10][10]
  float pad_mid[64 * 100];     // zero-padded conv1 out, [ch][10][10]
  float u[64 * 32];            // [64][dim]
  float v[64 * 32];
};

// planes -> u, v. The loop order is (out channel, in channel, tap, row) with the 8 columns of a row
// innermost and contiguous, so each innermost statement is one scalar times eight adjacent floats.
template <int L>
inline void policy_spatial_forward_impl(const PolicySpatial& sp, const uint8_t* pl,
                                        PolicySpatialScratch& s) {
  //HOIST EVERY FIELD OF sp BEFORE THE LOOPS. Reading sp.layers from inside them cost 25% at 32
  //channels: the scratch is written through pointers, so the compiler cannot prove the load is
  //loop-invariant, and it reloads it and gives up on vectorising the inner row.
  const int P = sp.planes, C = sp.ch, D = sp.dim;
  //scale 1: the trainer multiplied the plane slice back by 127, so planes arrive as 0/1.
  const float in_scale = sp.scale ? 1.0f : (1.0f / 127.0f);
  //The convolution stack's output, [C][64]. A LOCAL array rather than a member of the scratch:
  //routing it through s.h2 -- the same struct the convolution reads its input from -- cost 25% at
  //32 channels, because the compiler could no longer assume the two do not alias. 16 KB of stack
  //is cheaper than that.
  float h2[64 * 64];
  // Plane bytes into the padded float buffer.
  // Clearing all 100 cells per plane rewrites the 64 interior ones the loop below overwrites
  // anyway, which LOOKS wasteful -- but zeroing only the border measured 26% SLOWER at 32 channels.
  // One contiguous memset vectorises; a border walk is a handful of short scalar stores that do not.
  std::memset(s.pad_in, 0, sizeof(float) * (size_t)P * 100);
  for (int i = 0; i < P; ++i) {
    const uint8_t* src = pl + i * 64;
    float* dst = s.pad_in + (size_t)i * 100;
    for (int r = 0; r < 8; ++r)
      for (int c = 0; c < 8; ++c) dst[(r + 1) * 10 + c + 1] = (float)src[r * 8 + c] * in_scale;
  }
  // conv1: [P,8,8] -> [C,8,8], relu, written straight into the padded buffer conv2 will read.
  // With one layer there is no such buffer and no reason to clear it -- at 32 channels that memset
  // is 12.8 KB of pointless zeroing per expansion, which is why the one-layer variant first
  // measured SLOWER than the two-layer one at small channel counts.
  if (L >= 2) std::memset(s.pad_mid, 0, sizeof(float) * (size_t)C * 100);
  for (int o = 0; o < C; ++o) {
    float acc[64];
    const float b = sp.bc1[o];
    for (int q = 0; q < 64; ++q) acc[q] = b;
    const float* w = &sp.Wc1[(size_t)o * P * 9];
    for (int i = 0; i < P; ++i) {
      const float* base = s.pad_in + (size_t)i * 100;
      for (int k = 0; k < 9; ++k) {
        const float wk = w[i * 9 + k];
        if (wk == 0.0f) continue;
        for (int r = 0; r < 8; ++r) {
          const float* src = base + (r + 1) * 10 + 1 + POLICY_TAP[k];
          float* a = acc + r * 8;
          for (int c = 0; c < 8; ++c) a[c] += wk * src[c];
        }
      }
    }
    //With one layer there is no second convolution to read a padded buffer, so relu straight into
    //the [C][64] form the u/v projection wants.
    if (L < 2) {
      float* h1 = h2 + (size_t)o * 64;
      for (int q = 0; q < 64; ++q) h1[q] = acc[q] > 0.0f ? acc[q] : 0.0f;
      continue;
    }
    float* dst = s.pad_mid + (size_t)o * 100;
    for (int r = 0; r < 8; ++r)
      for (int c = 0; c < 8; ++c) {
        const float x = acc[r * 8 + c];
        dst[(r + 1) * 10 + c + 1] = x > 0.0f ? x : 0.0f;
      }
  }
  // conv2 + relu, kept as [C][64] so the u/v projection below reads one channel at a time.
  //The layer count guards the WHOLE nest, not the loop condition: putting `L >= 2 &&` in the
  //for-condition hides the trip count from the compiler and it stops unrolling the nest.
  if (L >= 2)
  for (int o = 0; o < C; ++o) {
    float acc[64];
    const float b = sp.bc2[o];
    for (int q = 0; q < 64; ++q) acc[q] = b;
    const float* w = &sp.Wc2[(size_t)o * C * 9];
    for (int i = 0; i < C; ++i) {
      const float* base = s.pad_mid + (size_t)i * 100;
      for (int k = 0; k < 9; ++k) {
        const float wk = w[i * 9 + k];
        if (wk == 0.0f) continue;
        for (int r = 0; r < 8; ++r) {
          const float* src = base + (r + 1) * 10 + 1 + POLICY_TAP[k];
          float* a = acc + r * 8;
          for (int c = 0; c < 8; ++c) a[c] += wk * src[c];
        }
      }
    }
    float* dst = h2 + (size_t)o * 64;
    for (int q = 0; q < 64; ++q) dst[q] = acc[q] > 0.0f ? acc[q] : 0.0f;
  }
  // u = relu(h2^T @ Wu), v = h2^T @ Wv.  NO RELU ON v -- see the trainer's note: Wv starts at zero,
  // and relu's subgradient at zero is zero, so a relu there would leave v dead for ever.
  std::memset(s.u, 0, sizeof(float) * 64 * (size_t)D);
  std::memset(s.v, 0, sizeof(float) * 64 * (size_t)D);
  for (int c = 0; c < C; ++c) {
    const float* hc = h2 + (size_t)c * 64;
    const float* wu = &sp.Wu[(size_t)c * D];
    const float* wv = &sp.Wv[(size_t)c * D];
    for (int q = 0; q < 64; ++q) {
      const float hv = hc[q];
      if (hv == 0.0f) continue;          // relu leaves about half of h2 at exactly zero
      float* uq = s.u + (size_t)q * D;
      float* vq = s.v + (size_t)q * D;
      for (int d = 0; d < D; ++d) { uq[d] += hv * wu[d]; vq[d] += hv * wv[d]; }
    }
  }
  for (int q = 0; q < 64 * D; ++q) if (s.u[q] < 0.0f) s.u[q] = 0.0f;
}

// THE LAYER COUNT IS A TEMPLATE PARAMETER, not a field read at run time. Branching on sp.layers
// inside the convolution measured 62.7 microseconds per call at 32 channels against 49.1 with the
// count known at compile time -- a 28% penalty for one branch, because the compiler keeps both
// paths alive and stops specialising the loop nest around them. Dispatching once, here, costs a
// single predictable branch per expansion and gives each variant fully specialised code.
inline void policy_spatial_forward(const PolicySpatial& sp, const uint8_t* pl,
                                   PolicySpatialScratch& s) {
  if (sp.layers >= 2) policy_spatial_forward_impl<2>(sp, pl, s);
  else                policy_spatial_forward_impl<1>(sp, pl, s);
}

// One move's spatial contribution. `from` and `to` must already be oriented the same way
// policy_score() orients them, i.e. mirrored by ^56 when Black is to move.
inline float policy_spatial_score(const PolicySpatial& sp, const PolicySpatialScratch& s,
                                  int from, int to) {
  const int D = sp.dim;
  const float* u = s.u + (size_t)from * D;
  const float* v = s.v + (size_t)to * D;
  float acc = 0.0f;
  for (int d = 0; d < D; ++d) acc += u[d] * v[d];
  return acc;
}

// `sp` is the spatial forward's output for this position, and is REQUIRED when the net has a spatial
// term: scoring such a net without it would rank moves by a different function from the trained one.
inline float policy_score(const PolicyNet& net, const float* ctx, int pt, int from, int to, bool flip,
                          const PolicySpatialScratch* sp) {
  if (flip) { from ^= 56; to ^= 56; }
  float spatial_term = 0.0f;
  if (net.spatial.loaded) {
    if (!sp) {
      std::fprintf(stderr, "FATAL: policy_score() called without the spatial buffer on a net that has "
                           "a spatial term; that would score moves by a different function\n");
      std::abort();
    }
    spatial_term = policy_spatial_score(net.spatial, *sp, from, to);
  }
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
  float s = spatial_term;
  for (int k = 0; k < net.h2; ++k) s += ctx[k] * e[k];
  return s;
}

// The pre-spatial signature, for tools that never load a spatial net. It aborts on one rather than
// silently dropping the term -- a missed call site must fail loudly, not score a different model.
inline float policy_score(const PolicyNet& net, const float* ctx, int pt, int from, int to, bool flip) {
  return policy_score(net, ctx, pt, from, to, flip, nullptr);
}

#endif
