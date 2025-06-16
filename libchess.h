/// 
/// c++ -std=c++17 -shared -Wno-deprecated -Wno-writable-strings -Wno-deprecated-declarations -Wno-strncat-size -O3 -Xclang -fopenmp -Wl,-dylib,-lsqlite3,-lomp,-rpath,/opt/anaconda3/lib -I /opt/anaconda3/include -L/opt/anaconda3/lib -o libchess.so bitscanner.c board.c engine.c fen.c game.c game_omp.c move.c piece.c square.c tag.c zobrist-hash.c sqlite.c my_md5.c magic_bitboards.c boards_legal_moves8.c nnue/nnue/network.cpp nnue/nnue/nnue_accumulator.cpp nnue/nnue/nnue_misc.cpp nnue/nnue/features/half_ka_v2_hm.cpp nnue/bitboard.cpp nnue/evaluate.cpp nnue/memory.cpp nnue/misc.cpp nnue/nnue.cpp nnue/position.cpp
///
/// To compile on MacOS M1 using clang , run:
/// cc -c -Wno-strncat-size -O3 -I /opt/anaconda3/include bitscanner.c board.c engine.c fen.c game.c game_omp.c move.c piece.c square.c tag.c zobrist-hash.c sqlite.c my_md5.c magic_bitboards.c boards_legal_moves8.c

//to integrate with Lichess NNUE,  compile C++ files in nnue folder separately
//c++ -Wno-writable-strings -c -std=c++17 -O3 nnue/network.cpp nnue/nnue_accumulator.cpp nnue/features/half_ka_v2_hm.cpp bitboard.cpp evaluate.cpp memory.cpp misc.cpp nnue.cpp position.cpp

//finally, link all object files into libchess.so
//c++ -shared -Wno-strncat-size -O3 -Xclang -fopenmp -Wl,-dylib,-lsqlite3,-lomp,-rpath,/opt/anaconda3/lib -L/opt/anaconda3/lib -o libchess.so bitscanner.o board.o engine.o fen.o game.o game_omp.o move.o piece.o square.o tag.o zobrist-hash.o sqlite.o my_md5.o magic_bitboards.o boards_legal_moves8.o nnue/bitboard.o nnue/evaluate.o nnue/half_ka_v2_hm.o nnue/memory.o nnue/misc.o nnue/network.o nnue/nnue.o nnue/nnue_accumulator.o  nnue/position.o

/// DON'T FORGET to init and free magic bitboards by calling init_magic_bitboards() and cleanup_magic_bitboards()
/// use -g for debugging with lldb instead of -O3 (lldb ./test, then run, then bt)
/// To compile on alpine linux, run:
/// gcc -O3 -fopenmp -fPIC -I /usr/lib/gcc/x86_64-alpine-linux-musl/14.2.0/include -shared -Wl,-rpath,/home/apoliakevitch/libchess -o libchess.so bitscanner.c board.c engine.c fen.c game.c game_omp.c move.c piece.c square.c tag.c zobrist-hash.c sqlite.c my_md5.c magic_bitboards.c boards_legal_moves6.c -lgomp -lsqlite3
/// To build python bindings, use:
/// conda install cffi
/// cc -E libchess.h > libchess.ph
/// vi tasks.py
/// python3.12 tasks.py (it should produce chess.cpython-312-darwin.so from libchess.so and libchess.ph)
/// python3.12 test.py (to test chess module stored in chess.cpython-312-darwin.so)
///

#include <stdbool.h>
//#include <stdio.h>

#ifndef LIBCHESS_H
#define LIBCHESS_H

