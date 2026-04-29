// 每个线程处理一个像素
// 输入：Texture2D<float4> srcBGRA
// 输出：RWTexture2D<uint> dstNV12 (packed NV12)
Texture2D<float4> srcBGRA : register(t0);
RWTexture2D<uint> dstNV12 : register(u0);

[numthreads(16,16,1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint x = DTid.x;
    uint y = DTid.y;

    uint width, height;
    srcBGRA.GetDimensions(width, height);

    if (x >= width || y >= height)
        return;

    float4 bgra = srcBGRA.Load(int3(x,y,0));

    // RGB -> YUV (ITU-R BT.601)
    float Y = 0.299*bgra.r + 0.587*bgra.g + 0.114*bgra.b;
    float U = -0.169*bgra.r - 0.331*bgra.g + 0.5*bgra.b + 0.5;
    float V = 0.5*bgra.r - 0.419*bgra.g - 0.081*bgra.b + 0.5;

    // Y plane
    dstNV12[int2(x,y)] = (uint)(Y * 255.0);

    // UV plane：每 2x2 像素写一次
    if ((x % 2 == 0) && (y % 2 == 0)) {
        float4 c1 = srcBGRA.Load(int3(x,y,0));
        float4 c2 = srcBGRA.Load(int3(x+1,y,0));
        float4 c3 = srcBGRA.Load(int3(x,y+1,0));
        float4 c4 = srcBGRA.Load(int3(x+1,y+1,0));

        float U_avg = (-0.169*c1.r-0.331*c1.g+0.5*c1.b + 
                       -0.169*c2.r-0.331*c2.g+0.5*c2.b + 
                       -0.169*c3.r-0.331*c3.g+0.5*c3.b + 
                       -0.169*c4.r-0.331*c4.g+0.5*c4.b)/4.0 + 0.5;

        float V_avg = (0.5*c1.r-0.419*c1.g-0.081*c1.b + 
                       0.5*c2.r-0.419*c2.g-0.081*c2.b + 
                       0.5*c3.r-0.419*c3.g-0.081*c3.b + 
                       0.5*c4.r-0.419*c4.g-0.081*c4.b)/4.0 + 0.5;

        uint packedUV = ((uint)(V_avg*255) << 8) | ((uint)(U_avg*255));
        dstNV12[int2(x/2, y/2 + height/2)] = packedUV;
    }
}