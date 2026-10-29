#ifndef NV2A_GPU_RAW_TYPES_H
#define NV2A_GPU_RAW_TYPES_H
/* Scalar structured-buffer layout, shared with the geometry compute shader. */
#ifndef NV_GPU_HLSL
typedef struct NvRawAttr NvRawAttr;
typedef struct NvRawParams NvRawParams;
typedef struct NvRawDraw NvRawDraw;
typedef struct NvRawJob NvRawJob;
#endif
struct NvRawAttr {uint32_t offset,stride,type,size;};
struct NvRawParams {
    uint32_t program,count,state,vertex_base;
    uint32_t left,top,right,bottom;
    uint32_t cull_enable,cull_face,front_face,use_rc;
    uint32_t textured,fog_enable,fog_mode,reserved;
    float su,sv,fog_x,fog_y;
    float fog_color[4];
};
struct NvRawDraw {float constants[192][4];NvRawParams p;};
struct NvRawJob {uint32_t draw,element,output,reserved;};
#endif
