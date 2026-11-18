// Copyright 2026 The simple-engine-2 Authors.
//
// The blit vertex shader: one triangle that covers the target, drawn with
// three vertices and no vertex buffer. Texture coordinates run from (0, 0)
// at the top-left corner of the target to (1, 1) at the bottom-right.

struct Output {
  [[vk::location(0)]] float2 uv : TEXCOORD0;
  float4 position : SV_Position;
};

Output main(uint vertex_id : SV_VertexID) {
  Output output;
  const float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);
  output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0,
                           1.0);
  output.uv = uv;
  return output;
}
