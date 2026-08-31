#include <stdlib.h>
#include <stdio.h>
#include "magic_bitboards.h"
#include "libchess.h"

// Generate attack mask for rook
static uint64_t get_rook_attack_mask(int square) {
  uint64_t mask = 0;
  int rank = square / 8;
  int file = square % 8;
  // d-file (except square and edges)
  for (int r = 1; r < 7; r++) {
      if (r != rank) mask |= (1ULL << (r * 8 + file));
  }
  // 4th rank (except square and edges)
  for (int f = 1; f < 7; f++) {
      if (f != file) mask |= (1ULL << (rank * 8 + f));
  }
  return mask;
}

// Generate attack mask for bishop
static uint64_t get_bishop_attack_mask(int square) {
  uint64_t mask = 0;
  int rank = square / 8;
  int file = square % 8;
  // Diagonals (excluding edges)
  for (int r = 1; r < 7; r++) {
    for (int f = 1; f < 7; f++) {
      if (abs(r - rank) == abs(f - file) && (r != rank || f != file)) {
        mask |= (1ULL << (r * 8 + f));
      }
    }
  }
  return mask;
}

// Reference function for rook moves (linear approach)
static uint64_t get_rook_moves_reference(int square, uint64_t occupancy) {
  uint64_t moves = 0;
  int directions[4] = {8, -8, 1, -1}; // North, South, East, West
  int rank = square / 8;
  int file = square % 8;
  for (int d = 0; d < 4; d++) {
    int step = directions[d];
    int s = square;
    while (1) {
      s += step;
      if (s < 0 || s >= 64) break;
      int s_rank = s / 8;
      int s_file = s % 8;
      if ((step == 1 || step == -1) && s_rank != rank) break;
      if ((step == 8 || step == -8) && s_file != file) break;
      moves |= (1ULL << s);
      if (occupancy & (1ULL << s)) break;
    }
  }
  return moves;
}

// Reference function for bishop moves
static uint64_t get_bishop_moves_reference(int square, uint64_t occupancy) {
  uint64_t moves = 0;
  int directions[4] = {9, -9, 7, -7}; // Diagonals
  int rank = square / 8;
  int file = square % 8;
  for (int d = 0; d < 4; d++) {
    int step = directions[d];
    int s = square;
    while (1) {
      s += step;
      if (s < 0 || s >= 64) break;
      int s_rank = s / 8;
      int s_file = s % 8;
      if (abs(s_rank - rank) != abs(s_file - file)) break;
      moves |= (1ULL << s);
      if (occupancy & (1ULL << s)) break;
    }
  }
  return moves;
}

// Generate occupancy variations
static void generate_occupancy_variations(uint64_t mask, uint64_t *variations, int *count) {
  *count = 1 << bitCount(mask);
  uint64_t bits[64];
  int bit_count = 0;
  for (int i = 0; i < 64; i++) {
    if (mask & (1ULL << i)) bits[bit_count++] = i;
  }
  for (int i = 0; i < *count; i++) {
    uint64_t variation = 0;
    for (int j = 0; j < bit_count; j++) {
      if (i & (1 << j)) variation |= (1ULL << bits[j]);
    }
    variations[i] = variation;
  }
}

uint64_t LineThrough[64][64];

void initLineThrough() {
  for (Square sq1 = SquareA1; sq1 < SquareNone; ++sq1) {
    for (Square sq2 = SquareA1; sq2 < SquareNone; ++sq2) {
      LineThrough[sq1][sq2] = 0; // Default to 0 if not on the same line
      if (sq1 == sq2) continue;

      uint64_t bit1 = SQ_BIT(sq1);
      uint64_t bit2 = SQ_BIT(sq2);

      // Are they on a straight line (rank or file)?
      if (get_rook_moves(sq1, 0) & bit2) {
        LineThrough[sq1][sq2] = (get_rook_moves(sq1, 0) & get_rook_moves(sq2, 0)) | bit1 | bit2;
      }
      // Are they on a diagonal?
      else if (get_bishop_moves(sq1, 0) & bit2) {
        LineThrough[sq1][sq2] = (get_bishop_moves(sq1, 0) & get_bishop_moves(sq2, 0)) | bit1 | bit2;
      }
    }
  }
}

void init_magic_bitboards(void) {
  for (int square = 0; square < SQUARE_COUNT; square++) {
    // Initialize rook magics
    rook_magics[square].attack_mask = get_rook_attack_mask(square);
    rook_magics[square].magic_number = rook_magic_numbers[square];
    rook_magics[square].relevant_bits = rook_relevant_bits[square];
    int table_size = 1 << rook_magics[square].relevant_bits;
    rook_magics[square].move_table = (uint64_t *)malloc(table_size * sizeof(uint64_t));
    if (!rook_magics[square].move_table) {
      fprintf(stderr, "Failed to allocate rook move table for square %d\n", square);
      exit(1);
    }
    
    // Generate occupancy variations
    uint64_t variations[1 << MAX_OCCUPANCY_BITS];
    int variation_count;
    generate_occupancy_variations(rook_magics[square].attack_mask, variations, &variation_count);
    
    // Fill move table
    for (int i = 0; i < variation_count; i++) {
      uint64_t occupancy = variations[i];
      uint64_t moves = get_rook_moves_reference(square, occupancy);
      uint64_t index = ((occupancy & rook_magics[square].attack_mask) * rook_magics[square].magic_number) >> (64 - rook_magics[square].relevant_bits);
      rook_magics[square].move_table[index] = moves;
    }
    
    // Initialize bishop magics
    bishop_magics[square].attack_mask = get_bishop_attack_mask(square);
    bishop_magics[square].magic_number = bishop_magic_numbers[square];
    bishop_magics[square].relevant_bits = bishop_relevant_bits[square];
    table_size = 1 << bishop_magics[square].relevant_bits;
    bishop_magics[square].move_table = (uint64_t *)malloc(table_size * sizeof(uint64_t));
    if (!bishop_magics[square].move_table) {
      fprintf(stderr, "Failed to allocate bishop move table for square %d\n", square);
      exit(1);
    }
    
    generate_occupancy_variations(bishop_magics[square].attack_mask, variations, &variation_count);
    for (int i = 0; i < variation_count; i++) {
      uint64_t occupancy = variations[i];
      uint64_t moves = get_bishop_moves_reference(square, occupancy);
      uint64_t index = ((occupancy & bishop_magics[square].attack_mask) * bishop_magics[square].magic_number) >> (64 - bishop_magics[square].relevant_bits);
      bishop_magics[square].move_table[index] = moves;
    }
  }
  initLineThrough();
}

void cleanup_magic_bitboards(void) {
  for (int square = 0; square < SQUARE_COUNT; square++) {
    free(rook_magics[square].move_table);
    free(bishop_magics[square].move_table);
  }
}

uint64_t get_rook_moves(int square, uint64_t occupancy) {
  struct MagicEntry * magic = &rook_magics[square];
  uint64_t index = ((occupancy & magic->attack_mask) * magic->magic_number) >> (64 - magic->relevant_bits);
  return magic->move_table[index];
}

uint64_t get_bishop_moves(int square, uint64_t occupancy) {
  struct MagicEntry * magic = &bishop_magics[square];
  uint64_t index = ((occupancy & magic->attack_mask) * magic->magic_number) >> (64 - magic->relevant_bits);
  return magic->move_table[index];
}
