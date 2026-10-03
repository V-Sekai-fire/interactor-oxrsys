struct Yuv420Params {
  uint width;
  uint height;
};

[[vk::binding(0, 0)]]
ConstantBuffer<Yuv420Params> params;
[[vk::binding(1, 0)]]
StructuredBuffer<uint> luma;
[[vk::binding(2, 0)]]
StructuredBuffer<uint> cb;
[[vk::binding(3, 0)]]
StructuredBuffer<uint> cr;
[[vk::binding(4, 0)]]
RWStructuredBuffer<uint> rgbx;

[shader("compute")] [numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint i = tid.x;
  if ((i >= (params.width * params.height))) {
    return;
  }
  uint x = (i % params.width);
  uint y = (i / params.width);
  uint c = (((y >> 1u) * (params.width >> 1u)) + (x >> 1u));
  float yy = float(((luma[(i >> 2u)] >> ((i & 3u) * 8u)) & 255u));
  float pb = (float(((cb[(c >> 2u)] >> ((c & 3u) * 8u)) & 255u)) - 128.000000);
  float pr = (float(((cr[(c >> 2u)] >> ((c & 3u) * 8u)) & 255u)) - 128.000000);
  uint r = uint(min(max(((yy + (1.574800 * pr)) + 0.500000), 0.000000), 255.000000));
  uint g = uint(min(max((((yy - (0.187300 * pb)) - (0.468100 * pr)) + 0.500000), 0.000000), 255.000000));
  uint b = uint(min(max(((yy + (1.855600 * pb)) + 0.500000), 0.000000), 255.000000));
  rgbx[i] = (((r | (g << 8u)) | (b << 16u)) | 4278190080u);
}
