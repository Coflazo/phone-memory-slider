# noise-c (vendored subset)

The [Noise Protocol Framework](https://noiseprotocol.org/) in C, used by `core/src/link/noise_ik.cpp` for the phone link's
`Noise_IK_25519_ChaChaPoly_SHA256` handshake.

- Source: https://github.com/signalapp/noise-c (Signal's fork of rweather/noise-c)
- Pinned commit: `6d940358181c0b1d9355521aebd0d5dc6ca0d0f9`
- Fetched 2026-10-08 as https://codeload.github.com/signalapp/noise-c/tar.gz/6d940358181c0b1d9355521aebd0d5dc6ca0d0f9
  (archive SHA-256 `5867e73b3f44d2f72a3d09bb6bc0bf6f7cdad3bb52ab832aa4d714b36c81563b`)
- License: MIT, Copyright (C) 2016 Southern Storm Software, Pty Ltd. `LICENSE` is upstream's `COPYING`, unchanged.
  The bundled primitives are MIT or public domain; `src/crypto/README` and `src/crypto/ed25519/README.md` say which.

## What is here

Only what this one protocol needs: the protocol layer, the reference (no libsodium, no OpenSSL) backends for
Curve25519, ChaChaPoly and SHA-256, and what they pull in (ChaCha20, Poly1305, curve25519-donna, and the ed25519-donna
base-point code and SHA-512 that Curve25519 key derivation uses). Left out: every other algorithm (AES-GCM, BLAKE2,
SHA-512 as a Noise hash, Curve448, Kyber, New Hope, Ed25519 signatures), the keys and protobufs libraries, tests,
tools, and `src/protocol/rand_os.c` / `rand_sodium.c`.

No upstream file is modified. These files are ours, not upstream's:

| File | Purpose |
|---|---|
| `CMakeLists.txt` | Builds the static library `noise_c` with upstream's `ED25519_CUSTOMHASH` and `ED25519_CUSTOMRANDOM` defines, warnings off (`/W0`, `-w`) instead of the project's `/W4 /WX`. |
| `pms_unused_algorithms.c` | Constructors for the algorithms left out, returning NULL, so noise-c's algorithm switches link; creating such a state fails cleanly. |
| `.gitattributes` | `-text`, so a checkout keeps upstream's bytes and the hashes below hold. |
| `README.md` | This file. |

`noise_rand_bytes` (and `ed25519_randombytes_unsafe`, which ed25519-donna needs for batch verification we never call)
come from `core/src/link/random.cpp` instead of `rand_os.c`: `BCryptGenRandom` on Windows, `getentropy` elsewhere,
abort on failure. `core/CMakeLists.txt` links that object into `noise_c`.

## Files (SHA-256, identical to the pinned commit)

| File | SHA-256 |
|---|---|
| `LICENSE` | `f2c0cc36aa63c383fd1669b63eff210ee8cabb304dd818a9f442d93906eff587` |
| `include/noise/protocol.h` | `8d6cfca4bd55e1892aa35032108350e90a391e1df1c6cd5ddaf60978bc222066` |
| `include/noise/protocol/buffer.h` | `7194f16b2b8046e3ace2fd6ef03671aba0b063d4c4033dfcd7442acec10a5d42` |
| `include/noise/protocol/cipherstate.h` | `fb2902deb57339755810185894e76f6a7bf513df3f241f9eb4da59b00447d47f` |
| `include/noise/protocol/constants.h` | `d965d4ff8f98a2caa7c31045a43ae7d4c1d94526059f8b1c44a0f893df742496` |
| `include/noise/protocol/dhstate.h` | `67b5f26aee29416d6ab63f0097b28194575b4d935017d1cc2e133382c3101853` |
| `include/noise/protocol/errors.h` | `778a407b2c8d1d8f8fb163137efae1aaa6c37323cfa875657fef2f0c04413055` |
| `include/noise/protocol/handshakestate.h` | `6145f42caeedd0b88b6a55a1aa092d20466ee5192367895a873f238654a07711` |
| `include/noise/protocol/hashstate.h` | `23d0c1ff891d55d658c217316a43121a6afc7cbf15043f3ab1892d8bdd20ff49` |
| `include/noise/protocol/names.h` | `4c880b718b4433362568e47fbb380a1bdd74ade6b106c43c9d90b3bdb8877479` |
| `include/noise/protocol/randstate.h` | `47be42dbc4fc5ca86b60c094bdc1aba2917eebc394e70bd76672b10dd7a343ed` |
| `include/noise/protocol/signstate.h` | `ea269b214d84e98b54dc5f9a1c2c49c4860e032992402c369f5fd9bc291f6e53` |
| `include/noise/protocol/symmetricstate.h` | `54387b74d526feb6f312788282d33a39b88528322d180f2e39ef4d36d8dbd4ae` |
| `include/noise/protocol/util.h` | `903bf61248c195c4fb6908730f0556b1e1256dfa51e83626f9eb4c6bb0a43c70` |
| `src/backend/ref/cipher-chachapoly.c` | `a7f0052506ccf3a8bb4b8653151daf54d4e57468ef1312089845cc1f6693641b` |
| `src/backend/ref/dh-curve25519.c` | `b56ca0b9a8e0e2264f7f6f01f866b4e97b81c70ed643124a4cd6b835d0e9f1e3` |
| `src/backend/ref/hash-sha256.c` | `20a24f7b028e96ef9b66fac49046bfb8ad5ee3156bc7b50a23e74113e819d05a` |
| `src/crypto/README` | `0e3c246adf351f0d9e274bc613e45c9edc44188d6ffc521c9ba565f15bc89878` |
| `src/crypto/chacha/chacha.c` | `b0edb1b0be40b487bf6bbeb762ac65a09a3ff0b6dd419e039762561bfbbc0349` |
| `src/crypto/chacha/chacha.h` | `7314510b2ce69352d7359034c13fe10922a6e833999ddd7894b6f7276b0ec306` |
| `src/crypto/donna/curve25519-donna-c64.c` | `e975722a19a9924a19e897caf3d8110bed6c9da5c971c21dd2018b1f1ab91796` |
| `src/crypto/donna/curve25519-donna.c` | `4faf160733de281baf398ae77cd0eb3c470e47de110eb82f4babf9e89b25d9fc` |
| `src/crypto/donna/poly1305-donna-16.h` | `0a6066a4562654230603ffda5dd115d5095858e52d2459e7e1eed3f51602050e` |
| `src/crypto/donna/poly1305-donna-32.h` | `85bc63c1a468fe31a896c242be1cb7afe0cc9a262e5042911abc2cc5dbd1bcb6` |
| `src/crypto/donna/poly1305-donna-64.h` | `9911f24eef3da8af2daf153bd99b8c275bec9567194110262e5c6ac7f33b47df` |
| `src/crypto/donna/poly1305-donna-8.h` | `9caba0303b49face4e351d291110857497ddf34facc516b690a3bb86a754b7e5` |
| `src/crypto/donna/poly1305-donna.c` | `5b73b696330f991a62f8e6971eaf32f134533f9a374d7420c1f5bceb74691999` |
| `src/crypto/donna/poly1305-donna.h` | `db2194945f4aeb914e13efee982d365cd32cc16009febb6f68b51b6aaf17514e` |
| `src/crypto/ed25519/curve25519-donna-32bit.h` | `f92c32aa69d5a015a81b96bfd75063061447bcc60b79d7fa48f4056262addd39` |
| `src/crypto/ed25519/curve25519-donna-64bit.h` | `200b6f43ab3ef2a4c835a79c3795d169f4762f19b51af1f950952587f6573310` |
| `src/crypto/ed25519/curve25519-donna-helpers.h` | `a3cd2e774f9f57cfe16ac7b010085590464c602b9eb64e704d48b25860a7e9a1` |
| `src/crypto/ed25519/curve25519-donna-sse2.h` | `fa8e7894e269b0667c18d0ec9c66025468c0d1fd2b7e370d026d2239c95d8758` |
| `src/crypto/ed25519/ed25519-donna-32bit-sse2.h` | `c7b3658eb060f1ece83865a979bb48125ff45cc65f70ade96e51377c2109ab71` |
| `src/crypto/ed25519/ed25519-donna-32bit-tables.h` | `4487a16e12cd57252df91364ad820cc5eb93d91cbc602fc21d96c676134754bc` |
| `src/crypto/ed25519/ed25519-donna-64bit-sse2.h` | `086a0b7418114ed12141c9fcbb19d30a47fc2e14f97532874418c85920767bf6` |
| `src/crypto/ed25519/ed25519-donna-64bit-tables.h` | `417c853c5ffddfc8b3e244096985fb919e872d9d4cc02e4f6c2047a1c5ec599c` |
| `src/crypto/ed25519/ed25519-donna-64bit-x86-32bit.h` | `faa56400228b031de42b230e3d12192963bf863583f93f0ede5b93b843920e73` |
| `src/crypto/ed25519/ed25519-donna-64bit-x86.h` | `73d90a2fdcb746613e7d77fc320d63f4a3cfd5f15ecf6e320a81016d82c020fd` |
| `src/crypto/ed25519/ed25519-donna-basepoint-table.h` | `0bfcde5ac42fe07700b9c04b280d932c888d91a73597130201ff11dcfe629e3d` |
| `src/crypto/ed25519/ed25519-donna-batchverify.h` | `81c518f1591a3e90f952af752495bfb8e8b29082f63e56a441961957db04645d` |
| `src/crypto/ed25519/ed25519-donna-impl-base.h` | `243eee2c096612090bf724ca530590664605df1ca11744da09fe830a160c159a` |
| `src/crypto/ed25519/ed25519-donna-impl-sse2.h` | `693a03c83572d0d082fbbdb24867c313672b4372041aa774a2e343b0203a45fa` |
| `src/crypto/ed25519/ed25519-donna-portable-identify.h` | `bc3f37b7106dfac8594367b598747664d65e1a3ed5125d70c525649785070a6d` |
| `src/crypto/ed25519/ed25519-donna-portable.h` | `532091020ed9046bfc33e9ff927345039e29b3fb545bc74a2bd29f913b68ace5` |
| `src/crypto/ed25519/ed25519-donna.h` | `b53f69909a80a6930879aa093a41831aa6f3f6e443187f159eacc96cb7e6294e` |
| `src/crypto/ed25519/ed25519-hash-custom.h` | `7927f6b76490de6dfcbda4f38cdcfc5255e9d8766afea9530f578758ff3ffe2b` |
| `src/crypto/ed25519/ed25519-hash.h` | `d1ee2991539feb3bd1bade01dfabb2a9d6a4c86e9f9deda7da1df45c831f732e` |
| `src/crypto/ed25519/ed25519-randombytes-custom.h` | `db6ebec8ea68ec2d4aa1978cbea25b01379283a5a4f1f296cfd1e4a0eb3ea666` |
| `src/crypto/ed25519/ed25519-randombytes.h` | `0716dd5e8a68e4241b432f14852cc69db2be07915163daffd01b564f2af553a7` |
| `src/crypto/ed25519/ed25519.c` | `491efa60521d6ef3cc8b42c722adbb07c5a8da3e355bd9ed8947a5ca02cec594` |
| `src/crypto/ed25519/ed25519.h` | `98c711b3d04fcaaa9ff560efab6b0f2e969326208a7a854d56462162b1734bfb` |
| `src/crypto/ed25519/modm-donna-32bit.h` | `56716eb2f1eb1c5c5de494110b4ef912392ea7217a0e05608e3c70c236460ee1` |
| `src/crypto/ed25519/modm-donna-64bit.h` | `1e320582deefcd5547bc9b036f639bd8248c01b0f4cfa1d8f7e1703dc7eea3ac` |
| `src/crypto/sha2/sha256.c` | `18aae0ec4723ef4c9dedf235092b132552972777ef21133c54918d974e849062` |
| `src/crypto/sha2/sha256.h` | `c8c1e483b4bd13a82c15a977fc6fdc52a24408758bcef15d184037cd7650b964` |
| `src/crypto/sha2/sha512.c` | `421ca63ce84b23abeb900cb5e73a2452c39c3f56546be38311c5f9e651e0b7e8` |
| `src/crypto/sha2/sha512.h` | `dbfac3d1512221669a0d3ff1e3889c170fa17508446944271db7f6cced27e106` |
| `src/protocol/cipherstate.c` | `4729f6458140cb156863f69d51e44c480e898f35458b5c1869bf9d171e0fa035` |
| `src/protocol/dhstate.c` | `4fa25a523019423deae386a77a0909e7dfc2e1c30a2c55b72da378079db1c851` |
| `src/protocol/errors.c` | `db6b5defd051012dfe3393fe2728810c21479e369619e4bdcb779b2da75611dc` |
| `src/protocol/handshakestate.c` | `e1c182c60210d21ad8f8888968591291a2cefbefca281993bc1230663cf2df52` |
| `src/protocol/hashstate.c` | `18f655185822d1d597e538d445d43b472b21f819f72109bb83864fc13cb76ad8` |
| `src/protocol/internal.c` | `a21b06fa8c5c40177c2c808b037c01824b9f5baecfb0bd02ad8d7f5848ed4510` |
| `src/protocol/internal.h` | `dfd5f27d6a3ea301c3185aca75264ae3898597de6acf6abe1685e060f5a3675d` |
| `src/protocol/names.c` | `b84328d7138fc0155049f54117d98e6d49eacea5188142e784ada3fffdb9a1f4` |
| `src/protocol/patterns.c` | `c7664106caa887967f78c11fcd186fea63c465255ffc9439fb8eb8ee1954714e` |
| `src/protocol/randstate.c` | `d3e1470817a33e5c7e9085fce855069b19a64cde8bfd347f52a6a5047bc66878` |
| `src/protocol/signstate.c` | `5c0607baab640a8323feb5a43fd18c99131af98a4f0368a4232790a3a6e77b28` |
| `src/protocol/symmetricstate.c` | `1474f1da98c68ad05a18c4b2b0938950cfe53b4b8a145a4926f131b241b84703` |
| `src/protocol/util.c` | `c1538502560b204a63ca567deb10eed743533bf04329df6f36e2fc67eadf7657` |

To check: `sha256sum` each file, or diff against the archive above.