#ifdef __cplusplus
extern "C" {
#endif

/// <summary>
/// Array of 841 unique true random unsigned 64-bit integers obtained from random.org
/// that uses atmospheric noise
/// </summary>
static const unsigned long bitStrings[841] = {
0x35840e4be496fdc9UL,
0x726adfa6fb848ccfUL,
0x8ebc36ddb5dad364UL,
0x725aa5ba532d681cUL,
0x8e5723c222eba83fUL,
0x675747cb0ca6d4bcUL,
0x61858ccc018f938fUL,
0x83d54185e5ce4624UL,
0x8fee932b383253b5UL,
0x9ca433b46e42eedUL,
0x506ae2b07759ee60UL,
0x5fdd05ac62d725dcUL,
0x2441c40ed458c250UL,
0x6dd9f06fb077cd17UL,
0xc3ad4742c43893a1UL,
0x9f74c524e7d16748UL,
0xf1b3499b6999f29UL,
0xca21260f62fc961dUL,
0x345238b4df3a58daUL,
0x39220b7748b567a0UL,
0x494f8b64f5d406f0UL,
0x33afff0b8389bb35UL,
0xb6b55dffc5f4a8adUL,
0x55d540e113eaf090UL,
0xf64d4836662e1364UL,
0x41de5700ddedc3b0UL,
0x8c059bd857d09f70UL,
0xcc9485a8bae7953bUL,
0x4b6a264e4b456ef2UL,
0x67464a64b6300dceUL,
0x479afd7eb52100b0UL,
0x4baa9c3d994fb9fUL,
0xd28a20c743fd4d69UL,
0xa9f1b7ad4d79fa72UL,
0x95365695f49fc4ceUL,
0x7bb46b8bf455d26aUL,
0x2a44f0b0bb62088eUL,
0x92f2812c5aa5d56bUL,
0xa018e855f2196cdaUL,
0xef763ffbbdbb4aebUL,
0xf614e5af230ee55UL,
0xa6781db7662418a2UL,
0x112e637b817f75f4UL,
0x3223aec1b49a69dcUL,
0xa148ba09d3ed8b79UL,
0x7e2c4c1cece2d7acUL,
0xa76acfe8fe0b2593UL,
0xf0a190108ab20d38UL,
0x784d2af7de7f22b7UL,
0x42bd3937d08ba367UL,
0x5826116c8fcb0d04UL,
0x14d1b867cddbe60fUL,
0x3aab8c9054bcdaccUL,
0x54f20620cb77df5cUL,
0x1d677ca63ceb95e4UL,
0x9a325771ad1b10a5UL,
0x13ab96f451f572f1UL,
0xc0aed2efffcf049dUL,
0x3ef0006a08de9183UL,
0xd13b50cc63ec40b3UL,
0xa96473ff0720c612UL,
0x9fc5d555d1bf03f0UL,
0x8fcb6ab8ca40bfeUL,
0x26ab8378164dde1bUL,
0xebf4593621d87635UL,
0x21e14754de4c1059UL,
0xa2e5c7726c254fb2UL,
0x2eb6fb3378724b5aUL,
0x6eaaa148740df89bUL,
0x586ff92f5ae98b52UL,
0xd1c34cdd195dbb9aUL,
0x1c5ceefdc5a078fbUL,
0x4addf6e9a80c874cUL,
0xb06f06da9d7b8a9cUL,
0xb8bdbe45ed1838d4UL,
0x7849e7c9b2c9f01cUL,
0x4901a6c6b517eb76UL,
0x388817fc3e2ec6efUL,
0x44913fec3674478bUL,
0x353ae91ce8a7512UL,
0xf787ae830ef1c7dUL,
0x9feeaad92dd42713UL,
0xff527846fede9439UL,
0x3adf19896c9f06f3UL,
0x4c3e1708dccee307UL,
0xbdd663f838c5ba8dUL,
0x7b9062797b22d719UL,
0xd1df9a0dc3321481UL,
0x9ce90c712dbca9ceUL,
0xbaa708fcbd47851dUL,
0x7fab15542662aea3UL,
0xa05d63cf7dec032eUL,
0x940ba59b97145ae3UL,
0x4b2351134288a6adUL,
0xa5f6f902f3532c65UL,
0xad818f549942d0e8UL,
0x5f0d3fafee193c60UL,
0xe365589cc658c95UL,
0xfc392d5491c6bb6UL,
0x427dfc73e1dcd8e1UL,
0xa5341f2c90c36cddUL,
0x13120e789daf365UL,
0xe75f5c374c15c62dUL,
0x68929da6c5c6e335UL,
0x3db8c8408b1470dbUL,
0xc4949eda3d028e57UL,
0xf4e806597fc3360aUL,
0x299ae737e391e9e7UL,
0x64b54ae229d17bfUL,
0x11e57ba3d59d4e20UL,
0x87a2fd1ae3f3f805UL,
0xc1598ccc66551a30UL,
0x16c5a2cab3f23090UL,
0xa0fa5c1907b1db63UL,
0x610da09c7050bd40UL,
0x56b91e8a0e53d106UL,
0xa3a7115ad4a86226UL,
0x7125d411714d8ad2UL,
0x53016c296e7212b1UL,
0x95d47f2e198fe677UL,
0xd72a546a0e884e08UL,
0x60681c9c36d9faf9UL,
0x8c630ed0ae476a2aUL,
0x41bb57028b8c52f3UL,
0xbc24957db1f6292aUL,
0xf46aad94973df65dUL,
0xfc66ac3ce3db0ba3UL,
0x284a2a13c4435634UL,
0xddf6c5a3b232a169UL,
0x494351cd1c89f13aUL,
0x2e08e48842763055UL,
0x554a7bc205376697UL,
0xed081657f7a8d0dbUL,
0x5ed378abc064a5e5UL,
0xcb8a3eb8aa3ed3c5UL,
0x170817c1d407d0fbUL,
0xc18f569e9980ce64UL,
0x2eebecccce642c70UL,
0x8c0b18eb0d174cf8UL,
0xa1fe71b9915d7924UL,
0x3e8ad8f6dad0918bUL,
0xa75b71cd6fad79dcUL,
0xff7286446b7096bdUL,
0x3d21b50076bbc83cUL,
0x249be0c9af465f92UL,
0xdfaaf87ce56de803UL,
0x1e97c62bc676823eUL,
0xbe55b78b1aca51ceUL,
0x922010e8aa17ba45UL,
0x68115d66237024b4UL,
0x5b1b9387b9bf47abUL,
0x791cdc8ac1f8f597UL,
0x59663453c0541416UL,
0xf2a26e750dd744e7UL,
0x3fd9e50130cad47eUL,
0x343e8c6a17d3aa60UL,
0x7192b46e8e31f9c0UL,
0x6b2943ab6f889fc7UL,
0xad4400aabe62c4f1UL,
0xc139ef44353705f1UL,
0xc7d583dd118aef04UL,
0xd317953689e263efUL,
0x73e98b61660176fdUL,
0xd4cc05a581e0b746UL,
0x4de3effbdaecfea2UL,
0x41981421584576b1UL,
0x4ab9b24191480b8fUL,
0xa8229e5f344d19daUL,
0x28acb25ead4d5ec4UL,
0xc6948c8d72e283fbUL,
0xb5cd04a5dc7414c6UL,
0xbc543753e65d5324UL,
0x8d1c4bb32f4e0d5eUL,
0x65af76148e6b58f6UL,
0x7cb269620d66c477UL,
0xb9e7fc20a51e30d4UL,
0xdf8cc1e53e086effUL,
0x1633e7ea2c36e83eUL,
0xe1287cd3a7a8a09UL,
0x6ad37c09d0264e61UL,
0xef9c35e5632104cbUL,
0xe78206679da95fa9UL,
0xa835b43101585db4UL,
0x36184a22fce95c9cUL,
0x7b937f56f8ee84a9UL,
0xfd893422dd52b322UL,
0x98f980a475b774bfUL,
0x5754d86e52a6defeUL,
0xce739d33823ce991UL,
0x6679a40d8ef0bec2UL,
0xb387b7f51f72fa1dUL,
0xf491824f625bfeb9UL,
0x95a178d927016c81UL,
0x3a98c4f07a262b63UL,
0xea8b55c8ff8ce401UL,
0xa2dfcf2381bf16eeUL,
0x4cc51064536a2d52UL,
0x314c3dc506d52db2UL,
0xa8a937160dacd916UL,
0x97a1d2278f6fae57UL,
0x90b7d520889d4871UL,
0x519fad2008a09f95UL,
0x33a74353149cb3b6UL,
0xe5a76a963382f9a6UL,
0xa1cff6cc6e4f5dddUL,
0x6edc15686ca7a2b1UL,
0xf620314d7124b04eUL,
0x4d6cc02a9090d718UL,
0x66cd9392f59b9ef4UL,
0x69ef3137693f068bUL,
0xe3655f7965eb0940UL,
0x8859c9770d8a2080UL,
0xf46cf5071caca680UL,
0x9acefcaa43aa1647UL,
0xd8a69777c563a689UL,
0xadee498eb259ddaUL,
0x9d5d421fb6a61dd5UL,
0x32d39e87c009d5d7UL,
0xdd922fc98d8b1b2bUL,
0x18850d5d03130efUL,
0x9bbb3e2b32776aceUL,
0x176faf52075afd85UL,
0x7c1ed1c675478f60UL,
0x1108b2cb675fad42UL,
0xb3213d0673629bc1UL,
0xc1412c9ebfd5726cUL,
0x9db7060f032efeb7UL,
0xc122be8ac79f07ffUL,
0xbb6abcc13abc4ddcUL,
0x3e36d9e6ad6161c7UL,
0xb393a95ea524618aUL,
0x5c2d9cd2a42fed7UL,
0xf28b8df90a7e720bUL,
0x49a197825a56f57eUL,
0xf3ab08fe25e6f990UL,
0x713940c42ebfb469UL,
0x666360dacf686088UL,
0x4a035bdde6e7abf7UL,
0x349804e0efe8febeUL,
0x554d205418ea61f6UL,
0xb53c0dc9508ccf50UL,
0xefe7c016a78f20deUL,
0x1743b34db04c04deUL,
0xc68225747792af75UL,
0xcc6662799629a6b5UL,
0xd7e848b18b3cd9dfUL,
0xff2c8a25e525708bUL,
0x7f13ee70b46a3b09UL,
0x907fa7230232c5ceUL,
0x7af346534089778UL,
0xbb8337ab7ecd6424UL,
0x50bf721092312eadUL,
0xcea6cc6fd2889cdaUL,
0xb751d5257f6a4831UL,
0x8f69fd454a73cb85UL,
0x858f8e15cb60038dUL,
0xf1e964def3472357UL,
0xc54c3f1381a2e218UL,
0xcba9aea6b0a75ed3UL,
0x1268b9424bd4a35fUL,
0x1229779effe284eaUL,
0x42294cdb313e5364UL,
0xbf07aa0ca09604eeUL,
0x3a0be08ef129c27dUL,
0x5dead1a526ddc6b6UL,
0xaef0ff57987d6ea8UL,
0xff228d3681751b87UL,
0xc486cc4b0c7a43c5UL,
0x15e808a4a8c9d1b7UL,
0xc4d421cea359f3b0UL,
0x301c2c2a261c59cfUL,
0x7c4a811f62abdddbUL,
0xabcea06ff2f5aa06UL,
0x7ffcb4bf55d1aecbUL,
0xab46675fa0e8c029UL,
0xedd3de77a0b47637UL,
0xcedddcc436286c7aUL,
0x9f36dc7e05e198fUL,
0x9f313870728fb8a1UL,
0xad90024bab0688aUL,
0xcde95ede6e502dacUL,
0xc74d70ce87142cdcUL,
0x8cfc625b7e11a509UL,
0xed8e51e7b84c3763UL,
0x6003ea95554e301aUL,
0x4de92773b048e46UL,
0xf7b335ce76a01e55UL,
0x4b3245656f473a82UL,
0x2ca1c5c62cc27319UL,
0x40bfd5d65f9a0d76UL,
0xb53b858d4a29973bUL,
0x52de6f699deb82ddUL,
0x5abb6fa132322504UL,
0x660b249fb9c3a73dUL,
0x1eb7b45f65f3b933UL,
0x3baa82e02794473fUL,
0x9f6241111876522aUL,
0xf1ac4083557a4f37UL,
0x7f1002990f35d1a0UL,
0x13ea4a5852b18236UL,
0x711cd79689f6ab54UL,
0xc1b116fa3a3f9a27UL,
0xcd0b58b41dfc4f43UL,
0x52d9d711fd25549cUL,
0xcf765fc64c433fadUL,
0xb0f58a3c11f69148UL,
0xe63819d24c43e3adUL,
0xa50a9507e6dc6582UL,
0x8a4597b64f585a1eUL,
0x8c09b6e9782c0171UL,
0xdf46b66e89d1ade6UL,
0x5189c8ce094ffde7UL,
0x321ce06e029044d5UL,
0xaf69cbde1fd4ad18UL,
0x6fdc08fab8186063UL,
0x89f02f8834bb690dUL,
0xbf74956f60c46ceaUL,
0x2b9f09d2daee847aUL,
0x7e5b1562b322b141UL,
0x124bd6eeb02b37f6UL,
0x3377505860342740UL,
0x47518dfec52acb3UL,
0x9389b9d9fd43f42bUL,
0x4bc9048f506287c1UL,
0xaee215e41fe15992UL,
0x94193a3dc2456066UL,
0xa2a65ac54dfc2f3dUL,
0x524b39c2cd8bd74UL,
0x6fb9af83a5f659b8UL,
0x844a3d785a722448UL,
0x6599f42b716c9bafUL,
0x65d6c38e8860c03aUL,
0x10105e7dc32154daUL,
0xe254696248d012cdUL,
0xc59c53caee488358UL,
0x83f6f1a8d4f70b8cUL,
0x2e85a007fa678805UL,
0xf9d5505b606a2b0aUL,
0x5e9492437589fd6aUL,
0xf0b942d9f6b31b5dUL,
0x273b10369de291f3UL,
0x8670d3dfc9f385ddUL,
0x4c6510c085c05178UL,
0x32587fc6dc63fa08UL,
0x2f81e75acff67229UL,
0x4ce57703f8c52279UL,
0xd233c4c6a3e16618UL,
0x13d50ad7e6285994UL,
0x8a68eff6cfdbe72aUL,
0xba315303fd5d1affUL,
0xaed37115dd71d5b0UL,
0xb540ec24b32b6c5cUL,
0xe538f62c9137edc7UL,
0xfd5fe2737201d922UL,
0xe301246c3606d120UL,
0xda2895d18b774327UL,
0xfe94218d5165aeb9UL,
0xd585de7782e0a28UL,
0x1ac42bf818ec3731UL,
0xe719075171995997UL,
0xc841e05d34f2dbd4UL,
0x37aca33d517b94aUL,
0x5b2af9574836d530UL,
0x2fb0cf82057668d6UL,
0xdf422cc02f59a004UL,
0x8f6cc8e0eb896713UL,
0x886cf4441c653caeUL,
0x95da8794b70aac7bUL,
0x5a13738ea2e51a87UL,
0x3cb695a54713f45UL,
0x565920893f5ebe85UL,
0xa74718b4e552d4f3UL,
0xe570ae8191b0148aUL,
0x1ae9ec32d326b069UL,
0xf002e96bd28c95bcUL,
0x2ebee4db2d41bdd2UL,
0x242e7937dd113bc9UL,
0x9ab75408c6af98dbUL,
0x3898a5f442f39a3dUL,
0x586e78d93d6818cUL,
0x627469061c9647e8UL,
0xed7ac789678902caUL,
0x26580b74e6e701d2UL,
0xf7972d419bcf6764UL,
0x52cdc57fcfff06f3UL,
0x1ec022435fc9d297UL,
0xfd0a9930b503f2d5UL,
0x8968e0ecbbe87f69UL,
0x9bf0017c5936d42UL,
0x495c114bf1253685UL,
0xba3cae382b4087bUL,
0x46e6f66cf58a55abUL,
0x9248edf39260ea89UL,
0x3f76c7d4acef3595UL,
0xf57bc2969b6b9f19UL,
0x7c3361cfd07474e4UL,
0xd3b74e75da9557eeUL,
0x2a0678534f728e4bUL,
0x4d28e9ff76539f4cUL,
0x2a9c04830c99054dUL,
0x612dd6748ab97ccbUL,
0x734974eaafdbe90eUL,
0x4e86d7a4b4346835UL,
0x6048b7c85db21d73UL,
0xf293429b16f0ab85UL,
0x402f9447af94d168UL,
0xcbca38c7ba6d86deUL,
0xe56d02c2c2982839UL,
0x1b6ad378985b4991UL,
0x2026c35b6c118cb9UL,
0xf4c3d4a2020eea38UL,
0xaa1b4e50b4ee88b2UL,
0x8551da353f8289faUL,
0xb4f164d12aee32c0UL,
0xdf9637637bac3a27UL,
0xb219a5d752da5ccaUL,
0x2b6ed36b23a5bab0UL,
0x3ac9a841767165f2UL,
0x8c783bc65d862477UL,
0xede9e83b97a0e69aUL,
0x98b0914901f92968UL,
0xa03e3f9d2c0fa3beUL,
0x473e3d50ca57b81eUL,
0x9e4b3697197e8b33UL,
0xd4e8c3f52163b810UL,
0x45faa3e8b4541d84UL,
0xe4eb5e0749cc35abUL,
0xd2062b30e89939fUL,
0x43ce3a929e4723fbUL,
0x33e19e3d1e1812c7UL,
0xef8512ab342daac7UL,
0xa96ed47df37b6204UL,
0x57bd77f72134e549UL,
0x976d8f3229d262beUL,
0x42df8534fd22ac81UL,
0x1c6e808819808270UL,
0xbbc17cdb146585b5UL,
0x38f5c30afa0c6bfaUL,
0xe2784d6e0499791dUL,
0xd360f681fb02fb17UL,
0x4914313cc6c36cd9UL,
0x4c60e61eb253b417UL,
0x675d1839e76d9371UL,
0xd6c7eeb904e04882UL,
0xac0d13e4561978c5UL,
0x252fd89a537deeedUL,
0xc50f24425c8d91baUL,
0x529048967b69a783UL,
0x4409d8e7bfaeebc2UL,
0x5727e621c498eba0UL,
0xaf51e47160cc27a9UL,
0xada62019f3f87132UL,
0xd4863dd6cc290479UL,
0x9f3480e31ca9392eUL,
0x5a8f1d4b83a2c3d7UL,
0x6bc818aab1c72662UL,
0x15ac1fc26b2976c2UL,
0x889f391adffd00aUL,
0xc380328fb901637eUL,
0x8f03cd8d0cc8b331UL,
0x200dec5a28c369c5UL,
0x8031e44efe118416UL,
0x136a38b593bcb4dfUL,
0x85db1fb74c0b5e4aUL,
0xe07083709b498ee2UL,
0x3de428ed0e3e831UL,
0x2e21b179dd7e38a2UL,
0x852093279285c4bbUL,
0x8722d3164c08355bUL,
0xd4629e6bf989ab17UL,
0x33d1f758803f1b5fUL,
0xccbf7676d9382449UL,
0x21caa973533cd1a3UL,
0xa30489a85e186d75UL,
0x16999266c202edddUL,
0x750ecaedfd2ef626UL,
0x4ade9fed6244eb69UL,
0x84f05c3c7f038614UL,
0xcadc6eb01be7d306UL,
0x18ef7d1783836eedUL,
0x93a70f55cca2a7d7UL,
0x9c85050b675a80dfUL,
0xb872463e6c010948UL,
0xb14db6d8fcd6b76UL,
0xe84f58a000a76633UL,
0x2c262aca8fb1166fUL,
0x342de2d3881f9ec2UL,
0x27145aa26abbbbebUL,
0x921fab9a326ba7c8UL,
0x1ab05fe41768134cUL,
0xdab98a2b75a6a40eUL,
0x7545bf11aa118299UL,
0x3c21cb5c6cdb4772UL,
0xfe8f7ff27686f884UL,
0x4ce0ce87061df79cUL,
0xbfd8c1efbbcd482UL,
0x2eb4713d00670dccUL,
0x3495e1e4d0cc009fUL,
0xc2083bb8278aa4bfUL,
0xce7c5174af4c6dabUL,
0xb8ed10b0ba28fd39UL,
0x2818cfe16e4d6ef3UL,
0x18d1b72f317ff497UL,
0x73781b4e525714ddUL,
0xc57a289404e029bfUL,
0xa1c08148a31838bdUL,
0xf89da53309525870UL,
0xdef5811e235ca8c0UL,
0xf1ca1d32c5a1dc00UL,
0xeb583f601f750cefUL,
0x30a0002822362d62UL,
0x55b2b64444a54c73UL,
0xa5c092b2a5e90b32UL,
0xfc29609e9b912ea3UL,
0x4351d32c2d0aa7d3UL,
0xfdf1131ab320f9d2UL,
0x5f815fa84ff00cd4UL,
0x8bce59be78285b76UL,
0x2ea6af98f56a1c6dUL,
0x76edbe73ed4cc5ecUL,
0x61756f689a2d2d2fUL,
0xe76bb22ae68739e2UL,
0xb61de8da5bc28b34UL,
0xd96516ecaccdf3f2UL,
0xb819215140cf6c47UL,
0xbb29861664929b86UL,
0xedf39610a4d1e1caUL,
0x211d809437e0759bUL,
0xc76f668a6f45254UL,
0x76c76fd296f17548UL,
0xacbafd1a85d42ebfUL,
0xe3b981cfa7f284deUL,
0x11ac178666bd4f28UL,
0xe527a562368b12d9UL,
0xf3eb065cc26984d3UL,
0x7f4b2f02e4289f79UL,
0x9d4f1d02c3de1101UL,
0xb058b4979774f0f3UL,
0x8285d907a192eccaUL,
0x1fae9368ce2e7f44UL,
0xd0d5954643559936UL,
0xd9d738e526a3594fUL,
0x4e612ad4e00b2d47UL,
0x8eaf9ea5d098e5f0UL,
0x2e2743f202d64a21UL,
0xda4b1e0463b016c3UL,
0xf23de53ac4473bdfUL,
0xd31f0947b05fbdd0UL,
0x22c8f45fa3c56f6cUL,
0xbf708a07e9860028UL,
0xb322527c2abd7708UL,
0x3bdd1f055970a4cbUL,
0x42ffb1851f12979aUL,
0xcee13b94dbc77ae5UL,
0xccbf51e852813369UL,
0xfac530f1684e181fUL,
0xa52f4c32fbbba151UL,
0x51eafe2729027974UL,
0x90b15a2f8507e7c0UL,
0x5f9f6ea59dc4c8fUL,
0xd8f06098ff778efaUL,
0x6727cf974ae99e64UL,
0xb99aa79270b18e84UL,
0x7ab60caaa4abeb28UL,
0xd0841fe59d3caa80UL,
0x1211f0c6b9696f1dUL,
0xf03c6847644d7879UL,
0xd4959e261728cfc5UL,
0x34e50d0ee0642f52UL,
0xe5e0df35ac188259UL,
0xe356feca1e559f4dUL,
0x45cd38a5f9102717UL,
0x89229d65c0a7ffe7UL,
0xa6d2ddf11155bab4UL,
0xc8cacd17b5d433daUL,
0x349829d29005b7caUL,
0xea542eb2c54b9acaUL,
0x4467d93b85717e91UL,
0xff0d72d867cdd408UL,
0xaa36c73496e48bfcUL,
0x75f95d3ee509f464UL,
0x37ef720be9f96761UL,
0x818a6b18ee14fe94UL,
0xfd33638bb73bf87UL,
0x7453720be2959e1eUL,
0x602ab35fcc506116UL,
0x39e5ff960bd9f522UL,
0x3bad9b24249a7abeUL,
0x700692aadaf1bcd9UL,
0x3d35b58fb3223df2UL,
0xb530a430c36304efUL,
0x3f8312d325b2ba84UL,
0x5932ef60d76b8712UL,
0x64f652556dd568aUL,
0xcb984faa0cbc8e12UL,
0x49435273547faf5eUL,
0xa9baebdd08cc3c0bUL,
0x8c06cf1721c44cf8UL,
0x345aec9776c63681UL,
0xfd47e8334ddbe39UL,
0x863a58e6b504a644UL,
0xa4f1ce452472bcb1UL,
0xe8c426ffc6ee8214UL,
0x461232244f213f15UL,
0x258fe44c7931ad89UL,
0xbcfa5cf7d409029UL,
0x7d08b736f0cc9038UL,
0xd3fa14a0674aff98UL,
0x13a8d7d16590420aUL,
0xfd8eb446df2ff87fUL,
0xa897fbb524031077UL,
0x76122570f4d2bed9UL,
0x88b212447f45eed3UL,
0x6a55f06bdaed26b8UL,
0x19108d0d0f63414eUL,
0xc1f81bb0bba3f90cUL,
0x26e7a15302357ee2UL,
0xc05bb93baac04742UL,
0x1c7fdc43dbd48475UL,
0xa27badce1d1be14cUL,
0x4d313e799da4b7c6UL,
0x5e0d3397145597cfUL,
0x425cb028fd8ac9b0UL,
0xdf7437445c4bfd25UL,
0xe370156dafcc1085UL,
0xd9bac6f81f1a885dUL,
0x594fa5bca9e4c098UL,
0x44e519735022f327UL,
0xb2d68da436948709UL,
0x14396623ec1841f1UL,
0x99c8dba4bf532e9dUL,
0xbf4498acef5e26f5UL,
0xe81ce11ba381135fUL,
0x7e332246041143d1UL,
0x96be05c491688787UL,
0x80a3d6e7a7280280UL,
0x822eea68e776fb92UL,
0xe58dc3f73d244fccUL,
0xa72f1c37046998abUL,
0xe682bff3e4489015UL,
0x3e195f1d413cb474UL,
0xb3a642d38aab78f6UL,
0xe5e29c81728ccf6eUL,
0x146e5ecc4d83cdf2UL,
0x106132738b2fecc9UL,
0xa1fde661336b6e3fUL,
0x8135c2d9db4d31eUL,
0x134918063bca68daUL,
0x9baacdd6f66c85b3UL,
0x41d6e9fe60dbd9bbUL,
0xfad68ac64798f39eUL,
0xd473b401ee826d5eUL,
0xb66b501453208b65UL,
0x2fcf189cea856978UL,
0x74e3141dfdde4c54UL,
0xc9ebfa8289746d09UL,
0x5c054a1aca3f3374UL,
0x17fb427348bc435cUL,
0xac2b11242a1edfb7UL,
0xe71458635f2ca58cUL,
0x7a2151f896416826UL,
0x3b501fe593913bd4UL,
0x6f08ba08dac15909UL,
0x942cca45b4e12c73UL,
0xb3b25f3b1a11a2e3UL,
0x6f48d3428f641588UL,
0x887e0eb517729d6eUL,
0xa6530d551aaed6ceUL,
0x906c782a2bbd9aa4UL,
0xdd587bee55c60fd1UL,
0x53a091466135decdUL,
0x76561a1e580bdb00UL,
0xd8c005ad5ab53646UL,
0x935ca67a90509939UL,
0xac90318690587146UL,
0x7d3e951b3aa277e1UL,
0xc44f55f64faae4c6UL,
0xb9e10431ed378bcfUL,
0x56a986049dc5bdc4UL,
0x5714568631ac83d8UL,
0x9bd66349af841a7dUL,
0xa775a502eacd43b6UL,
0x197825a541b9784eUL,
0x1601e16fe0b82f42UL,
0x4b96a317f456fb24UL,
0x1cae1b8252eeac6eUL,
0xd7f64f83c3060174UL,
0xae8a9f024c1d628UL,
0xb09dd8f7605d5281UL,
0xbd45af4cf0b8004cUL,
0xfd56c44e41040066UL,
0x641a64a3af185b4eUL,
0x278395ad44bad0b3UL,
0xa874a0010dd303edUL,
0xfc95aa83ca09f6f4UL,
0x945a44827ae57eedUL,
0xf89ede532dfed529UL,
0xa962de24afab5d9UL,
0xde0cf3706b3d2f4fUL,
0x44c08bba959357ccUL,
0x9d1c5ff39ac64fb0UL,
0x8c0f3cdd36909beaUL,
0xe22bdea0b1b9d9caUL,
0x97d8a4925f9e130cUL,
0x3b58e7d69952dfbbUL,
0xcbc2d1230edc32ffUL,
0xe91b2d3a837654f8UL,
0xb611be506495c36aUL,
0x63bd7730b5883214UL,
0x505f8898f2ce878bUL,
0xfcab2f7b6c0d8540UL,
0xbbddf48b9a0b88ceUL,
0xeb5718de8a30ca25UL,
0x74db0ec9be14431bUL,
0x9a876bae9f14e3b7UL,
0x95171c326d20f7f4UL,
0x89885570e950016UL,
0xde8c286b18aea025UL,
0xc785508bc8a87d79UL,
0x276a9fde8deeba7cUL,
0x2f746a3a7758c6f8UL,
0x63150c2b1388ed30UL,
0x10b9119540142e48UL,
0x56114e8b99c2c625UL,
0x87e5ab0f0a987107UL,
0xa273610bcdab5e5fUL,
0xdf23ba89f405e672UL,
0x4c83e081b492504cUL,
0x5375f30fee4a4727UL,
0x704d4514998b087UL,
0x130a10e2ef82b2f8UL,
0x2e311ff378ce19UL,
0x71bcfab32d538021UL,
0xf250aa18e76c7128UL,
0xd625109dfedd607dUL,
0x5528287503cd6a1UL,
0x7f71796069b7e44UL,
0x98602ad54d1ef49eUL,
0x37956b88abf294cUL,
0xb8eadd3b52cdc841UL,
0x237f2d11c7eec5c3UL,
0xe143f3f5b72ceac6UL,
0x7919a608d61b5b9aUL,
0x6bf1f1b90a2b72e9UL,
0x985d10606be4d6cdUL,
0x2fe86b3df34ffa3eUL,
0xea9e4aa168f7288UL,
0xb47f6cdf9ecc6531UL,
0x4332db15e07daba3UL,
0xf9e030bb655f66daUL,
0xfaefc6d699523d1bUL,
0x82bb9345dfc30661UL,
0x100e0dca5af7e132UL,
0x15d0dea1f0760c68UL,
0x17f4764ade5e46bcUL,
0xb18a8bc9d0c84cf9UL,
0x7b1d67afd35e0edcUL,
0xf9323d9fedefcac7UL,
0x6c0acfaee3a701b2UL,
0x3ce516ac99438f5aUL,
0x66fd7b7065c5ccc1UL,
0x38096c934b44950bUL,
0xf53f21649109a8d1UL,
0x29ff9f8d99b977c1UL,
0xd167e2e6e538676bUL,
0xf307008b9b745ad6UL,
0xc9b0a5c84480188fUL,
0x7f57b03e86d4d72eUL,
0xe074309e1d945072UL,
0xfe5fedca29ef518aUL,
0x5cf2f2d88ea8c0d4UL,
0x2da72b860d8b6ffaUL,
0xd2e69e90b1b20db0UL,
0x5dad9fa8c1e15315UL,
0xf40273a3f7c97c72UL,
0xc191042fdf27d3acUL,
0xc8e290bab3743535UL,
0x4a8172baf406a56UL,
0xf8e7cd4a9da04045UL,
0xd779af006186a64dUL,
0xa1b78c792eb46d02UL,
0xbb253c9745f723e2UL,
0x8898e90b56e1c609UL,
0xf3283b8fb2919034UL,
0x7c563de76fc3599fUL,
0xbfcc377b66783264UL,
0x7682331325b27c59UL,
0x2ee6d21724df5601UL,
0x9ac5dcb48757fa22UL,
0x1b3e9dc0b7ae5b3UL,
0x8878890a75c93470UL,
0xd63e08268ada11bdUL,
0x9554ba472dfa0ae7UL,
0xc0c9e9436ec1d912UL,
0x347ded95498626c3UL,
0x7a7f81b9bbd65089UL,
0x2ce31c10706393d0UL,
0xc8c8f4b053b7b7efUL,
0xf13e514e70c14b47UL,
0x96a6bb4e48f8bf9aUL,
0x84c3054a6f3be81cUL,
0x81e0d0888c877bdeUL,
0x1290709b699273fdUL,
0xc81f5a1469485092UL,
0x301b0a9a92aeb933UL,
0xad0ff7b6037ae5dbUL,
0xd714393548a9189cUL,
0xac33296fbb5dc298UL,
0xeeb34216b9a2b959UL,
0xe157e9c6d2f1e823UL,
0xcbca3eaad144a438UL,
0x4dc2b8b9f3eb61bcUL,
0x12571477fb64195fUL,
0xfd0162e10e89b3deUL,
0x6783e1aae376a7e1UL,
0x1cf5f0a19860bdccUL,
0xc415701f13943de3UL,
0x79c2a9657fdbd7aeUL,
0xb128032e3b43f929UL,
0xae2c5b1c967ab72eUL,
0x515f483caf1c4fd6UL,
0xe4dff8ac86e0cc65UL,
0x8612b6a121371c44UL,
0x2bdbaa2c338becb6UL,
0x699168e04b6716dfUL,
0xe15361956897667aUL,
0x66196b25d511980dUL,
0x960b17362d0106fUL,
0x7c8769a09eb8a4dUL,
0x8d05511a23d5df26UL,
0x2a7950ae83f31bd7UL,
0xcec58d2cb65622b6UL,
0x92b3c5942a8b19aeUL,
0x7657079a144845dfUL,
0x51a91d1a37be64c4UL,
0xa605782ec36d81e9UL,
0x8fdefb333ed52fdUL,
0xcb018c193ed7f85dUL,
0xccea7d615fe1cae7UL,
0xe754ffc987cd3c4UL,
0xe6983662e18e1826UL };

#define TO_ENGINE_NAMED_PIPE_PREFIX "/tmp/to_chess_engine_pipe"
#define FROM_ENGINE_NAMED_PIPE_PREFIX "/tmp/from_chess_engine_pipe"
#define MAX_NUMBER_OF_GAME_THREADS 16 //these threads just process pgn files
#define MAX_NUMBER_OF_SQL_THREADS 8 //these threads just update NextMovesX.db, where X is thread number. 
                                    //Use power of 2, i.e. 1, 2, 4, 8. 8 is max!
#define COMMIT_NEXT_MOVES_ROWS 5000000
#define COMMIT_GAMES_ROWS 10
#define MAX_SLEEP_COUNTER_FOR_SQLWRITER 16
#define MAX_NUMBER_OF_GAMES 256000
#define MAX_NUMBER_OF_ECO_LINES 2048
#define MAX_NUMBER_OF_GAME_MOVES 1024
#define MAX_NUMBER_OF_NEXT_MOVES 64
#define MAX_NUMBER_OF_TAGS 22
#define MAX_NUMBER_OF_ECO_TAGS 3
#define MAX_TAG_NAME_LEN 32
#define MAX_ECO_TAG_NAME_LEN 10
#define MAX_TAG_VALUE_LEN 90
#define MAX_SAN_MOVES_LEN 4096
#define MAX_UCI_MOVES_LEN 4096
#define MAX_ECO_MOVES_LEN 1024
#define MAX_FEN_STRING_LEN 90
#define MAX_UCI_OPTION_NAME_LEN 32
#define MAX_UCI_OPTION_TYPE_LEN 8
#define MAX_UCI_OPTION_TYPE_NUM 5
#define MAX_UCI_OPTION_STRING_LEN 32
#define MAX_UCI_OPTION_BUTTON_NUM 4
#define MAX_UCI_OPTION_SPIN_NUM 16
#define MAX_UCI_OPTION_CHECK_NUM 16
#define MAX_UCI_OPTION_COMBO_NUM 4
#define MAX_UCI_OPTION_COMBO_VARS 8
#define MAX_UCI_OPTION_STRING_NUM 8
#define MAX_UCI_MULTI_PV 8
#define MAX_VARIATION_PLIES 16
#define NO_MATE_SCORE 21000
#define MATE_SCORE 20000
#define INACCURACY 30
#define MISTAKE 75
#define BLUNDER 175

// Time management constants
#define MIN_MOVES_REMAINING 40
#define MAX_MOVES_REMAINING 60
#define TIME_SAFETY_BUFFER 5000 // 5s in ms
#define CRITICAL_TIME_FACTOR 1.5
#define MIN_TIME_THRESHOLD 10000 // 10s in ms
#define MIN_ITERATIONS 1001
#define MAX_ITERATIONS 1000001 // Safety cap


#define FILE_A 0x0101010101010101UL
#define FILE_B 0x0202020202020202UL
#define FILE_C 0x0404040404040404UL
#define FILE_D 0x0808080808080808UL
#define FILE_E 0x1010101010101010UL
#define FILE_F 0x2020202020202020UL
#define FILE_G 0x4040404040404040UL
#define FILE_H 0x8080808080808080UL

#define RANK1 0x00000000000000FFUL
#define RANK2 0x000000000000FF00UL
#define RANK3 0x0000000000FF0000UL
#define RANK4 0x00000000FF000000UL
#define RANK5 0x000000FF00000000UL
#define RANK6 0x0000FF0000000000UL
#define RANK7 0x00FF000000000000UL
#define RANK8 0xFF00000000000000UL

/*
// B-tree minimum degree (adjust based on performance needs)
#define T 32
typedef struct BTreeNode {
    unsigned long keys[2 * T - 1];    // Array of keys
    struct BTreeNode *children[2 * T]; // Array of child pointers
    int num_keys;                 // Current number of keys
    bool is_leaf;                 // Leaf node flag
} BTreeNode;

bool BTreeSearch(BTreeNode *, unsigned long);
BTreeNode* BTreeInsert(BTreeNode*, unsigned long);
void BTreeCleanUp(BTreeNode * root, void * db);
*/

/// <summary>
/// Castling types enumeration
/// </summary>
enum CastlingSide { CastlingSideNone, CastlingSideKingside, CastlingSideQueenside, CastlingSideBoth };

/// <summary>
/// Enumeration of castling rights: lowest two bits for white, highest - for black
/// </summary>
enum CastlingRightsEnum {
	CastlingRightsWhiteNoneBlackNone = CastlingSideNone,
	CastlingRightsWhiteNoneBlackKingside = CastlingSideKingside << 2,
	CastlingRightsWhiteNoneBlackQueenside = CastlingSideQueenside << 2,
	CastlingRightsWhiteNoneBlackBoth = CastlingSideBoth << 2,
	CastlingRightsWhiteKingsideBlackNone = CastlingSideKingside,
	CastlingRightsWhiteQueensideBlackNone = CastlingSideQueenside,
	CastlingRightsWhiteBothBlackNone = CastlingSideBoth,
	CastlingRightsWhiteKingsideBlackKingside = CastlingSideKingside | (CastlingSideKingside << 2),
	CastlingRightsWhiteQueensideBlackKingside = CastlingSideQueenside | (CastlingSideKingside << 2),
	CastlingRightsWhiteBothBlackKingside = CastlingSideBoth | (CastlingSideKingside << 2),
	CastlingRightsWhiteKingsideBlackQueenside = CastlingSideKingside | (CastlingSideQueenside << 2),
	CastlingRightsWhiteQueensideBlackQueenside = CastlingSideQueenside | (CastlingSideQueenside << 2),
	CastlingRightsWhiteBothBlackQueenside = CastlingSideBoth | (CastlingSideQueenside << 2),
	CastlingRightsWhiteKingsideBlackBoth = CastlingSideKingside | (CastlingSideBoth << 2),
	CastlingRightsWhiteQueensideBlackBoth = CastlingSideQueenside | (CastlingSideBoth << 2),
	CastlingRightsWhiteBothBlackBoth = CastlingSideBoth | (CastlingSideBoth << 2)
};

/// <summary>
/// Color enumeration
/// </summary>
enum Color { ColorWhite, ColorBlack };

static char * color[] = { "white", "black" };

/// <summary>
/// File enumeration from a to h
/// </summary>
enum Files {FileA, FileB, FileC, FileD, FileE, FileF, FileG, FileH, FileNone};
static char enumFiles[] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'N'};
/// <summary>
/// Rank enumeration from 1 to 8
/// </summary>
enum Ranks {Rank1, Rank2, Rank3, Rank4, Rank5, Rank6, Rank7, Rank8, RankNone};
static char enumRanks[] = {'1', '2', '3', '4', '5', '6', '7', '8', 'N'};

