// vk_skeleton.c
//
// Faithful port of ref_gl1's skeletal/reference system:
//   - Skeletons/r_Skeletons.c   (joint rotation math)
//   - Skeletons/r_SkeletonLerp.c (LerpReferences + DoSkeletalRotations)
//
// The shared skeleton creators (SkeletonCreators[], numJointsInSkeleton[]),
// reference tables (numReferences[], jointIDs[]) and matrix math
// (Matricies3FromDirAndUp, RotatePointAboutLocalOrigin, TransformPoint,
// Matrix3*, HACK_Pitch_Adjust) live in qcommon/H2Common and are already linked
// into ref_vk, so we call them directly rather than re-implementing.

#include "vk_skeleton.h"
#include "vk_local.h"

#include "qcommon/Skeletons.h"
#include "qcommon/Reference.h"
#include "qcommon/ArrayedList.h"
#include "qcommon/Matrix.h"
#include "qcommon/Vector.h"

#include <string.h>
#include <stdlib.h>

extern refimport_t ri;

// Must match VKM_MAX_VERTS in vk_model.c (the lerped vertex array bound).
#define VKM_MAX_VERTS 4096

// ---------------------------------------------------------------------------
// Skeleton model storage (mirrors r_SkeletonLerp.c statics).
// SkeletalClusters[] / ClusterNodes[] are the shared global arrays defined in
// qcommon/r_Skeletons.c equivalent. ref_gl1 keeps them in r_Skeletons.c; here
// we declare the same globals so the qcommon skeleton creators populate them.
// NOTE: ref_gl1's r_Skeletons.c defines SkeletalClusters/ClusterNodes. We port
// that file's logic inline below and own those arrays here.
// ---------------------------------------------------------------------------

M_SkeletalCluster_t SkeletalClusters[MAX_ARRAYED_SKELETAL_JOINTS];
ArrayedListNode_t   ClusterNodes[MAX_ARRAYED_JOINT_NODES];

// Per-frame working skeletons (ported from r_SkeletonLerp.c).
static ModelSkeleton_t   cur_skeleton;
static M_SkeletalJoint_t cur_skeleton_joints[MAX_JOINTS_PER_SKELETON];
static ArrayedListNode_t cur_skeleton_nodes[MAX_JOINT_NODES_PER_SKELETON];

// Swap skeleton: the upper/lower body animation split (e.g. running while
// casting - lower body runs via 'frame', upper body casts via 'swapFrame').
static ModelSkeleton_t   swap_skeleton;
static M_SkeletalJoint_t swap_skeleton_joints[MAX_JOINTS_PER_SKELETON];
static ArrayedListNode_t swap_skeleton_nodes[MAX_JOINT_NODES_PER_SKELETON];

// On-disk frame vertex format (byte xyz + normal index), matches fmtrivertx_t.
typedef struct { byte v[3]; byte n; } skel_trivert_t;
typedef struct {
    float scale[3];
    float translate[3];
    char  name[16];
    skel_trivert_t verts[1];
} skel_frame_t;

// ---------------------------------------------------------------------------
// r_Skeletons.c port: skeleton creation + joint rotation
// ---------------------------------------------------------------------------

static int GetRootIndex(const int max, const int num_joints)
{
    qboolean skip_block = false;
    for (int i = 0; i < max; i++) {
        if (SkeletalClusters[i].inUse) continue;
        const int required_size = num_joints + i;
        if (required_size > max) return -1;

        for (int j = i + 1; j < required_size; j++) {
            if (SkeletalClusters[j].inUse) { i = j; skip_block = true; break; }
        }
        if (skip_block) { skip_block = false; continue; }

        for (int j = i; j < required_size; j++)
            SkeletalClusters[j].inUse = true;
        return i;
    }
    return -1;
}

int VK_Skel_CreateSkeleton(const int structure)
{
    const int index = GetRootIndex(MAX_ARRAYED_SKELETAL_JOINTS, numJointsInSkeleton[structure]);
    if (index < 0) return -1;
    SkeletonCreators[structure](SkeletalClusters, sizeof(M_SkeletalCluster_t), ClusterNodes, index);
    return index;
}

static void CreateSkeletonInPlace(const int structure, const ModelSkeleton_t* skel)
{
    SkeletonCreators[structure](skel->rootJoint, sizeof(M_SkeletalJoint_t), skel->rootNode, 0);
}

