// instance.hlsl
// Instanced rendering for all game entities.
//
// Root signature:
//   [0] 16 root constants (b0) — row-major viewProj
//   [1] Root SRV (t0)          — StructuredBuffer<InstanceData>

struct InstanceData {
    float4x4 world;
    float4   color;
};

StructuredBuffer<InstanceData> gInstances : register(t0);

cbuffer SceneCB : register(b0) {
    float4x4 gViewProj;   // row-major (transposed on CPU before upload)
};

struct VSInput {
    float3 position : POSITION;
    uint   iid      : SV_InstanceID;
};

struct PSInput {
    float4 svPos : SV_POSITION;
    float4 color : COLOR0;
    float3 normal : NORMAL;   // view-space normal for simple shading
};

PSInput VSMain(VSInput v) {
    InstanceData inst = gInstances[v.iid];

    // inst.world is stored transposed (row-major) — mul order matches CPU intent
    float4 worldPos = mul(float4(v.position, 1.f), inst.world);

    PSInput o;
    o.svPos  = mul(worldPos, gViewProj);
    o.color  = inst.color;

    // Approximate normal from world matrix columns (no separate normal buffer)
    float3 up = normalize(float3(inst.world[0][1], inst.world[1][1], inst.world[2][1]));
    o.normal  = up;
    return o;
}

float4 PSMain(PSInput p) : SV_TARGET {
    // Simple diffuse: fixed light from above + ambient
    float3 lightDir = normalize(float3(0.5f, 1.f, -0.3f));
    float  ndotl    = saturate(dot(p.normal, lightDir));
    float3 shaded   = p.color.rgb * (0.35f + 0.65f * ndotl);
    return float4(shaded, p.color.a);
}