static unsigned long bitFiles[] = {FILE_A, FILE_B, FILE_C, FILE_D, FILE_E, FILE_F, FILE_G, FILE_H};
static unsigned long bitRanks[] = {RANK1, RANK2, RANK3, RANK4, RANK5, RANK6, RANK7, RANK8};

/// <summary>
/// Square enumeration
/// </summary>
enum SquareName {
	SquareA1, SquareB1, SquareC1, SquareD1, SquareE1, SquareF1, SquareG1, SquareH1,
	SquareA2, SquareB2, SquareC2, SquareD2, SquareE2, SquareF2, SquareG2, SquareH2,
	SquareA3, SquareB3, SquareC3, SquareD3, SquareE3, SquareF3, SquareG3, SquareH3,
	SquareA4, SquareB4, SquareC4, SquareD4, SquareE4, SquareF4, SquareG4, SquareH4,
	SquareA5, SquareB5, SquareC5, SquareD5, SquareE5, SquareF5, SquareG5, SquareH5,
	SquareA6, SquareB6, SquareC6, SquareD6, SquareE6, SquareF6, SquareG6, SquareH6,
	SquareA7, SquareB7, SquareC7, SquareD7, SquareE7, SquareF7, SquareG7, SquareH7,
	SquareA8, SquareB8, SquareC8, SquareD8, SquareE8, SquareF8, SquareG8, SquareH8, SquareNone
};