static void ClearSkeleton(ModelSkeleton_t* skel, const int root)
{
    for (int child = skel->rootJoint[root].children; child != ARRAYEDLISTNODE_NULL;
         child = skel->rootNode[child].next)
    {
        ClearSkeleton(skel, skel->rootNode[child].data);
        FreeNode(skel->rootNode, child);
    }
    skel->rootJoint[root].inUse = false;
}

// --- joint rotation (SetupJointRotations / FinishJointRotations) ---

static void TransformPlacement(matrix3_t rotation, vec3_t origin, Placement_t* placement)
{
    RotatePointAboutLocalOrigin(rotation, origin, placement->origin);
    RotatePointAboutLocalOrigin(rotation, origin, placement->direction);
    RotatePointAboutLocalOrigin(rotation, origin, placement->up);
}

static void RotateDecendents(ModelSkeleton_t* skel, M_SkeletalJoint_t* joint,
                             M_SkeletalJoint_t* ancestor)
{
    for (int jointChild = joint->children; jointChild != ARRAYEDLISTNODE_NULL;
         jointChild = skel->rootNode[jointChild].next)
    {
        joint = skel->rootJoint + skel->rootNode[jointChild].data;
        TransformPlacement(ancestor->rotation, ancestor->parent.origin, &joint->parent);
        RotateDecendents(skel, joint, ancestor);
    }
}

static void RotateJoint(ModelSkeleton_t* skel, M_SkeletalJoint_t* joint, const vec3_t angles)
{
    matrix3_t rotation, rotation2, toWorld, partialBackToLocal;

    vec3_t localAngles = VEC3_INIT(angles);
    localAngles[ROLL] += (float)Matricies3FromDirAndUp(joint->model.direction, joint->model.up,
                                                       toWorld, partialBackToLocal);

    memset(rotation, 0, sizeof(rotation));
    Matrix3FromAngles(localAngles, rotation);
    Matrix3MultByMatrix3(rotation, toWorld, rotation2);
    Matrix3MultByMatrix3(partialBackToLocal, rotation2, joint->rotation);

    VectorCopy(joint->model.origin, joint->parent.origin);

    Matrix3MultByVec3(joint->rotation, joint->model.direction, joint->parent.direction);
    Vec3ScaleAssign(10.0f, joint->parent.direction);
    Vec3AddAssign(joint->parent.origin, joint->parent.direction);

    Matrix3MultByVec3(joint->rotation, joint->model.up, joint->parent.up);
    Vec3ScaleAssign(10.0f, joint->parent.up);
    Vec3AddAssign(joint->parent.origin, joint->parent.up);

    RotateDecendents(skel, joint, joint);
}

static void SetupJointRotations(ModelSkeleton_t* skel, const int jointIndex, const int anglesIndex)
{
    const CL_SkeletalJoint_t* modelJointAngles = &ri.skeletalJoints[anglesIndex];
    M_SkeletalJoint_t* joint = &skel->rootJoint[jointIndex];

    if (joint->children != ARRAYEDLISTNODE_NULL) {
        int child2, jointChild;
        for (child2 = modelJointAngles->children, jointChild = joint->children;
             child2 != ARRAYEDLISTNODE_NULL;
             child2 = ri.jointNodes[child2].next, jointChild = skel->rootNode[jointChild].next)
        {
            if (jointChild == ARRAYEDLISTNODE_NULL) break;
            SetupJointRotations(skel, skel->rootNode[jointChild].data, ri.jointNodes[child2].data);
        }
    }
    RotateJoint(skel, joint, modelJointAngles->angles);
}

