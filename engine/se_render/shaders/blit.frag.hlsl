// Copyright 2026 The simple-engine-2 Authors.
//
// The blit fragment shader: copies the composite to the swapchain with the
// nearest texel and no blending.

Texture2D<float4> source_texture : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

struct Input {
  [[vk::location(0)]] float2 uv : TEXCOORD0;
};

float4 main(Input input) : SV_Target0 {
  return source_texture.Sample(source_sampler, input.uv);
}
