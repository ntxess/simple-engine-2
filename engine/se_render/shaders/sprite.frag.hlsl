// Copyright 2026 The simple-engine-2 Authors.
//
// The sprite fragment shader: the texture, sampled with the nearest texel
// (the legacy SFML textures are not smooth), modulated by the sprite's
// tint, as sf::Sprite's vertex colour does. The texture and its sampler are
// one combined image sampler in SPIR-V (glslang --auto-sampled-textures).

Texture2D<float4> sprite_texture : register(t0, space2);
SamplerState sprite_sampler : register(s0, space2);

struct Input {
  [[vk::location(0)]] float2 uv : TEXCOORD0;
  [[vk::location(1)]] float4 tint : TEXCOORD1;
};

float4 main(Input input) : SV_Target0 {
  return sprite_texture.Sample(sprite_sampler, input.uv) * input.tint;
}
