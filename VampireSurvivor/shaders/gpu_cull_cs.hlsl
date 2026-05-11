// gpu_cull_cs.hlsl
// Phase-2 GPU frustum culling + indirect draw arg generation.
// Not wired up yet — placeholder for the next milestone.
//
// Flow (called once per frame before the instanced draw):
//   1. CPU resets gDrawArgs[0].instanceCount to 0 via CopyBufferRegion.
//   2. Dispatch CSMain with ceil(instanceCount / 64) groups.
//   3. Each thread tests one instance against 6 frustum planes.
//   4. Visible instances are written compactly to gOutput;
//      instanceCount is incremented atomically.
//   5. Rendering pass calls ExecuteIndirect with gDrawArgs.

struct InstanceData {
    float4x4 world;
    float4   color;
};

struct DrawIndexedArgs {
    uint indexCountPerInstance;
    uint instanceCount;          // reset to 0 before dispatch, incremented here
    uint startIndexLocation;
    int  baseVertexLocation;
    uint startInstanceLocation;
};

StructuredBuffer<InstanceData>   gInput    : register(t0);
RWStructuredBuffer<InstanceData> gOutput   : register(u0);
RWStructuredBuffer<DrawIndexedArgs> gDrawArgs : register(u1);

cbuffer CullCB : register(b0) {
    float4 gPlanes[6];   // world-space frustum planes (normal + d)
    uint   gCount;       // total input instance count
    float3 gPad;
};

bool InFrustum(float4x4 world) {
    // Extract center from last column of world matrix (column-major in HLSL)
    float3 center = float3(world._41, world._42, world._43);
    // Approximate bounding sphere radius from diagonal of scale
    float  radius = 0.5f * length(float3(world._11, world._22, world._33));

    [unroll]
    for (int i = 0; i < 6; ++i) {
        if (dot(gPlanes[i].xyz, center) + gPlanes[i].w < -radius)
            return false;
    }
    return true;
}

[numthreads(64, 1, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= gCount) return;

    InstanceData inst = gInput[tid.x];
    if (InFrustum(inst.world)) {
        uint slot;
        InterlockedAdd(gDrawArgs[0].instanceCount, 1, slot);
        gOutput[slot] = inst;
    }
}