static void FinishJointRotations(ModelSkeleton_t* skel, const int jointIndex)
{
    matrix3_t rotation, rotation2, toWorld, partialBackToLocal;
    vec3_t localAngles;

    M_SkeletalJoint_t* joint = &skel->rootJoint[jointIndex];

    for (int jointChild = joint->children; jointChild != ARRAYEDLISTNODE_NULL;
         jointChild = skel->rootNode[jointChild].next)
        FinishJointRotations(skel, skel->rootNode[jointChild].data);

    Vec3SubtractAssign(joint->parent.origin, joint->parent.direction);
    Vec3SubtractAssign(joint->parent.origin, joint->parent.up);

    VectorNormalize(joint->parent.direction);
    VectorNormalize(joint->parent.up);

    localAngles[YAW] = 0.0f;
    localAngles[PITCH] = 0.0f;
    localAngles[ROLL] = (float)Matricies3FromDirAndUp(joint->parent.direction, joint->parent.up,
                                                      toWorld, partialBackToLocal);

    Matricies3FromDirAndUp(joint->model.direction, joint->model.up, toWorld, NULL);

    memset(rotation, 0, sizeof(rotation));
    Matrix3FromAngles(localAngles, rotation);
    Matrix3MultByMatrix3(rotation, toWorld, rotation2);
    Matrix3MultByMatrix3(partialBackToLocal, rotation2, joint->rotation);
}

// --- mesh vertex rotation (RotateModelSegments) ---

static void RotateModelSegment(const M_SkeletalJoint_t* joint, vec3_t* modelVerticies,
                               const vec3_t angles, const M_SkeletalCluster_t* modelCluster)
{
    matrix3_t toWorld, partialBackToLocal;
    vec3_t localAngles = VEC3_INIT(angles);
    localAngles[ROLL] += (float)Matricies3FromDirAndUp(joint->model.direction, joint->model.up,
                                                       toWorld, partialBackToLocal);

    matrix3_t rotation, rotation2;
    Matrix3FromAngles(localAngles, rotation);
    Matrix3MultByMatrix3(rotation, toWorld, rotation2);
    Matrix3MultByMatrix3(partialBackToLocal, rotation2, rotation);

    for (int i = 0; i < modelCluster->numVerticies; i++)
        RotatePointAboutLocalOrigin(rotation, joint->model.origin,
                                    modelVerticies[modelCluster->verticies[i]]);
}

static void RotateModelSegments(ModelSkeleton_t* skel, const int jointIndex,
                                const int modelClusterIndex, const int anglesIndex,
                                vec3_t* modelVerticies)
{
    const M_SkeletalCluster_t* modelCluster = &SkeletalClusters[modelClusterIndex];
    const CL_SkeletalJoint_t* modelJointAngles = &ri.skeletalJoints[anglesIndex];
    const M_SkeletalJoint_t* joint = skel->rootJoint + jointIndex;

    if (modelCluster->children != ARRAYEDLISTNODE_NULL) {
        int child, child2, jointChild;
        for (child = modelCluster->children, child2 = modelJointAngles->children,
             jointChild = joint->children;
             child != ARRAYEDLISTNODE_NULL && child2 != ARRAYEDLISTNODE_NULL;
             child = ClusterNodes[child].next, child2 = ri.jointNodes[child2].next,
             jointChild = skel->rootNode[jointChild].next)
        {
            if (jointChild == ARRAYEDLISTNODE_NULL) break;
            RotateModelSegments(skel, skel->rootNode[jointChild].data,
                                ClusterNodes[child].data, ri.jointNodes[child2].data,
                                modelVerticies);
        }
    }
    RotateModelSegment(joint, modelVerticies, modelJointAngles->angles, modelCluster);
}

// --- joint interpolation (LinearlyInterpolateJoints) ---

static void LerpVert3(const vec3_t a, const vec3_t b, vec3_t out,
                      const float move[3], const float front[3], const float back[3])
{
    for (int i = 0; i < 3; i++)
        out[i] = a[i] * front[i] + b[i] * back[i] + move[i];
}

