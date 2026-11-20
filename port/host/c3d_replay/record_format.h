/*
 * The backend replay harness's record file (roadmap P56 package D): what the host build's renderer frontend handed the
 * backend (gfx_backend.h), written by port/host/backend_record.c (DKR_BACKEND_RECORD=path) and read by
 * port/host/c3d_replay/replay.c, which plays it through the 3DS backend (port/3ds/gfx_c3d.c) and the real citro3d.
 *
 * Little-endian, both sides 32-bit x86 (-m32), so structs are written as they are in memory. The header holds the
 * sizeof of every struct written: a struct that changes size fails loudly instead of being misread. Pointers inside a
 * struct (a draw state's textures, a mesh part's mesh) are replaced by ids before writing: 0 is NULL, ids start at 1
 * and are never reused within a file.
 */
#ifndef PORT_C3D_REPLAY_RECORD_FORMAT_H
#define PORT_C3D_REPLAY_RECORD_FORMAT_H

#define REC_MAGIC 0x42524B44u /* "DKRB" */
#define REC_VERSION 1

enum RecOp {
    REC_INIT = 1,
    REC_START_FRAME,   /* u32 frame (counted by the recorder from 0) */
    REC_END_FRAME,
    REC_CLEAR_COLOR,   /* s16 rect[4], u8 rgba[4] */
    REC_CLEAR_DEPTH,   /* s16 rect[4] */
    REC_DRAW_TRIANGLES, /* GfxDrawState, s32 numTriangles, GfxVertex[numTriangles * 3] */
    REC_DRAW_INDEXED,  /* GfxDrawState, s32 numVerts, GfxVertex[numVerts], s32 numTriangles, u16[numTriangles * 3] */
    REC_DRAW_MODEL,    /* GfxDrawState, GfxModelParams, s32 numVerts, GfxModelVertex[], s32 numTriangles, u16[] */
    REC_DRAW_MESHES,   /* GfxDrawState, GfxModelParams, s32 numParts, GfxMeshPart[numParts] (mesh = id) */
    REC_MESH_CREATE,   /* u32 id (0: the host had no room), s32 numVerts, GfxModelVertex[], s32 numIndices, u16[] */
    REC_MESH_FREE,     /* u32 id */
    REC_TEXTURE_UPLOAD, /* u32 id, GfxTexture (pixels and backendData NULL), u32 pixels[width * height] */
    REC_TEXTURE_FREE,  /* u32 id */
    REC_TEXTURE_SHARE, /* u32 dst id, GfxTexture dst (pixels and backendData NULL), u32 src id */
    REC_GPU_KIND,      /* s32 kind (gfxb_gpu_kind, or the payload of gfxb_gpu_kind_call) */
    REC_GPU_KINDS_FRAME, /* s32 frame */
    REC_GPU_TRANSFORM, /* (the frame's question; the replay asks the 3DS backend the same) */
    REC_END = 0xFF
};

typedef struct RecHeader {
    unsigned int magic, version;
    unsigned int sizeofVertex, sizeofDrawState, sizeofTexture, sizeofModelVertex, sizeofModelParams, sizeofMeshPart;
    unsigned int sizeofPointer;
    unsigned int reserved[7];
} RecHeader;

#endif
