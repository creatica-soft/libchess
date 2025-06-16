#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

unsigned int leftrotate(unsigned int F, unsigned int s) {
    return (F << s) | (F >> (32 - s));
}

//For chess I just need 64-bit unsigned int as the output
unsigned long md5(char * msg) {
  //All variables are unsigned 32 bit and wrap modulo 2^32 when calculating 
  unsigned int s[64], K[64];
  // s specifies the per-round shift amounts
  s[0] = 7;
  s[1] = 12;
  s[2] = 17;
  s[3] = 22;
  s[4] = 7;
  s[5] = 12;
  s[6] = 17;
  s[7] = 22;
  s[8] = 7;
  s[9] = 12;
  s[10] = 17;
  s[11] = 22;
  s[12] = 7;
  s[13] = 12;
  s[14] = 17;
  s[15] = 22;
  s[16] = 5;
  s[17] = 9;
  s[18] = 14;
  s[19] = 20;
  s[20] = 5;
  s[21] = 9;
  s[22] = 14;
  s[23] = 20;
  s[24] = 5;
  s[25] = 9;
  s[26] = 14;
  s[27] = 20;
  s[28] = 5;
  s[29] = 9;
  s[30] = 14;
  s[31] = 20;
  s[32] = 4;
  s[33] = 11;
  s[34] = 16;
  s[35] = 23;
  s[36] = 4;
  s[37] = 11;
  s[38] = 16;
  s[39] = 23;
  s[40] = 4;
  s[41] = 11;
  s[42] = 16;
  s[43] = 23;
  s[44] = 4;
  s[45] = 11;
  s[46] = 16;
  s[47] = 23;
  s[48] = 6;
  s[49] = 10;
  s[50] = 15;
  s[51] = 21;
  s[52] = 6;
  s[53] = 10;
  s[54] = 15;
  s[55] = 21;
  s[56] = 6;
  s[57] = 10;
  s[58] = 15;
  s[59] = 21;
  s[60] = 6;
  s[61] = 10;
  s[62] = 15;
  s[63] = 21;
  
  K[0] = 0xd76aa478;
  K[1] = 0xe8c7b756;
  K[2] = 0x242070db;
  K[3] = 0xc1bdceee;
  K[4] = 0xf57c0faf;
  K[5] = 0x4787c62a;
  K[6] = 0xa8304613;
  K[7] = 0xfd469501;
  K[8] = 0x698098d8;
  K[9] = 0x8b44f7af;
  K[10] = 0xffff5bb1;
  K[11] = 0x895cd7be;
  K[12] = 0x6b901122;
  K[13] = 0xfd987193;
  K[14] = 0xa679438e;
  K[15] = 0x49b40821;
  K[16] = 0xf61e2562;
  K[17] = 0xc040b340;
  K[18] = 0x265e5a51;
  K[19] = 0xe9b6c7aa;
  K[20] = 0xd62f105d;
  K[21] = 0x02441453;
  K[22] = 0xd8a1e681;
  K[23] = 0xe7d3fbc8;
  K[24] = 0x21e1cde6;
  K[25] = 0xc33707d6;
  K[26] = 0xf4d50d87;
  K[27] = 0x455a14ed;
  K[28] = 0xa9e3e905;
  K[29] = 0xfcefa3f8;
  K[30] = 0x676f02d9;
  K[31] = 0x8d2a4c8a;
  K[32] = 0xfffa3942;
  K[33] = 0x8771f681;
  K[34] = 0x6d9d6122;
  K[35] = 0xfde5380c;
  K[36] = 0xa4beea44;
  K[37] = 0x4bdecfa9;
  K[38] = 0xf6bb4b60;
  K[39] = 0xbebfbc70;
  K[40] = 0x289b7ec6;
  K[41] = 0xeaa127fa;
  K[42] = 0xd4ef3085;
  K[43] = 0x04881d05;
  K[44] = 0xd9d4d039;
  K[45] = 0xe6db99e5;
  K[46] = 0x1fa27cf8;
  K[47] = 0xc4ac5665;
  K[48] = 0xf4292244;
  K[49] = 0x432aff97;
  K[50] = 0xab9423a7;
  K[51] = 0xfc93a039;
  K[52] = 0x655b59c3;
  K[53] = 0x8f0ccc92;
  K[54] = 0xffeff47d;
  K[55] = 0x85845dd1;
  K[56] = 0x6fa87e4f;
  K[57] = 0xfe2ce6e0;
  K[58] = 0xa3014314;
  K[59] = 0x4e0811a1;
  K[60] = 0xf7537e82;
  K[61] = 0xbd3af235;
  K[62] = 0x2ad7d2bb;
  K[63] = 0xeb86d391;
  
/* 
  // the above K[64] array is computed by this formula 
  unsigned int K2[64];
  for (int i = 0; i <= 63; i++) {
    K2[i] = floor(4294967296 * fabs(sin(i + 1)));
    if (K[i] != K2[i]) printf("K = %x but K2 = %x\n", K[i], K2[i]);
  }
*/
  
  // Initialize variables:
  unsigned int a0 = 0x67452301;   // A
  unsigned int b0 = 0xefcdab89;   // B
  unsigned int c0 = 0x98badcfe;   // C
  unsigned int d0 = 0x10325476;   // D

  // Pre-processing: adding a single 1 bit
  // append "1" bit to message    
  // Notice: the input bytes are considered as bit strings,
  //  where the first bit is the most significant bit of the byte.[53]

  // Pre-processing: padding with zeros
  //append "0" bit until message length in bits ≡ 448 (mod 512)

  // Notice: the two padding steps above are implemented in a simpler way
  // in implementations that only work with complete bytes: append 0x80
  // and pad with 0x00 bytes so that the message length in bytes ≡ 56 (mod 64).
  size_t len = strlen(msg);
  size_t lenBits = len * 8;
  unsigned int padding = len % 64;
  if (padding < 56)
    padding = 56 - padding;
  else
    padding += 120 - padding;
  size_t total_len = len + padding + 8;
  size_t chunk_num = total_len / 64;
  unsigned char * m = (unsigned char *)malloc(total_len);
  memset(m, 0, total_len);
  memcpy(m, msg, len);
  
  // Pre-processing: adding a single 1 bit
  // append "1" bit to message<    
  // Notice: the input bytes are considered as bit strings,
  // where the first bit is the most significant bit of the byte.[53]

  // Pre-processing: padding with zeros
  // append "0" bit until message length in bits ≡ 448 (mod 512)
  
  // Notice: the two padding steps above are implemented in a simpler way
  // in implementations that only work with complete bytes: append 0x80
  // and pad with 0x00 bytes so that the message length in bytes ≡ 56 (mod 64).
  //printf("unsigned char %x\n", (unsigned char)0x80);
  memset(m + len, 0x80, 1);
  memset(m + len + 1, 0, padding - 1);
  
  //append original length in bits mod 2^64 to message
  memcpy(m + len + padding, &lenBits, 8);
  
  unsigned char * M = (unsigned char *)malloc(16 * 4);
  unsigned int A, B, C, D, F, g, X;
  
  // Process the message in successive 64-byte chunks:
  for (int k = 0; k < chunk_num; k++) {
    //break chunk into 16 4-byte words M[j], 0 ≤ j ≤ 15
    memset(M, 0, 64);
    unsigned int Mj = 0;
    for (unsigned int j = 0; j < 16; j++)
      memcpy(M + j * 4, m + k * 64 + j * 4, 4);
    for (unsigned int j = 0; j < 16; j++)
      memcpy(&Mj, M + j * 4, 4);
      
    // Initialize hash value for this chunk:
    A = a0;
    B = b0;
    C = c0;
    D = d0;
    
    // Main loop:
    for (unsigned int i = 0; i <= 63; i++) {
        if (i >= 0 && i <= 15) {
          F = (B & C) | ((~B) & D);
          g = i;
        } else if (i >= 16 && i <= 31) {
            F = (B & D) | (C & (~D));
            g = (5 * i + 1) % 16;
        } else if (i >= 32 && i <= 47) {
            F = B ^ C ^ D;
            g = (3 * i + 5) % 16;
        } else if (i >= 48 && i <= 63) {
            F = C ^ (B | (~D));
            g = (7 * i) % 16;
        }

        unsigned int Mg = 0;
        memcpy(&Mg, M + 4 * g, 4);
        A = A + F + Mg + K[i];  // M[g] must be a 32-bit block
        A = B + leftrotate(A, s[i]);
        
        X = D;
        D = C;
        C = B;
        B = A;        
        A = X;
    }
    // Add this chunk's hash to result so far:
    a0 += A;
    b0 += B;
    c0 += C;
    d0 += D;
  }
  free(m);
  free(M);
  
  // (Output is in little-endian)
  d0 = (d0 << 24) & 0xff000000 | (d0 << 8) & 0xff0000 | (d0 >> 8) & 0xff00 | (d0 >> 24);
  c0 = (c0 << 24) & 0xff000000 | (c0 << 8) & 0xff0000 | (c0 >> 8) & 0xff00 | (c0 >> 24);
  b0 = (b0 << 24) & 0xff000000 | (b0 << 8) & 0xff0000 | (b0 >> 8) & 0xff00 | (b0 >> 24);
  a0 = (a0 << 24) & 0xff000000 | (a0 << 8) & 0xff0000 | (a0 >> 8) & 0xff00 | (a0 >> 24);

  unsigned long digestH, digestL;
  digestH = (unsigned long)a0 << 32 | (unsigned long)b0;
  digestL = (unsigned long)c0 << 32 | (unsigned long)d0; 
  
  //The below is the original output of MD5
  //sprintf(res, "%lx%lx", digestH, digestL);
  //return res;
  
  //For chess I need just 64-bit unsigned int
  return digestH ^ digestL;
}
/*
int main(int argc, char ** argv) {
  char res[129];
  if (argc == 2) 
    printf("%s\n", md5(argv[1], res));
  return 0;
}
*/

#ifdef __cplusplus
}
#endif
