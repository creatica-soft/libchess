#include <assert.h>
#include <errno.h>
#include <ctype.h>
//#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "libchess.h"

#ifdef _MSC_VER
#include <intrin.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// <summary>
/// Count the number of bits set to 1 in a unsigned long long, probably the best substitution to POPCNT processor instruction 
/// </summary>
/// <returns>The number of bits set to one</returns>
unsigned long long bitCount(unsigned long long value) {
#ifdef _MSC_VER
	return __popcnt64(value); // equivalent to __builtin_popcountl
#else
  return __builtin_popcountll(value);
#endif
  /*
	unsigned long long result = value - ((value >> 1) & 0x5555555555555555UL);
	result = (result & 0x3333333333333333UL) + ((result >> 2) & 0x3333333333333333UL);
	return (unsigned char)(((result + (result >> 4)) & 0xF0F0F0F0F0F0F0FUL) * 0x101010101010101UL >> 56);
	*/
}
/*
const unsigned long long magic = 0x37E84A99DAE458F;
unsigned char magicTable[] = {
	0, 1, 17, 2, 18, 50, 3, 57,
	47, 19, 22, 51, 29, 4, 33, 58,
	15, 48, 20, 27, 25, 23, 52, 41,
	54, 30, 38, 5, 43, 34, 59, 8,
	63, 16, 49, 56, 46, 21, 28, 32,
	14, 26, 24, 40, 53, 37, 42, 7,
	62, 55, 45, 31, 13, 39, 36, 6,
	61, 44, 12, 35, 60, 11, 10, 9,
};*/
/// <summary>
/// Gets the least significant bit.
/// LSbit seems to be twice as fast as MSbit
/// </summary>
/// <param name="b">64-bit unsigned positive integer</param>
/// <returns>Zero-based least significant bit, or 64 for zero argument</returns>
unsigned long lsBit(unsigned long long b) {
	if (b == 0) return SquareNone;
#ifdef _MSC_VER
	unsigned long index;
	_BitScanForward64(&index, b); // equivalent to __builtin_ctzl
	return index;
#else
	return __builtin_ctzll(b);
#endif
	// there is no difference in performance between gcc built-in function
	// and the table lookup (at least on armv8 it is not a factor)
	//return (enum SquareName)__builtin_ctzl(b);
	//return (enum SquareName)magicTable[((unsigned long long)((long long)b & -(long long)b) * magic) >> 58];
}

/*
unsigned char genLSBit(unsigned long long b) {
	return (enum SquareName)magicTable[((unsigned long long)((long long)b & -(long long)b) * magic) >> 58];
}

// unpacks bits from unsigned 64-bit integer into a 8x8 bit array
// where LSB is in the left bottom corner and MSB is in the right top corner (like a chessboard)
void unpack_bits(unsigned long long number, float * bit_array) {
    for (int row = 0; row < 8; row++) {
        // Extract the byte for the current row
        unsigned char byte = (number >> ((7 - row) << 3)) & 0xFF;
        for (int column = 0; column < 8; column++) {
            // Extract the bit at 'column' position in the byte
            bit_array[row * 8 + column] = (float)((byte >> column) & 1);
        }
    }
}*/
#ifdef __cplusplus
}
#endif