static void LinearlyInterpolateJoints(ModelSkeleton_t* newSkel, const int newIndex,
                                      ModelSkeleton_t* oldSkel, const int oldIndex,
                                      ModelSkeleton_t* liSkel, const int liIndex,
                                      float move[3], float frontv[3], float backv[3])
{
    const M_SkeletalJoint_t* newJoint = &newSkel->rootJoint[newIndex];
    const M_SkeletalJoint_t* oldJoint = &oldSkel->rootJoint[oldIndex];
    M_SkeletalJoint_t* liJoint = &liSkel->rootJoint[liIndex];

    if (newJoint->children != ARRAYEDLISTNODE_NULL) {
        int newChild, oldChild, liChild;
        for (newChild = newJoint->children, oldChild = oldJoint->children, liChild = liJoint->children;
             newChild != ARRAYEDLISTNODE_NULL;
             newChild = newSkel->rootNode[newChild].next,
             oldChild = oldSkel->rootNode[oldChild].next,
             liChild  = liSkel->rootNode[liChild].next)
        {
            if (oldChild == ARRAYEDLISTNODE_NULL || liChild == ARRAYEDLISTNODE_NULL) break;
            LinearlyInterpolateJoints(newSkel, newSkel->rootNode[newChild].data,
                                      oldSkel, oldSkel->rootNode[oldChild].data,
                                      liSkel,  liSkel->rootNode[liChild].data,
                                      move, frontv, backv);
        }
    }

    LerpVert3(newJoint->model.origin,    oldJoint->model.origin,    liJoint->model.origin,    move, frontv, backv);
    LerpVert3(newJoint->model.direction, oldJoint->model.direction, liJoint->model.direction, move, frontv, backv);
    LerpVert3(newJoint->model.up,        oldJoint->model.up,        liJoint->model.up,        move, frontv, backv);

    Vec3SubtractAssign(liJoint->model.origin, liJoint->model.direction);
    Vec3SubtractAssign(liJoint->model.origin, liJoint->model.up);
    VectorNormalize(liJoint->model.direction);
    VectorNormalize(liJoint->model.up);
}

// ---------------------------------------------------------------------------
// .fm block parsing
// ---------------------------------------------------------------------------

void VK_Skel_ParseSkeleton(vk_model_skel_t* ms, const void* data, int num_frames,
                           int framesize, int* num_xyz)
{
    (void)framesize;
    const int* in_i = (const int*)data;

    ms->num_frames = num_frames;
    ms->skeletalType = *in_i;
    ms->rootCluster  = VK_Skel_CreateSkeleton(ms->skeletalType);
    if (ms->rootCluster < 0) { ms->skeletalType = SKEL_NULL; return; }

    const int num_clusters = *(++in_i);
    ms->numClusters = num_clusters;

    // Per-cluster vertex lists (cumulative, as in fmLoadSkeleton).
    int num_verts = 0;
    for (int cluster = num_clusters - 1; cluster > -1; cluster--) {
        num_verts += *(++in_i);
        const int cluster_index = ms->rootCluster + cluster;
        SkeletalClusters[cluster_index].numVerticies = num_verts;
        SkeletalClusters[cluster_index].verticies = malloc(num_verts * (int)sizeof(int));
    }

    int start_vert_index = 0;
    for (int cluster = num_clusters - 1; cluster > -1; cluster--) {
        for (int v = start_vert_index; v < SkeletalClusters[ms->rootCluster + cluster].numVerticies; v++) {
            const int vert_index = *(++in_i);
            for (int c = 0; c <= cluster; c++)
                SkeletalClusters[ms->rootCluster + c].verticies[v] = vert_index;
        }
        start_vert_index = SkeletalClusters[ms->rootCluster + cluster].numVerticies;
    }

    const int have_skeleton = *(++in_i);
    ms->haveSkeleton = have_skeleton;

    if (have_skeleton) {
        const float* in_f = (const float*)in_i;
        ModelSkeleton_t* skels = malloc(num_frames * (int)sizeof(ModelSkeleton_t));
        // Each frame's skeleton needs its own joint/node arrays. These MUST be
        // zero-initialized: the skeleton creators use GetFreeNode(), which scans
        // for entries with in_use==false. ref_gl1 uses Hunk_Alloc (zero-filled);
        // plain malloc leaves in_use garbage, so GetFreeNode returns wrong/-1
        // node indices and corrupts the joint children tree -> only the root
        // joint gets interpolated placement data, leaving child joints (upper
        // back, head) at origin/dir = 0 -> degenerate rotation -> neck stretch.
        M_SkeletalJoint_t* joint_pool =
            calloc((size_t)num_frames * numJointsInSkeleton[ms->skeletalType], sizeof(M_SkeletalJoint_t));
        ArrayedListNode_t* node_pool =
            calloc((size_t)num_frames * numNodesInSkeleton[ms->skeletalType], sizeof(ArrayedListNode_t));

        for (int i = 0; i < num_frames; i++) {
            skels[i].rootJoint = joint_pool + (size_t)i * numJointsInSkeleton[ms->skeletalType];
            skels[i].rootNode  = node_pool  + (size_t)i * numNodesInSkeleton[ms->skeletalType];
            CreateSkeletonInPlace(ms->skeletalType, &skels[i]);

            for (int c = 0; c < num_clusters; c++) {
                M_SkeletalJoint_t* j = &skels[i].rootJoint[c];
                j->model.origin[0] = *(++in_f); j->model.origin[1] = *(++in_f); j->model.origin[2] = *(++in_f);
                j->model.direction[0] = *(++in_f); j->model.direction[1] = *(++in_f); j->model.direction[2] = *(++in_f);
                j->model.up[0] = *(++in_f); j->model.up[1] = *(++in_f); j->model.up[2] = *(++in_f);
                VectorCopy(j->model.origin,    j->parent.origin);
                VectorCopy(j->model.direction, j->parent.direction);
                VectorCopy(j->model.up,        j->parent.up);
            }
        }
        ms->skeletons = skels;
    } else {
        // Skeleton data packed inline into the vertex array.
        if (num_xyz) *num_xyz -= num_clusters * 3;
    }
}