static char * squareName[] = {
	"a1", "b1", "c1", "d1", "e1", "f1", "g1", "h1",
	"a2", "b2", "c2", "d2", "e2", "f2", "g2", "h2",
	"a3", "b3", "c3", "d3", "e3", "f3", "g3", "h3",
	"a4", "b4", "c4", "d4", "e4", "f4", "g4", "h4",
	"a5", "b5", "c5", "d5", "e5", "f5", "g5", "h5",
	"a6", "b6", "c6", "d6", "e6", "f6", "g6", "h6",
	"a7", "b7", "c7", "d7", "e7", "f7", "g7", "h7",
	"a8", "b8", "c8", "d8", "e8", "f8", "g8", "h8", "none"
};

int squareColor(int sqName);

/// <summary>
/// Diagonal enumeration
/// </summary>
enum Diagonals {
	DiagonalH1H1, DiagonalG1H2, DiagonalF1H3, DiagonalE1H4, DiagonalD1H5, DiagonalC1H6, 
	DiagonalB1H7, DiagonalA1H8, DiagonalA2G8, DiagonalA3F8, DiagonalA4E8, DiagonalA5D8, 
	DiagonalA6C8, DiagonalA7B8, DiagonalA8A8, DiagonalNone
};

/// <summary>
/// Anti diagonals
/// </summary>
enum Antidiagonals {
	AntidiagonalA1A1, AntidiagonalA2B1, AntidiagonalA3C1, AntidiagonalA4D1, AntidiagonalA5E1,
	AntidiagonalA6F1, AntidiagonalA7G1, AntidiagonalA8H1, AntidiagonalB8H2, AntidiagonalC8H3,
	AntidiagonalD8H4, AntidiagonalE8H5, AntidiagonalF8H6, AntidiagonalG8H7, AntidiagonalH8H8, AntidiagonalNone
};

