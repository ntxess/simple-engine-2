// Copyright 2026 The simple-engine-2 Authors.
//
// The sprite vertex shader (Renderer2D's sprite pipeline). Vertices are in
// the camera's canvas pixels with texel coordinates (se_render_core's
// SpriteVertex); one uniform block per draw maps them to clip space and to
// normalised texture coordinates. Vertex inputs use TEXCOORD<n> semantics
// (SDL's D3D12 backend binds attribute location n to TEXCOORD n) and
// [[vk::location(n)]] for SPIR-V; dxc ignores the latter.

cbuffer SpriteUniforms : register(b0, space1) {
  // Clip space = canvas pixels * xy + zw.
  float4 canvas_to_clip;
  // xy: 1 / the texture's size in texels; zw unused.
  float4 texel_to_uv;
};

struct Input {
  [[vk::location(0)]] float2 position : TEXCOORD0;
  [[vk::location(1)]] float2 texel : TEXCOORD1;
  [[vk::location(2)]] float4 tint : TEXCOORD2;
};

struct Output {
  [[vk::location(0)]] float2 uv : TEXCOORD0;
  [[vk::location(1)]] float4 tint : TEXCOORD1;
  float4 position : SV_Position;
};

Output main(Input input) {
  Output output;
  output.position = float4(
      input.position * canvas_to_clip.xy + canvas_to_clip.zw, 0.0, 1.0);
  output.uv = input.texel * texel_to_uv.xy;
  output.tint = input.tint;
  return output;
}