void VK_Skel_ParseReferences(vk_model_skel_t* ms, const void* data, int num_frames, int* num_xyz)
{
    // dmreferences_t { int referenceType; qboolean haveRefs; Placement_t refsForFrame[..] }
    const int* in_i = (const int*)data;
    ms->referenceType = in_i[0];
    ms->haveRefs      = in_i[1];

    if (ms->referenceType < 0 || ms->referenceType >= NUM_REFERENCED) {
        ms->referenceType = REF_NULL;
        return;
    }

    const int num_refs = numReferences[ms->referenceType];

    if (!ms->haveRefs) {
        if (num_xyz) *num_xyz -= num_refs * 3;
        return;
    }

    ms->refsForFrame = malloc((size_t)num_frames * num_refs * sizeof(Placement_t));
    if (num_frames < 1 || !ms->refsForFrame) return;

    // refsForFrame data begins after the two ints (referenceType, haveRefs).
    const Placement_t* ref_in = (const Placement_t*)(in_i + 2);
    memcpy(ms->refsForFrame, ref_in, (size_t)num_frames * num_refs * sizeof(Placement_t));
}

void VK_Skel_FreeModel(vk_model_skel_t* ms)
{
    if (!ms) return;
    if (ms->skeletons) {
        ModelSkeleton_t* skels = (ModelSkeleton_t*)ms->skeletons;
        // joint_pool/node_pool were single allocations starting at frame 0's arrays.
        if (skels[0].rootJoint) free(skels[0].rootJoint);
        if (skels[0].rootNode)  free(skels[0].rootNode);
        free(skels);
        ms->skeletons = NULL;
    }
    if (ms->refsForFrame) { free(ms->refsForFrame); ms->refsForFrame = NULL; }
    // Release this model's cluster block back to the shared pool.
    if (ms->skeletalType >= 0 && ms->rootCluster >= 0) {
        for (int c = 0; c < ms->numClusters; c++) {
            const int idx = ms->rootCluster + c;
            if (SkeletalClusters[idx].verticies) {
                free(SkeletalClusters[idx].verticies);
                SkeletalClusters[idx].verticies = NULL;
            }
            SkeletalClusters[idx].inUse = false;
            SkeletalClusters[idx].numVerticies = 0;
        }
    }
}

int VK_Skel_ReferenceType(const vk_model_skel_t* ms)
{
    if (!ms) return REF_NULL;
    return (ms->referenceType >= 0 && ms->referenceType < NUM_REFERENCED) ? ms->referenceType : REF_NULL;
}

// ---------------------------------------------------------------------------
// Per-frame reference lerp (port of r_SkeletonLerp.c LerpReferences)
// ---------------------------------------------------------------------------

static cvar_t* s_r_references = NULL;

static void ApplySkeletonToRef(LERPedReferences_t* refInfo, ModelSkeleton_t* skel,
                               Placement_t* placement, const int joint_index,
                               qboolean update_placement)
{
    const int joint_id = refInfo->jointIDs[joint_index];
    M_SkeletalJoint_t* joint = &skel->rootJoint[joint_id];

    TransformPoint(joint->rotation, joint->model.origin, joint->parent.origin, placement->origin);
    TransformPoint(joint->rotation, joint->model.origin, joint->parent.origin, placement->direction);
    TransformPoint(joint->rotation, joint->model.origin, joint->parent.origin, placement->up);

    // When the reference was lerped from the swapFrame (upper body), reposition
    // it from the swap skeleton's root to the current skeleton's root, so the
    // cast pose lands at the run pose's body position (ref_gl1 ApplySkeletonToRef).
    if (update_placement) {
        Vec3SubtractAssign(swap_skeleton.rootJoint->model.origin, placement->origin);
        Vec3AddAssign(cur_skeleton.rootJoint->model.origin, placement->origin);
    }
}