/// <summary>
/// Piece type enumeration
/// </summary>
enum PieceType {PieceTypeNone, Pawn, Knight, Bishop, Rook, Queen, King, PieceTypeAny};
static char * pieceType[] = {"none", "pawn", "knight", "bishop", "rook", "queen", "king", "any"};

static float pieceValue[] = { 0.0, 0.1, 0.30, 0.32, 0.50, 0.90, 1.0 }; //scaled down by kings value of 10
static float pieceMobility[] = { 0.0, 4.0, 8.0, 11.0, 14.0, 25.0, 8.0 }; //max value - used for norm

/// <summary>
/// Piece enumeration: first three bits are used to encode the type, fourth bit defines the color.
/// Shifting PieceName by 3 to the right gives PieceColor: color = piece >> 3
/// Masking 3 lowest bits returns the PieceType: type = piece & 7
/// </summary>
enum PieceName {
	PieceNameNone,
	WhitePawn, WhiteKnight, WhiteBishop, WhiteRook, WhiteQueen, WhiteKing, PieceNameWhite, PieceNameAny,
	BlackPawn, BlackKnight, BlackBishop, BlackRook, BlackQueen, BlackKing, PieceNameBlack
};

static char * pieceName[] = {
	"none", 
	"white pawn", "white knight", "white bishop", "white rook", "white queen", "white king", "whites", "any",
	"black pawn", "black knight", "black bishop", "black rook", "black queen", "black king", "blacks"
};

