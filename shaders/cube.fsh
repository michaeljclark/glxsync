/*
 * maj2_random
 *
 * maj2_random is a simplified floating point hash function derived from SHA-2,
 * retaining its high quality entropy compression function modified to permute
 * entropy from a vec2 (designed for UV coordinates) returning float values
 * between 0.0 and 1.0. since maj2_random is a hash function it will return
 * coherent noise. vector argument can be truncated prior to increase grain.
 */

#version 150

in vec3 v_normal;
in vec2 v_uv;
in vec4 v_color;
in vec3 v_fragPos;
in vec3 v_lightDir;

out vec4 outFragColor;

#define NROUNDS 2

/* first 8 rounds of the SHA-256 k constant */
uint sha256_k[8] = uint[]
(
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u
);

uint ror(uint x, int d) { return (x >> d) | (x << (32-d)); }
uint sigma0(uint h1) { return ror(h1, 2) ^ ror(h1, 13) ^ ror(h1, 22); }
uint sigma1(uint h4) { return ror(h4, 6) ^ ror(h4, 11) ^ ror(h4, 25); }
uint ch(uint x, uint y, uint z) { return z ^ (x & (y ^ z)); }
uint maj(uint x, uint y, uint z) { return (x & y) ^ ((x ^ y) & z); }
uint gamma0(uint a) { return ror(a, 7) ^ ror(a, 18) ^ (a >> 3); }
uint gamma1(uint b) { return ror(b, 17) ^ ror(b, 19) ^ (b >> 10); }

vec2 unorm(uvec2 n) { return uvec2(n & uvec2((1u<<23)-1u)) / vec2((1u<<23)-1u); }
vec2 trunc(vec2 uv, float d) { return floor(uv / d) * d; }

uvec2 maj2_extract(vec2 uv)
{
    /*
     * extract 48-bits of entropy from mantissas to create truncated
     * two word initialization vector 'W' composed using the 48-bits
     * of 'uv' entropy rotated and copied to keep the field equalized.
     * the exponent is ignored because the inputs are expected to be
     * normalized 'uv' values such as texture coordinates. it would be
     * beneficial to include the exponent entropy but we can't depend
     * on frexp or ilogb and log2 would be inaccurate.
     */
    vec2 s = sign(uv);
    uint x = uint(abs(uv.x) * float(1u<<23)) | (uint(s.x < 0) << 23);
    uint y = uint(abs(uv.y) * float(1u<<23)) | (uint(s.y < 0) << 23);

    return uvec2((x) | (y << 24), (y >> 8) | (x << 16));
}

vec2 maj2_random(vec2 uv)
{
    uint H[8] = uint[] ( 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u );
    uint W[2];
    uint T0,T1;
    int i;

    uvec2 st = maj2_extract(uv);

    W[0] = st.x;
    W[1] = st.y;

    for (i=0; i<NROUNDS; i++) {
        W[i] = gamma1(W[(i-2)&1]) + W[(i-7)&1] + gamma0(W[(i-15)&1]) + W[(i-16)&1];
    }

    /* we use N=2 rounds instead of 64 and alternate 2 words of iv in W */
    for (i=0; i<NROUNDS; i++) {
        T0 = W[i&1] + H[7] + sigma1(H[4]) + ch(H[4], H[5], H[6]) + sha256_k[i];
        T1 = maj(H[0], H[1], H[2]) + sigma0(H[0]);
        H[7] = H[6];
        H[6] = H[5];
        H[5] = H[4];
        H[4] = H[3] + T0;
        H[3] = H[2];
        H[2] = H[1];
        H[1] = H[0];
        H[0] = T0 + T1;
    }

    return unorm(uvec2(H[0] ^ H[1] ^ H[2] ^ H[3],
                       H[4] ^ H[5] ^ H[6] ^ H[7]));
}

void main()
{
  // uncomment for temporal surface stability i.e. increase grain
  //float r = maj2_random(trunc(v_uv, 0.1)).x * 0.5;
  float r = maj2_random(v_uv).x * 0.5;

  float ambient = 0.1;
  float diff = max(dot(v_normal, v_lightDir), 0.0);
  vec4 finalColor = (ambient + diff + r) * v_color;
  outFragColor = vec4(finalColor.rgb, v_color.a);
}