void VK_Skel_LerpReferences(vk_model_skel_t* ms, struct entity_s* e,
                            const vec3_t* lerped,
                            const byte* frames, int framesize,
                            const vec3_t move, const vec3_t frontv, const vec3_t backv,
                            float refdef_time)
{
    entity_t* ent = (entity_t*)e;
    LERPedReferences_t* refInfo = (LERPedReferences_t*)ent->referenceInfo;
    if (!refInfo || ms->referenceType == REF_NULL) return;
    if (ent->flags & RF_IGNORE_REFS) return;

    if (!s_r_references) s_r_references = ri.Cvar_Get("r_references", "1", 0);

    const float delta = refdef_time - refInfo->lastUpdate;
    refInfo->lastUpdate = refdef_time;

    const int num_refs = numReferences[ms->referenceType];

    int frame = ent->frame, oldframe = ent->oldframe;
    if (frame < 0 || frame >= ms->num_frames) frame = 0;
    if (oldframe < 0 || oldframe >= ms->num_frames) oldframe = 0;

    const qboolean do_joint_rot =
        (ms->skeletalType != SKEL_NULL && ms->haveSkeleton &&
         ent->rootJoint != NULL_ROOT_JOINT && ms->skeletons != NULL);

    // Move-and-cast: when swapFrame is active, references whose joint is at/above
    // swapCluster (the upper body: hands, staff, blade, head) follow the cast
    // (swapFrame) pose, not the lower-body run pose. Hands carry the bow charge
    // flame; without this branch they track the run animation and the effect
    // appears off to the side. Mirrors ref_gl1 LerpReferences.
    const qboolean have_swap = (do_joint_rot && ent->swapFrame != NO_SWAP_FRAME &&
                                frames != NULL);
    int sf = ent->swapFrame, osf = ent->oldSwapFrame;
    float smove[3], sfront[3], sback[3];

    ModelSkeleton_t* skels = (ModelSkeleton_t*)ms->skeletons;

    if (do_joint_rot) {
        float lmove[3], lfront[3], lback[3];
        for (int i = 0; i < 3; i++) { lmove[i] = move[i]; lfront[i] = frontv[i]; lback[i] = backv[i]; }

        cur_skeleton.rootJoint = cur_skeleton_joints;
        cur_skeleton.rootNode  = cur_skeleton_nodes;
        CreateSkeletonInPlace(ms->skeletalType, &cur_skeleton);
        LinearlyInterpolateJoints(&skels[frame], 0, &skels[oldframe], 0,
                                  &cur_skeleton, 0, lmove, lfront, lback);

        HACK_Pitch_Adjust = true;
        SetupJointRotations(&cur_skeleton, 0, ent->rootJoint);
        FinishJointRotations(&cur_skeleton, 0);

        if (have_swap) {
            if (sf < 0 || sf >= ms->num_frames) sf = 0;
            if (osf < 0 || osf >= ms->num_frames) osf = 0;
            ent->swapCluster = 0;

            // Swap-frame lerp vectors from the swapFrame's translate/scale.
            const skel_frame_t* pf  = (const skel_frame_t*)(frames + (size_t)sf  * framesize);
            const skel_frame_t* opf = (const skel_frame_t*)(frames + (size_t)osf * framesize);
            const float lerp = 1.0f - ent->backlerp;
            for (int i = 0; i < 3; i++) {
                smove[i]  = lerp * pf->translate[i] + ent->backlerp * opf->translate[i];
                sfront[i] = lerp * pf->scale[i];
                sback[i]  = ent->backlerp * opf->scale[i];
            }
            if (ent->scale != 1.0f && ent->scale > 0.001f)
                for (int i = 0; i < 3; i++) { smove[i]*=ent->scale; sfront[i]*=ent->scale; sback[i]*=ent->scale; }

            swap_skeleton.rootJoint = swap_skeleton_joints;
            swap_skeleton.rootNode  = swap_skeleton_nodes;
            CreateSkeletonInPlace(ms->skeletalType, &swap_skeleton);
            LinearlyInterpolateJoints(&skels[sf], 0, &skels[osf], 0,
                                      &swap_skeleton, 0, smove, sfront, sback);
            SetupJointRotations(&swap_skeleton, 0, ent->rootJoint);
            FinishJointRotations(&swap_skeleton, 0);
        }

        HACK_Pitch_Adjust = false;
    }

    for (int i = 0; i < num_refs; i++) {
        Placement_t* cur_placement = &refInfo->references[i].placement;
        Placement_t* old_placement = &refInfo->oldReferences[i].placement;
        qboolean update_placement = false;

        if (delta <= 1.0f)
            memcpy(old_placement, cur_placement, sizeof(Placement_t));

        const qboolean use_swap = have_swap && refInfo->jointIDs &&
                                  refInfo->jointIDs[i] >= ent->swapCluster;

        if (ms->refsForFrame) {
            if (!use_swap) {
                const Placement_t* fr  = &ms->refsForFrame[frame * num_refs + i];
                const Placement_t* ofr = &ms->refsForFrame[oldframe * num_refs + i];
                LerpVert3(fr->origin,    ofr->origin,    cur_placement->origin,    move, frontv, backv);
                LerpVert3(fr->direction, ofr->direction, cur_placement->direction, move, frontv, backv);
                LerpVert3(fr->up,        ofr->up,        cur_placement->up,        move, frontv, backv);
            } else {
                const Placement_t* fr  = &ms->refsForFrame[sf  * num_refs + i];
                const Placement_t* ofr = &ms->refsForFrame[osf * num_refs + i];
                LerpVert3(fr->origin,    ofr->origin,    cur_placement->origin,    smove, sfront, sback);
                LerpVert3(fr->direction, ofr->direction, cur_placement->direction, smove, sfront, sback);
                LerpVert3(fr->up,        ofr->up,        cur_placement->up,        smove, sfront, sback);
                update_placement = true;
            }
        } else if (lerped) {
            // References packed inline into the vertex array at [num_xyz_render + i*3].
            const int base = ms->num_xyz_render + i * 3;
            VectorCopy(lerped[base + 0], cur_placement->origin);
            VectorCopy(lerped[base + 1], cur_placement->direction);
            VectorCopy(lerped[base + 2], cur_placement->up);
        }

        if (do_joint_rot && refInfo->jointIDs && refInfo->jointIDs[i] != -1 &&
            s_r_references && (int)s_r_references->value)
        {
            ApplySkeletonToRef(refInfo, update_placement ? &swap_skeleton : &cur_skeleton,
                               cur_placement, i, update_placement);
        }

        if (delta > 1.0f)
            memcpy(old_placement, cur_placement, sizeof(Placement_t));
    }

    if (do_joint_rot) {
        ClearSkeleton(&cur_skeleton, 0);
        if (have_swap) ClearSkeleton(&swap_skeleton, 0);
    }
}