static unsigned char pieceLetter[] = {' ', 'P', 'N', 'B', 'R', 'Q', 'K', 'C', '*', 'p', 'n', 'b', 'r', 'q', 'k', 'c'};

enum PieceLetter { 
	PieceLetter_e, PieceLetter_P, PieceLetter_N, PieceLetter_B, PieceLetter_R, 
	PieceLetter_Q, PieceLetter_K, PieceLetter_X, PieceLetter_O, PieceLetter_p, PieceLetter_n,
	PieceLetter_b, PieceLetter_r, PieceLetter_q, PieceLetter_k, PieceLetter_x
};

/// <summary>
/// UCI promo letters, for SAN moves should be converted to uppercase
/// </summary>
enum PromoLetter { PromoLetter_n = 2, PromoLetter_b, PromoLetter_r, PromoLetter_q};

static unsigned char promoLetter[] = { '\0', '\0', 'N', 'B', 'R', 'Q' };

enum MoveType {
	MoveTypeNormal, MoveTypeValid, MoveTypeCapture, MoveTypeCastlingKingside = 4, 
	MoveTypeCastlingQueenside = 8, MoveTypePromotion = 16, MoveTypeEnPassant = 32, 
	MoveTypeNull = 64
};

static const char * moveType[] = {
	"normal", "valid", "capture", "castling kingside", 
	"castling queenside", "promotion", "en passant", "null"
};

enum ProblemType { ProblemTypeNone, ProblemTypeBestMove, ProblemTypeAvoidMove };

enum GameStage { OpeningGame, MiddleGame, EndGame, FullGame };
static const char * gameStage[] = { "opening", "middlegame", "endgame", "fullgame" };

/// <summary>
/// Forthys-Edwards Notation for the position preceding a move 
/// </summary>
struct Fen {
	///<summary>
	/// FEN string as it is
	///</summary>
	char fenString[90];
	/// <summary>
	/// 1st field in FEN - ranks, separated by '/'
	/// </summary>
	char ranks[8][9];
	/// <summary>
	/// 2nd field in FEN - side to move
	/// </summary>
	int sideToMove;
	/// <summary>
	/// 3rd field in FEN - castling Rights
	/// </summary>
	int castlingRights;
	/// <summary>
	/// 4th field in FEN - EnPassant square - is set after double advancing a pawn regardless of whether an opposite side can capture or not.
	/// In X-FEN it is only setup when the opposite side can actually capture, although illegal captures (when pinned, for example) may not be checked
	/// </summary>
	int enPassant;
	unsigned long enPassantLegalBit;
	/// <summary>
	/// 5th field in FEN - number of plies since last pawn advance or capture
	/// Used in 50-move draw rule
	/// </summary>
	unsigned short halfmoveClock;
	/// <summary>
	/// 6th field in FEN - full move number
	/// </summary>
	unsigned short moveNumber;
	/// <summary>
	/// True if castling rights are indicated by the castling rook file instead of letters for kingside or queenside
	/// </summary>
	bool isChess960;
	/// <summary>
	/// Castling rook files for chess 960 position indexed by side: 0 - kingside castling (short), 1 - queenside castling (long) and by color: 0 - white, 1 - black
	/// </summary>
	int castlingRook[2][2];
	unsigned long castlingBits;
};

///<summary>
/// UCI option types
///</summary>
enum OptionType {
	Button, Check, Combo, Spin, String
};

static const char * optionTypes[] = {
	"button", "check", "combo", "spin", "string"
};

///<summary>
/// UCI spin type option
///</summary>
struct OptionSpin {
	char name[MAX_UCI_OPTION_NAME_LEN];
	long defaultValue;
	long value;
	long min;
	long max;
};

///<summary>
/// UCI check type option
///</summary>
struct OptionCheck {
	char name[MAX_UCI_OPTION_NAME_LEN];
	bool defaultValue;
	bool value;
};

///<summary>
/// UCI string type option
///</summary>
struct OptionString {
	char name[MAX_UCI_OPTION_NAME_LEN];
	char defaultValue[MAX_UCI_OPTION_STRING_LEN];
	char value[MAX_UCI_OPTION_STRING_LEN];
};

///<summary>
/// UCI combo type option
///</summary>
struct OptionCombo {
	char name[MAX_UCI_OPTION_NAME_LEN];
	char defaultValue[MAX_UCI_OPTION_STRING_LEN];
	char values[MAX_UCI_OPTION_COMBO_VARS][MAX_UCI_OPTION_STRING_LEN];
	char value[MAX_UCI_OPTION_STRING_LEN];
};

///<summary>
/// UCI button type option
///</summary>
struct OptionButton {
	char name[MAX_UCI_OPTION_NAME_LEN];
	bool value; //if true, the button will be pressed
};

