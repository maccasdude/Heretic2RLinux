// vk_skeleton.h
//
// Skeletal animation + reference point system for flex models, ported faithfully
// from ref_gl1 (Skeletons/r_Skeletons.c + Skeletons/r_SkeletonLerp.c). Produces
// the per-frame entity->referenceInfo placements that the client effects system
// consumes (staff/weapon trails, foot shadows) and applies skeletal joint
// rotations to mesh vertices (upper-body bend when casting/aiming).

#ifndef VK_SKELETON_H
#define VK_SKELETON_H

#include "qcommon/q_Typedef.h"
#include "qcommon/Reference.h"   // Placement_t, LERPedReferences_t
#include "qcommon/Skeletons.h"   // ModelSkeleton_t deps, CL_SkeletalJoint_t
#include "qcommon/Matrix.h"      // matrix3_t
#include "qcommon/ArrayedList.h" // ArrayedListNode_t

// Skeleton model structs. These mirror ref_gl1's Skeletons/m_Skeleton.h and
// m_SkeletalCluster.h exactly (data-layout definitions, not GL-specific), so
// the shared qcommon SkeletonCreators[] populate them identically.
typedef struct M_SkeletalJoint_s {
    int        children;   // Must be the first field.
    Placement_t model;     // Relative to the model.
    Placement_t parent;    // Relative to the parent joint.
    matrix3_t   rotation;
    qboolean    inUse;
} M_SkeletalJoint_t;

typedef struct ModelSkeleton_s {
    M_SkeletalJoint_t* rootJoint;
    ArrayedListNode_t* rootNode;
} ModelSkeleton_t;

typedef struct M_SkeletalCluster_s {
    int      children;     // Must be the first field.
    int      numVerticies;
    int*     verticies;
    qboolean inUse;
} M_SkeletalCluster_t;

struct entity_s;

// Per-model skeletal data parsed from the .fm "skeleton" and "references" blocks.
// One of these lives inside each vk_model that has skeletal/reference data.
typedef struct vk_model_skel_s {
    int          skeletalType;        // SKEL_* (-1 = none)
    int          rootCluster;         // index into SkeletalClusters[] (from CreateSkeleton)
    int          numClusters;

    qboolean     haveSkeleton;        // per-frame joint placements present
    // skeletons[frame].rootJoint[cluster].model/parent placements.
    // Allocated as num_frames entries; each holds rootCluster joints.
    void*        skeletons;           // ModelSkeleton_t* (opaque here)

    int          referenceType;       // REF_* (-1 = none)
    qboolean     haveRefs;
    Placement_t* refsForFrame;        // num_frames * numReferences[referenceType]

    int          num_frames;          // mirror of header for bounds checks
    int          num_xyz_full;        // original num_xyz (before ref/skel subtraction)
    int          num_xyz_render;      // reduced num_xyz used for rendering verts
} vk_model_skel_t;

// Parse the "skeleton" block. Returns bytes consumed handled internally; updates
// *num_xyz when skeleton data is packed inline (not as per-frame placements).
void VK_Skel_ParseSkeleton(vk_model_skel_t* ms, const void* data, int num_frames,
                           int framesize, int* num_xyz);

// Parse the "references" block. Updates *num_xyz when refs are packed inline.
void VK_Skel_ParseReferences(vk_model_skel_t* ms, const void* data, int num_frames,
                             int* num_xyz);

// Free per-model skeletal allocations.
void VK_Skel_FreeModel(vk_model_skel_t* ms);

// Per-frame: compute entity->referenceInfo placements (and joint rotations) for
// this model + entity. Mirrors ref_gl1 FrameLerp's reference/skeleton path.
// 'lerped' is the already-lerped vertex array (num_xyz_full + packed refs), used
// when references are stored inline in the frames rather than refsForFrame.
// front/back/move are the frame lerp vectors from the caller's FrameLerp.
void VK_Skel_LerpReferences(vk_model_skel_t* ms, struct entity_s* e,
                            const vec3_t* lerped,
                            const byte* frames, int framesize,
                            const vec3_t move, const vec3_t frontv, const vec3_t backv,
                            float refdef_time);

// Apply skeletal joint rotations + swapFrame upper/lower body split to mesh
// vertices. Full port of ref_gl1 LerpStandardSkeleton + DoSkeletalRotations.
// 'lerped' holds the already-lerped main-frame verts (run/walk pose); this
// function overwrites the upper-body cluster verts with the swap-frame (cast)
// pose when the entity has a swapFrame, then rotates the cluster by joint
// angles. 'frames'/'framesize' give access to raw frame vert data for the swap
// lerp. move/front/back are the main-frame lerp vectors.
void VK_Skel_RotateMeshVerts(vk_model_skel_t* ms, struct entity_s* e, vec3_t* lerped,
                             const byte* frames, int framesize,
                             const vec3_t move, const vec3_t frontv, const vec3_t backv);

// Returns the model's reference type for re.GetReferencedID (-1 if none).
int  VK_Skel_ReferenceType(const vk_model_skel_t* ms);

#endif