void VK_Skel_RotateMeshVerts(vk_model_skel_t* ms, struct entity_s* e, vec3_t* lerped,
                             const byte* frames, int framesize,
                             const vec3_t move, const vec3_t frontv, const vec3_t backv)
{
    entity_t* ent = (entity_t*)e;
    if (ms->skeletalType == SKEL_NULL || !ms->haveSkeleton || ms->skeletons == NULL) return;
    if (ent->rootJoint == NULL_ROOT_JOINT && ent->swapFrame == NO_SWAP_FRAME) return;

    static vec3_t swap_lerped[VKM_MAX_VERTS];

    ModelSkeleton_t* skels = (ModelSkeleton_t*)ms->skeletons;
    const int num_xyz = ms->num_xyz_render;

    int frame = ent->frame, oldframe = ent->oldframe;
    if (frame < 0 || frame >= ms->num_frames) frame = 0;
    if (oldframe < 0 || oldframe >= ms->num_frames) oldframe = 0;

    // Which cluster is the swap (upper-body) cluster. ref_gl1 forces 0.
    const M_SkeletalCluster_t* swap_cluster = &SkeletalClusters[ms->rootCluster + 0];

    float lmove[3], lfront[3], lback[3];
    for (int i = 0; i < 3; i++) { lmove[i]=move[i]; lfront[i]=frontv[i]; lback[i]=backv[i]; }

    // --- swapFrame path: blend the cast (upper body) pose over the run pose ---
    qboolean have_swap = (ent->swapFrame != NO_SWAP_FRAME);
    if (have_swap) {
        int sf = ent->swapFrame, osf = ent->oldSwapFrame;
        if (sf < 0 || sf >= ms->num_frames) sf = 0;
        if (osf < 0 || osf >= ms->num_frames) osf = 0;
        ent->swapCluster = 0;

        swap_skeleton.rootJoint = swap_skeleton_joints;
        swap_skeleton.rootNode  = swap_skeleton_nodes;
        CreateSkeletonInPlace(ms->skeletalType, &swap_skeleton);

        const skel_frame_t* pf  = (const skel_frame_t*)(frames + (size_t)sf  * framesize);
        const skel_frame_t* opf = (const skel_frame_t*)(frames + (size_t)osf * framesize);

        const float lerp = 1.0f - ent->backlerp;
        float smove[3], sfront[3], sback[3];
        for (int i = 0; i < 3; i++) {
            smove[i]  = lerp * pf->translate[i] + ent->backlerp * opf->translate[i];
            sfront[i] = lerp * pf->scale[i];
            sback[i]  = ent->backlerp * opf->scale[i];
        }
        if (ent->scale != 1.0f && ent->scale > 0.001f) {
            for (int i = 0; i < 3; i++) { smove[i]*=ent->scale; sfront[i]*=ent->scale; sback[i]*=ent->scale; }
        }

        // Lerp the swap frame's verts, then copy only the cluster (upper body)
        // verts into s_lerped, replacing the run-pose upper body with the cast.
        const int lim = (num_xyz > VKM_MAX_VERTS) ? VKM_MAX_VERTS : num_xyz;
        for (int vi = 0; vi < lim; vi++)
            for (int k = 0; k < 3; k++)
                swap_lerped[vi][k] = (float)pf->verts[vi].v[k] * sfront[k]
                                   + (float)opf->verts[vi].v[k] * sback[k] + smove[k];

        for (int i = 0; i < swap_cluster->numVerticies; i++) {
            const int vi = swap_cluster->verticies[i];
            if (vi >= 0 && vi < VKM_MAX_VERTS) VectorCopy(swap_lerped[vi], lerped[vi]);
        }

        LinearlyInterpolateJoints(&skels[sf], 0, &skels[osf], 0,
                                  &swap_skeleton, 0, smove, sfront, sback);
    }

    // --- current skeleton (run pose body) ---
    if (ent->rootJoint != NULL_ROOT_JOINT || have_swap) {
        cur_skeleton.rootJoint = cur_skeleton_joints;
        cur_skeleton.rootNode  = cur_skeleton_nodes;
        CreateSkeletonInPlace(ms->skeletalType, &cur_skeleton);
        LinearlyInterpolateJoints(&skels[frame], 0, &skels[oldframe], 0,
                                  &cur_skeleton, 0, lmove, lfront, lback);
    }

    // --- DoSkeletalRotations ---
    if (ent->rootJoint == NULL_ROOT_JOINT) {
        if (have_swap) ClearSkeleton(&swap_skeleton, 0);
        return;
    }

    if (have_swap) {
        RotateModelSegments(&swap_skeleton, 0, ms->rootCluster, ent->rootJoint, lerped);
        // Reposition the swapped cluster from the swap body origin onto the
        // current body origin.
        for (int i = 0; i < swap_cluster->numVerticies; i++) {
            const int vi = swap_cluster->verticies[i];
            if (vi < 0 || vi >= VKM_MAX_VERTS) continue;
            Vec3SubtractAssign(swap_skeleton.rootJoint->model.origin, lerped[vi]);
            Vec3AddAssign(cur_skeleton.rootJoint->model.origin, lerped[vi]);
        }
        ClearSkeleton(&swap_skeleton, 0);
    } else {
        RotateModelSegments(&cur_skeleton, 0, ms->rootCluster, ent->rootJoint, lerped);
    }

    ClearSkeleton(&cur_skeleton, 0);
}