struct Engine {
	char id[MAX_UCI_OPTION_STRING_LEN];
	char authors[2 * MAX_UCI_OPTION_STRING_LEN];
	int numberOfCheckOptions, numberOfComboOptions, numberOfSpinOptions,
		numberOfStringOptions, numberOfButtonOptions;
	struct OptionCheck optionCheck[MAX_UCI_OPTION_CHECK_NUM];
	struct OptionCombo optionCombo[MAX_UCI_OPTION_COMBO_NUM];
	struct OptionSpin optionSpin[MAX_UCI_OPTION_SPIN_NUM];
	struct OptionString optionString[MAX_UCI_OPTION_STRING_NUM];
	struct OptionButton optionButton[MAX_UCI_OPTION_BUTTON_NUM];
	char engineName[255];
	char namedPipeTo[255];
	char namedPipeFrom[255];
	char position[MAX_FEN_STRING_LEN]; //FEN string
	char moves[MAX_UCI_MOVES_LEN]; //UCI moves
	int logfile;
	//go() arguments
	long movetime;
	int depth;
	int nodes;
	int mate;
	bool ponder;
	bool infinite;
	long wtime;
	long btime;
	long winc;
	long binc;
	int movestogo;
	char * searchmoves;
	FILE * toEngine;
	FILE * fromEngine;
};

struct Evaluation {
	unsigned char maxPlies;
	unsigned char depth;
	unsigned char seldepth;
	unsigned char multipv;
	int scorecp;
	int matein; //mate in <moves>, not <plies>
	unsigned long nodes;
	unsigned long nps;
	unsigned short hashful;//permill (per thousand)
	unsigned char tbhits;
	unsigned long time; //ms
	char pv[1024];
	char bestmove[6];
	char ponder[6];
	unsigned char nag;
};

/// <summary>
///  Converts FEN string to struct fen
/// </summary>
int strtofen(struct Fen *, const char *);

/// <summary>
///  Updates fenString in Fen struct
/// </summary>
int fentostr(struct Fen *);

/// <summary>
/// Square class represents chess board square
/// </summary>
struct Square {
	int name;
	unsigned long long bitSquare;
	int file;
	int rank;
	int diag;
	int antiDiag;
}; //__attribute__((aligned(32)));

/// <summary>
/// ChessPiece struct
/// </summary>
struct ChessPiece {
	int name;
	int type;
	int color;
	struct Square square;
};// __attribute__((aligned(32)));

/// <summary>
/// Board struct represents chess board
/// </summary>
struct Board {
	unsigned long long defendedPieces; //defended opponent pieces 
	unsigned long long attackedPieces; //sideToMove pieces attacked by opponent
	unsigned long long attackedSquares; //all squares attacked by opponent
	unsigned long long blockingSquares;
	unsigned long long checkers;
	unsigned long long pinnedPieces;
	unsigned long long pinningPieces;
	unsigned long long occupations[16];
	int piecesOnSquares[64];
	unsigned long long oPawnMoves, oKnightMoves, oBishopMoves, oRookMoves, oQueenMoves, oKingMoves, pawnMoves, knightMoves, bishopMoves, rookMoves, queenMoves, kingMoves;
	unsigned long long movesFromSquares[64]; //these include opponents moves as well
	unsigned long long sideToMoveMoves[64]; //these are just the moves of sideToMove
	unsigned long long channel[64]; //AI model input 21 channels (16 for each piece + 5 for promotions)
	unsigned long long sourceSquare[10]; //source square for AI channel[64] 
	unsigned long long hash;
	int opponentColor;
	int plyNumber;
	int numberOfMoves;
	int capturedPiece;
	int promoPiece;
	struct ChessPiece movingPiece;
	bool isCheck;
	bool isStaleMate;
	bool isMate;
	struct Fen * fen;
	struct ZobristHash * zh;
};// __attribute__((aligned(32)));

struct Move {
	int type;
	struct Square sourceSquare;
	struct Square destinationSquare;
	char sanMove[12];
	char uciMove[6];
	char pad[2];
	struct Board * chessBoard;
	int castlingRook;
};// __attribute__((aligned(32)));

struct BMPR {
  int samples;
  int sample;
  int channels;
  float * boards_legal_moves; // [batch_size, number_of_channels, 8, 8]
  long * move_type; // piece channel [0, 10] [batch_size]
  long * src_sq;
  long * dst_sq; // destination square
  long * result; // [batch_size]
  //int * stage; // [batch_size]
};

static char * startPos = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
static const unsigned long STARTPOS_HASH = 0xf012ba38f3542c87UL;
static const unsigned long STARTPOS_CASTLING_RIGHTS = 0xc415701f13943de3UL;

struct ZobristHash {
	/// <summary>
	/// Zobrist hash of a given board 
	/// </summary>
	unsigned long hash;
	unsigned long blackMove;
	unsigned long prevCastlingRights;
	unsigned long prevEnPassant;
	unsigned long castling[16];
	unsigned long enPassant[8];
	unsigned long piecesAtSquares[13][64];
	/// <summary>
	/// Hash of the standard chess starting position
	/// </summary>
//	unsigned long startPosHash;
};

struct MoveScoreGames {
  char move[6]; //uci move
  int score; //position score, i.e. sum of wins and losses by making this move
  unsigned int games;
  int scorecp; // evaluated by a chess engine but negative values meaning black winning, positive - white
};

struct MoveScores {
  char move[6]; //uci move
  double score; //weighted score, i.e. score / total number of games in NextMoves.db for a given position
  int scorecp;
};

enum Tags {
	Unknown, Event, Site, Date, Round, White, Black, Result,
	Annotator, PlyCount, TimeControl, Time, Termination, Mode, FEN, SetUp, Opening, Variation, Variant, WhiteElo, BlackElo, ECO
};

enum EcoTags {
	eECO, eOpening, eVariation
};

static char * tags[] = {
	"Unknown", "Event", "Site", "Date", "Round", "White", "Black", "Result",
	"Annotator", "PlyCount", "TimeControl", "Time", "Termination", "Mode", "FEN", "SetUp", "Opening", "Variation", "Variant", "WhiteElo", "BlackElo", "ECO"
};

static char * ecotags[] = {
	"ECO", "Opening", "Variation"
};

enum Variant {
	Standard, Chess960
};

static char * variant[] = {
	"Standard", "chess 960"
};

/*struct Tag {
	/// <summary>
	/// Tag name
	/// </summary>
	enum Tags name;
	/// <summary>
	/// Tag value
	/// </summary>
	char value[80];
};
*/
typedef char Tag[MAX_NUMBER_OF_TAGS][MAX_TAG_VALUE_LEN];

typedef char EcoTag[MAX_NUMBER_OF_ECO_TAGS][MAX_TAG_VALUE_LEN];

/// <summary>
/// Game class represents a PGN or EPD-formated chess game and may include tags (opcodes), moves and the game result
/// </summary>
struct Game {
	/// <summary>
	/// Space-separated moves in SAN format without numbers
	/// </summary>
	char sanMoves[MAX_SAN_MOVES_LEN];
	/// <summary>
	/// Chess game PGN header tag array
	/// </summary>
	Tag tags;
	int numberOfPlies;
};

struct EcoLine {
	/// <summary>
	/// Space-separated moves in SAN format without numbers
	/// </summary>
	char sanMoves[MAX_ECO_MOVES_LEN];
	/// <summary>
	/// Chess eco header tag array
	/// </summary>
	EcoTag tags;
};

struct Board * cloneBoard(struct Board * src);
void freeBoard(struct Board * board);
///<summary>
/// Generates unbiased random random number from inclusive range [min, max]
/// The first argument is min, the second is max
///</summary>
int randomNumber(const int, const int);

///<summary>
/// initializes a ZobristHash struct
///</summary>
void zobristHash(struct ZobristHash *);

///<summary>
/// calculates Zobrist hash from a Board struct and updates
/// ZobristHash struct, which should be initialized first
///<summary>
void getHash(struct ZobristHash *, struct Board *);

///<summary>
/// resets ZobristHash struct to initial game position
///</summary>
void resetHash(struct ZobristHash *);

///<summary>
/// updates ZobristHash of a given Board after a given move
///</summary>
int updateHash(struct ZobristHash *, struct Board *, struct Move *);

///<summary>
/// fills Square struct from SquareName enum
///</summary>
void square(struct Square *, int squareName);

///<summary>
/// fills ChessPiece struct from a given Square and a PieceName
///</summary>
void piece(struct Square *, struct ChessPiece *, int pieceName);

///<summary>
/// makes a Board struct from a Fen one including legal moves generation stored in Board->movesFromSquares
///</summary>
int fentoboard(struct Fen *, struct Board *);

// four standard bit manupulation functions
unsigned char bitCount(unsigned long);
unsigned char lsBit(unsigned long);
unsigned char genLSBit(unsigned long);
unsigned char msBit(unsigned long);
void unpack_bits(unsigned long number, float * bit_array);

///<summary>
/// This function strips game result from SAN moves string
///</summary>
void stripGameResult(struct Game *);

///<summary>
/// This function strips comments, variations and NAGs from SAN moves string
///</summary>
int normalizeMoves(char *);

///<summary>
/// This function removes move numbers from normalized SAN moves string
///</summary>
int movesOnly(char *);

///<summary>
/// generates all legal moves on a given board
/// stored in movesFromSquares array of the Board struct
///</summary>
void generateMoves(struct Board *);

///<summary>
/// This function generates board position given an array of enum PieceName[] and its size (the second argument)
/// as well as sideToMove (third argument), castlingRights (fourth argument) and enPassant (fifth argument)
/// The board should be passed by reference in the last argument
///<summary>
void generateEndGame(int * pieceName, int numberOfPieces, int sideToMove, int castlingRights, int enPassant, struct Board *);

int generateEndGames(int maxGameNumber, int maxPieceNumber, char * dataset, char * engine, long movetime, int depth, int hashSize, int threadNumber, char * syzygyPath, int multiPV, bool logging, bool writedebug, int threads);

///<summary>
/// validates a UCI or SAN move given by the last argument
/// and initializes the Move struct on a given board
///</summary>
int initMove(struct Move *, struct Board *, char *);

///<summary>
/// makes a given Move on board and updates Board and Fen struct
///</summary>
void makeMove(struct Move *);

/// <summary>
/// Parses the line into tag name and tag value
/// and fills the given Tag array
/// </summary>
int strtotag(Tag tag, char *);

/// <summary>
/// Parses the line into tag name and tag value
/// and fills the given EcoTag array
/// </summary>
int strtoecotag(EcoTag, char *);

///<summary>
/// Count number of games, which begin with a string specified by the second argument
/// from a FILE stream and index them by a game start position in the array long[].
/// The last argument is the number of games
///</summary>
unsigned long countGames(FILE *, char *, unsigned long[], unsigned long);

///<summary>
/// Reads PGN game tags for a first game pointed by a file stream
/// and fills the provided array of typedef Tag
/// returns 0 on success, non-zero on error
///</summary>
int gTags(Tag, FILE *);

///<summary>
/// Reads eco file tags for a first eco line pointed by a file stream
/// and fills the provided array of typedef EcoTag
/// returns 0 on success, non-zero on error
///</summary>
int eTags(EcoTag, FILE *);

///<summary>
/// Plays multiple pgn games from a given pgn file
///</summary>
unsigned long openGamesFromPGNfile(char * fileName, int gameThreads, int sqlThreads, char * ecoFileName, int minElo, int maxEloDiff, int minMoves, int numberOfGames, bool generateZobristHash, bool updateDb, bool createDataset, char * dataset, bool eval, char * engine, long movetime, int depth, int hashSize, int engineThreads, char * syzygyPath, int multiPV, bool logging);

///<summary>
/// This function is similar to playGames() with a difference that it takes a list of PGN
/// files and each thread is given the entire file from the list
/// It saves time by begining to play games one by one from the start of a file
/// eliminating the need to index them in the file first, which is time consuming for large PGN files
/// The first arg is an array of file names, the second arg is the number of of files in this array
/// The rest are the same as in pgnGames()
///</summary>
unsigned long openGamesFromPGNfiles(char * fileNames[], int numberOfFiles, int gameThreads, int sqlThreads, char * ecoFileName, int minElo, int numberOfGames, int maxEloDiff, int minMoves, bool generateZobristHash, bool updateDb, bool createDataset, char * dataset, bool eval, char * engine, long movetime, int depth, int hashSize, int engineThreads, char * syzygyPath, int multiPV, bool logging);

//functions for fast data loading in AI model training
//int initGamesFromPGNs(char * fileNames[], int numberOfFiles, int minElo, int maxEloDiff);
struct BMPR * dequeueBMPR();
void * getGame(void * context);
void * getGameCsv(void * context);
void getGame_detached(char ** fileNames, const int numberOfFiles, const int minElo, const int maxEloDiff, const int minMoves, const int numberOfChannels, const int numberOfSamples, const int bmprQueueLen, const int gameStage, const unsigned long steps);
void free_bmpr(struct BMPR * bmpr);
int boardLegalMoves(float * boards_legal_moves, int sample, int channels, struct Board * board);
int getStage(struct Board * board);
//float materialBalance(struct Board * board); //from the view of side to move
void cleanup_magic_bitboards(void);
void init_magic_bitboards(void);

///<summary>
/// This function initializes Game struct from a stream position given by FILE
/// It returns 0 on success and 1 on the EOF
///</summary>
int initGame(struct Game *, FILE *);

///<summary>
/// Plays a game given its struct
///</summary
int playGame(struct Game *);

/// <summary>
/// draws a chessboard
/// if the second argument is true, then also all legal moves for each piece 
/// </summary>
void writeDebug(struct Board *, bool);

///<summary>
/// draws a board with just a specified piece name such as white pawns, for example
/// filling other squares with '0' or 'o' regardless if it is occupied by other piece or not
///</summary>
//void drawBoard(struct Board *, int pieceName);

///<summary>
/// draws moves from a given square sq
/// it is called from writeDebug
///</summary>
//void drawMoves(struct Board *, int squareName);

///<summary>
/// returns 0 if occupations reconcile with piecesOnSquares,
/// otherwise, non-zero error code
///</summary>
int reconcile(struct Board *);

///<summary>
/// returns string representation in the first argument
/// of a bit field move type given in a secondary argument
///</summary>
void getMoveType(char *, unsigned char);

///<summary>
/// ECO classificator for a chess game (first argument)
/// second argument - array of ecoLines
/// third argument - the number of ecoLines
///</summary>
void ecoClassify(struct Game *, struct EcoLine **, int);

///<summary>
/// runs a chess engine in a child process
/// The second arg is engine binary path
///</summary>
int engine(struct Engine *, char *);

struct Engine * initChessEngine(char * engineName, long movetime, int depth, int hashSize, int threadNumber, char * syzygyPath, int multiPV, bool logging);

void releaseChessEngine(struct Engine * chessEngine);

///<summary>
/// returns chess engine option index for a given name and type
///</summary>
int nametoindex(struct Engine *, char *, int optionType);

///<summary>
/// gets chess engine options
///</summary>
//int getOptions(char *, struct Engine *);
int getOptions(struct Engine *);

int setOption(struct Engine *, char *, int optionType, void *);
void setOptions(struct Engine *);

bool isReady(struct Engine *);
bool newGame(struct Engine *);
void stop(struct Engine *);
void quit(struct Engine *);
bool position(struct Engine *);
//void go(long, int, int, int, char *, bool, bool, long, long, long, long, int, struct Engine *, struct Evaluation **);
int go(struct Engine *, struct Evaluation **);
float eval(struct Engine *);

unsigned long md5(char *);

///<summary>
/// This function returns the number of moves for a given Zobrist hash and 
/// the array of struct MoveScoreGames[MAX_NUMBER_OF_NEXT_MOVES] from NextMovesX.db files
/// where X is the number encoded by the number of most significant bits of the hash
/// The number of bits usually corresponds to sqlThreads (the last arg), which
/// can be 1, 2, 4 or 8. Therefore, X ranges from 0 to 7, i.e. 0 for 1 thread, 0 to 1 for 2 threads,
/// 0 to 3 for 4 threads and 0 to 7 for 8 threads
///</summary>
int nextMoves(unsigned long, struct MoveScoreGames **, int);

///<summary>
/// This function is similar to nextMoves except it returns the sorted array of moveScores
/// Sorting is done by weighted absolute scores from the highest to the lowest, 
/// where score = score / totalNumberOfGames for a given position
/// The last arg is the number of sql threads, which should be 1, 2, 4 or 8 depending on how many db files you have
///</summary>
int bestMoves(unsigned long, int color, struct MoveScores *, int);

/*
void libchess_init_nnue(const char * nnue_file);
void libchess_init_nnue_context(struct NNUEContext * ctx);
void libchess_free_nnue_context(struct NNUEContext * ctx);
int libchess_evaluate_nnue(const struct Board * board, struct NNUEContext * ctx);
int libchess_evaluate_nnue_incremental(const struct Board * board, const struct Board * prev_board, struct Move * move, struct NNUEContext * ctx);
void libchess_evaluate_dataset(struct Board * boards, struct Move * moves, int * scores, int n_positions);
*/

/*
int openDb(const char *, sqlite3 *);
int closeDb(sqlite3 *);
int getNextMoves(sqlite3 *, sqlite3_int64, struct MoveScoreGames **);
int getNextMove(sqlite3 *, sqlite3_int64, const char *, struct MoveScoreGames *);
int updateNextMove(sqlite3 *, sqlite3_int64, const char *, int);
*/

///<summary>
/// naive chess piece (except pawns) move generator
/// moves are limited by board boundary only
/// may be used at the start to populate move arrays[64] of unsigned long
///<summary>
//unsigned long moveGenerator(int pieceType, struct Square *);
//unsigned long moveGenerator(int pieceType, int squareName);
#ifdef __cplusplus
}
#endif
#endif

