# Vulkan Renderer (ref_vk) - Outstanding Work

Status as of v32: 2D complete, textured static world complete (fullbright).

## Done
- [x] Vulkan bootstrap (instance/device/swapchain/renderpass/depth)
- [x] 2D: menus, console, HUD, conchars, real big-font, books, cinematics
- [x] 3D static world geometry with diffuse textures (per-texture batched)
- [x] Depth testing, correct camera/view matrix
- [x] 2D composites correctly over 3D (pipeline_2d_bound tracking)

## TODO (rough priority order)

### Models / entities (IN PROGRESS - started after v32)
- [x] H2 flex model (.fm) format loader (non-skeletal)
- [x] Frame interpolation (non-skeletal models)
- [ ] Skeletal animation (player + monsters with skeletons)
- [x] Model rendering pipeline (per-entity transform matching R_RotateForEntity)
- [x] Entity list traversal (regular + alpha_entities) (R_DrawEntitiesOnList equivalent)
- [x] Items/props/monsters (non-skeletal)
- [ ] Player model + skeletal monsters
- [x] Sprites (billboarded .sp2 - pickups, effects)
- [ ] Model skins/textures

### Lightmaps (DEFERRED - do after models)
- [x] Parse LIGHTING lump
- [x] CalcSurfaceExtents (texturemins/extents per surface)
- [x] Lightmap atlas allocation (2048x2048 shelf-pack)
- [x] Second UV set for lightmap sampling
- [x] Multiply diffuse * lightmap in world fragment shader
- [x] Update world shader to 2 samplers (diffuse + lightmap)

### Other world features (DEFERRED)
- [ ] Sub-models (doors, lifts, moving brush models)
- [ ] Sky boxes (currently SURF_SKY filtered out)
- [ ] Water/lava surfaces (SURF_WARP, animated UVs)
- [x] Translucent entities + sprites (alpha blend pipeline)
- [ ] PVS culling (currently draw everything every frame)
- [ ] Frustum culling

### Effects (DEFERRED - largest)
- [ ] Particle system
- [ ] Dynamic lights
- [ ] client_effects integration
- [ ] Sprites, beams

## Known shortcuts to revisit
- World draws every surface every frame (no culling)
- One VkDeviceMemory allocation per resource (no sub-allocator)
- Descriptor pools fixed size (4096 2D / 1024 world)


## v46 (this session)
- FIXED: sprite frame textures - prepend "Sprites/" to frame name (matches GL1
  Mod_LoadSpriteModel). ALL effect sprites were failing to load (coronas, halos,
  crosshair, blood splat, lens flares, spell projectiles, water fx). Root cause:
  .sp2 frame names are stored relative to Sprites/ (e.g. "fx/halo_0.m8").
- FIXED: 2D quad batcher limit 8192 -> 16384 quads/frame (loading screen overflow).

## NEXT MAJOR FEATURE: Skeletal animation (player + monsters)
The player upper body does not bend when casting/aiming while running. H2 uses a
layered skeletal system:
- entity_t.rootJoint indexes into a shared skeletal_joints[] array (client-side,
  cl_skeletons.c computes joint angles each frame: head, upperback, lowerback
  pitch/roll from playerinfo->targetjointangles).
- The .fm "skeleton" block holds the bone hierarchy; vertices have bone weights.
- Rendering must: read joint angles for this entity's rootJoint, build bone
  matrices, CPU-skin vertices, then draw.
Currently vk_model.c renders the raw vertex frame (no skeleton), so the torso
never bends. This is the cause of the "casting animation plays lower-body run"
report. Substantial: needs skeleton parse + per-vertex bone data + joint matrix
application. GL1 reference: gl1_FlexModel.c skeletal path + cl_skeletons.c.

## v50 (this session)
- FIXED (MAJOR): monster AI "can't see player" under Vulkan. Root cause: the
  renderer must sample world lighting at the player position each frame and
  publish it via the r_lightlevel cvar; the client packs it into cmd.lightlevel,
  the server stores client->light_level, and g_AI.c FindTarget uses it (player
  invisible to monsters if light_level <= 5; randomized threshold < 77 below
  SKILL_HARD). VK never set r_lightlevel -> stayed 0 -> monsters blind until hit.
  Added vk_lightpoint.c: parses BSP planes/nodes/faces/lighting lumps and ports
  ref_gl1 R_RecursiveLightPoint/R_LightPoint; R_RenderFrame sets r_lightlevel
  from VK_LightPoint_Sample(clientmodelorg) * 150, matching gl1 R_SetLightLevel.
- FIXED: bloody/damaged NPC skins. Flex-model skin-name stride was
  FMDL_BLOCK_IDENT_SIZE (32); correct stride is MAX_FRAMENAME (64) per ref_gl1
  fmLoadSkin. Wrong stride read later skin names from wrong offsets, loading
  damage-variant skins instead of the base skin.

## STILL PENDING
- Skeletal animation (casting torso bend + player foot shadows): the shadow
  reference points (CORVUS_LEFTFOOT etc) need owner->referenceInfo from the
  skeletal reference system (re.GetReferencedID stubbed). Same root as the
  casting-animation upper/lower body split. Next major feature.
- Shadow model (RF_ALPHA_TEXTURE): main body shadow still needs verification;
  v48 diagnostic logs translucent/alpha-textured models - check the log.

## v51 (this session)
- FIXED: func_button (and other brush submodels) texture state swap on press.
  Buttons set s.frame (0=up, 1=down); brush surfaces animate via the texinfo
  nexttexinfo chain (numframes), indexed by entity frame. world_batch_t now
  stores frame_desc[] resolved from the chain; VK_World_RenderSubmodel takes an
  ent_frame and picks the frame descriptor. Matches gl1 R_TextureAnimation.

## CONFIRMED: staff effects ALSO blocked by skeletal references
- fx_Staff.c StaffLevelNUpdate bail at RefPointsValid(owner) == false, and read
  owner->referenceInfo->references[CORVUS_RIGHTHAND] / [ref_index] to spawn the
  swoosh/trail sprites. With referenceInfo NULL (refs stubbed), staff effects
  never spawn. SAME root cause as casting torso-bend and player foot shadows.
  => Skeletal reference system unblocks: casting animation, foot shadows,
     staff/weapon trail effects. This is now clearly the top-value feature.

## SKELETAL / REFERENCE SYSTEM - FULL ARCHITECTURE MAP (for implementation)

### Goal
Generate entity->referenceInfo (LERPedReferences_t) each frame so client effects
can read references[CORVUS_RIGHTHAND] etc. Unblocks: staff/weapon trails (fx_Staff),
player foot shadows (fx_Shadow), casting torso bend (skeletal joint rotation of verts).

### Data sources (all already parsed/available)
- refimport ri.skeletalJoints (CL_SkeletalJoint_t[]) and ri.jointNodes
  (ArrayedListNode_t[]) - ALREADY wired by client (vid_dll.c:225-226), available
  in ref_vk's ri. These hold per-entity joint angles (set by server/client via
  cl_skeletons.c + netmsg). Index by e->rootJoint.
- qcommon shared code already linked into ref_vk: Skeletons.c (SkeletonCreators[],
  numJointsInSkeleton[], numNodesInSkeleton[]), Reference.c (numReferences[],
  jointIDs[]), Matrix.c (Matrix3FromAngles, Matrix3MultByMatrix3, Matrix3MultByVec3,
  Matricies3FromDirAndUp, RotatePointAboutLocalOrigin, TransformPoint), ArrayedList.

### .fm blocks to parse (currently skipped in vk_model.c) - per ref_gl1 gl1_FlexModel.c
- "skeleton" (fmLoadSkeleton): reads skeletalType (int), num_clusters, per-cluster
  vertex lists -> SkeletalClusters[rootCluster+c].verticies. rootCluster =
  CreateSkeleton(skeletalType). Then have_skeleton flag; if set, per-frame per-cluster
  Placement (origin/dir/up) -> fmdl->skeletons[frame].rootJoint[c].model/.parent.
  If NOT set, num_xyz -= num_clusters*3 (skeleton packed into vertex array).
- "references" (fmLoadReferences): referenceType (int), haveRefs flag,
  refsForFrame[num_frames * numReferences[refType]] Placement_t. If !haveRefs,
  num_xyz -= numReferences*3 (packed into verts). CORVUS refType=REF_CORVUS=0,
  numReferences=NUM_REFERENCES_CORVUS=7 (LHAND,RHAND,LFOOT,RFOOT,STAFF,BLADE,HELL_HEAD).
- NOTE ORDER MATTERS: skeleton/references subtract from num_xyz when not inline, so
  frame vertex parsing must account for this. Player tris.fm: verify which path.

### Per-frame computation (port ref_gl1 Skeletons/r_SkeletonLerp.c FrameLerp path)
1. re.GetReferencedID(model) must return fmdl->referenceType (REF_CORVUS) - currently
   stubbed to -1. Fix: return parsed referenceType. This makes cl_entities.c allocate
   ent->referenceInfo = AllocateLERPedReference(REF_CORVUS).
2. In model draw (FrameLerp equiv): if referenceInfo != NULL and !RF_IGNORE_REFS,
   call LerpReferences:
   - delta = time - lastUpdate; lastUpdate = time
   - if rootJoint != NULL_ROOT_JOINT: SetupJointRotations + FinishJointRotations on
     cur_skeleton (applies ri.skeletalJoints[rootJoint] angles to the joint tree)
   - for each ref i: lerp refsForFrame[frame*n+i] vs [oldframe*n+i] by backlerp
     (R_LerpVert with move/front/back from frame scale/translate) -> references[i].placement
   - if jointIDs[i] != -1 and r_references: ApplySkeletonToRef (rotate placement by
     the joint's rotation matrix about joint origin) - THIS is the upper-body bend
   - copy to oldReferences when delta>1 (or before, when delta<=1)
3. Need globals mirroring r_SkeletonLerp.c: cur_skeleton/swap_skeleton (ModelSkeleton_t
   with rootJoint[MAX_JOINTS_PER_SKELETON], rootNode[]), sfl_cur_skel (front/back vecs),
   cur_skel_move. CreateSkeletonInPlace(skeletalType, &cur_skeleton) each frame.
4. r_references cvar (ri.Cvar_Get "r_references" "1") gates ApplySkeletonToRef.

### Vertex skinning for casting bend (DoSkeletalRotations -> RotateModelSegments)
- After lerping verts (s_lerped), if skeletalType != SKEL_NULL and rootJoint set,
  RotateModelSegments(cur_skeleton, 0, rootCluster, rootJoint, s_lerped) rotates the
  upper-body cluster verts by the joint angles. This is the torso bend on the MESH.
- This requires my CPU vertex path to use s_lerped[] (rotated) instead of raw frame
  lerp for skeletal models. Integrate into vk_model.c DecodeLerpVert path.

### Player specifics
- Player ent->rootJoint set from server skeletal data (modelindex 255 path). When
  rootJoint == NULL_ROOT_JOINT (-1), no bend, refs still lerp from frame data (static
  pose refs) - so staff/shadows work even before bend is perfect.
- skeletalType for players/male/tris.fm = SKEL_CORVUS; referenceType = REF_CORVUS.

### Implementation files (new)
- vk_skeleton.c/.h: wrap the qcommon skeleton creators + port r_Skeletons.c rotation
  (SetupJointRotations/FinishJointRotations/RotateModelSegments/LinearlyInterpolateJoints
  - OR call qcommon if it exposes them; r_Skeletons.c is ref_gl1-local, so port it).
  NOTE: r_Skeletons.c uses ri.skeletalJoints/ri.jointNodes and HACK_Pitch_Adjust
  (global in Matrix). Matricies3FromDirAndUp is in H2Common Matrix.c - verify signature.
- Extend vk_model.c fmdl parse for skeleton/references blocks; add referenceInfo lerp
  in draw; fix R_RegisterModel/GetReferencedID to expose referenceType.

### Verify against GL1 (later audit)
- HACK_Pitch_Adjust usage, R_LerpVert signature, MAX_FRAMENAME strides, the num_xyz
  subtraction ordering, swapFrame path (can defer swapFrame - only used for some anims).

## v52 (this session) - SKELETAL / REFERENCE SYSTEM implemented
Created vk_skeleton.c/.h: faithful port of ref_gl1 Skeletons/r_Skeletons.c +
r_SkeletonLerp.c (reference path). Wired into vk_model.c + vk_main.c.
- Parse "skeleton" + "references" .fm blocks (VK_Skel_ParseSkeleton/References);
  tracks num_xyz_full vs num_xyz_render for inline-packed ref/skel verts.
- re.GetReferencedID now returns the model's real referenceType (was -1), so the
  client allocates entity->referenceInfo. Path: vk_main R_GetReferencedID ->
  VK_Model_ReferenceType -> VK_Skel_ReferenceType.
- Per-frame in VK_Model_DrawEntity: pre-lerp all verts into s_lerped[], rotate
  upper-body cluster by joint angles (VK_Skel_RotateMeshVerts = casting bend),
  and compute reference placements (VK_Skel_LerpReferences) into referenceInfo.
  s_refdef_time published via VK_Model_SetFrameTime(fd->time) each frame, used to
  stamp referenceInfo->lastUpdate (what RefPointsValid checks).
- Skeleton structs (M_SkeletalJoint_t/ModelSkeleton_t/M_SkeletalCluster_t) mirror
  ref_gl1 m_Skeleton.h/m_SkeletalCluster.h, defined in vk_skeleton.h. Shared
  SkeletonCreators[]/numReferences[]/jointIDs[]/Matrix math from qcommon+H2Common.

### NEEDS TESTING / VERIFY against GL1
- Staff trail effects (fx_Staff RefPointsValid now passes?).
- Player foot shadows (fx_Shadow CORVUS_LEFTFOOT/RIGHTFOOT refs).
- Casting upper-body torso bend (VK_Skel_RotateMeshVerts mesh deform).
- Console should log "model 'X' skeletal: skelType=5 refType=0 ..." for player.
- Watch for: s_lerped scale double-apply (scale baked in move/front/back, NOT
  re-applied in XformPoint for skeletal path - verify visually no size change).
- swapFrame path NOT yet ported (only some anims use it; refs/bend work without).
- Verify num_xyz_full correct: if model has BOTH skeleton(inline) AND refs(inline),
  both subtract; num_xyz_full captured before first subtraction.

## v53 (fix) - ref_vk was missing from renderer list under v52
ROOT CAUSE: vk_skeleton.c references numNodesInSkeleton/numJointsInSkeleton/
SkeletonCreators/numReferences/jointIDs (defined in qcommon/Skeletons.c +
Reference.c). Those were NOT linked into the ref_vk target (only into the main
binary/game/client_effects). Result: libref_vk.so had an undefined symbol
(numNodesInSkeleton), so the engine's renderer-scan probe (which dlopens each
libref_*.so with RTLD_NOW and calls GetRefAPI) silently failed to load it -> vk
absent from the video menu / vid_ref list. v52 binaries were thus unusable; none
of the skeletal code actually ran.
FIX: added qcommon/Skeletons.c + qcommon/Reference.c to the ref_vk source list in
CMakeLists.txt. Verified libref_vk.so now dlopens with RTLD_NOW and the GetRefAPI
probe returns api_version=4 + valid title, matching ref_gl1.

## v54 (this session) - fix upper-body stretch + implement swapFrame (move+cast)
TWO fixes to the skeletal mesh deform:
1. STRETCH BUG: VK_Skel_RotateMeshVerts interpolated the joints with a plain
   unit lerp (move=0, front=1-backlerp, back=backlerp) while the VERTS were
   lerped with the frame scale/translate baked in. So joint->model.origin (the
   RotateModelSegment pivot) was in a different coord space than the verts ->
   upper body sheared off when aim/camera angle changed. FIX: pass the SAME
   move/front/back frame-lerp vectors (incl. e->scale) used for the verts into
   LinearlyInterpolateJoints. Now pivot and verts share one space.
2. MOVE-AND-CAST: ported the swapFrame upper/lower body split (was deferred).
   The player runs via 'frame' (lower body) while the cast pose plays via
   'swapFrame' (upper body). Full port of LerpStandardSkeleton swap branch +
   DoSkeletalRotations swap branch: lerp swap-frame verts, copy only the upper
   cluster verts over the run pose, interpolate swap_skeleton, rotate the swap
   cluster, then offset verts by (cur_skeleton.origin - swap_skeleton.origin).
   VK_Skel_RotateMeshVerts now takes frames+framesize to read swap-frame verts.
   Added swap_skeleton statics + skel_frame_t on-disk frame layout.
CONFIRMED WORKING in v53: staff trail effects now spawn (references valid).

## Remaining skeletal edge cases (for GL1 audit)
- LerpReferences swap-skeleton ref selection: GL1 runs SetupJointRotations/
  FinishJointRotations on swap_skeleton too, and ApplySkeletonToRef picks
  swap vs cur per ref based on jointID < swapCluster. Current VK only rotates
  refs via cur_skeleton. Staff/blade/hand refs (jointID=UPPERBACK) work; verify
  no ref needs the swap skeleton specifically.

## v55 (this session) - FIX neck/head stretch: zero-init skeleton node pools
Diagnostic (v55-diag) revealed: in RotateModelSegment, only the ROOT joint
(lowerback) had valid model.origin/direction; upperback + head joints had
origin=(0,0,0) dir=(0,0,0) -> degenerate Matricies3FromDirAndUp -> verts smear
toward origin (the neck/head stretch).
ROOT CAUSE: VK_Skel_ParseSkeleton allocated the per-frame joint_pool/node_pool
with malloc (uninitialized). The skeleton creators call GetFreeNode(), which
scans for nodes with in_use==false. With garbage in_use flags, GetFreeNode
returned wrong/-1 indices, corrupting the joint children tree, so
LinearlyInterpolateJoints' recursion never reached the child joints (upperback,
head) -> they kept zero placements. ref_gl1 uses Hunk_Alloc (page-zeroed), so it
never hit this. FIX: calloc the joint_pool and node_pool. Removed diagnostics.

## v56 (this session) - fix near-plane clipping (walls see-through up close)
SKELETAL SYSTEM CONFIRMED WORKING by user (v55).
BUG: under VK, world surfaces the camera was close to would clip away (you could
see through a nearby wall edge). ROOT CAUSE: both projection matrices used a
near plane of 4.0 (Quake2's default) and far 8192. ref_gl1 R_SetPerspective
(gl1_Main.c:460) uses near=1.0 with an explicit "// Q2: 4.0" comment - H2
lowered it - and far = r_farclipdist (default 4096). So surfaces 1..4 units from
the camera drew under GL but were near-clipped under VK.
FIX: vk_main.c BuildViewMVP and vk_world.c VK_World_Render both now use near=1.0,
far=4096 (vk_main reads r_farclipdist cvar, default 4096). Registered
r_farclipdist in R_Init.

## v57 (this session) - water/warp transparency + animation
Implemented the SURF_WARP/SURF_TRANS33/66/SURF_FLOWING/SURF_UNDULATE surface
class, faithful to ref_gl1 R_EmitWaterPolys + R_DrawAlphaSurfaces.
- World builder classifies each texture batch's surf_flags + stores RAW texel
  s,t (not normalized) for warp surfaces (EmitFaceEx warp arg). Opaque surfaces
  unchanged.
- New shaders world_warp.vert/.frag: turbsin water UV done per-FRAGMENT (smooth
  without the GL1 R_SubdivideSurface grid - we don't subdivide), undulate Z-bob
  per-vertex, SURF_FLOWING scroll. turbsin[i] == 8*sin(i*2pi/256) so shader uses
  8*sin(x) exactly. Fullbright (no lightmap) per H2 SURF_FULLBRIGHT.
- New warp pipeline (vk_pipeline_world warp_pipeline/warp_layout): alpha blend
  SRC_ALPHA/ONE_MINUS_SRC_ALPHA, depth-test ON depth-write OFF, push constant
  mat4 + 2 vec4 (time/alpha/flowing/undulate/isWarp).
- VK_World_Render now 2 passes: opaque (surf_flags==0) then warp/translucent.
  Alpha from gl_trans33/gl_trans66 cvars (TRANS33=0.33, TRANS66=0.66 default),
  matching GL. Plain warp water/lava = opaque (alpha 1).

### KNOWN GAPS (for GL1 audit)
- Submodel water (water on a moving brush submodel) still renders opaque via the
  non-warp EmitFace path - VK_World_RenderSubmodel has no warp handling. Rare.
- No R_SubdivideSurface: undulate Z-bob only moves face corners (coarse) since
  warp faces aren't subdivided into a grid. UV warp is per-fragment so unaffected.
  If undulate looks blocky on large surfaces, port R_SubdivideSurface.

## v58 (this session) - fix water over-distortion: subdivide + per-vertex warp
PROBLEM (v57): water looked chaotic/swirly under VK vs calm in GL. ROOT CAUSE:
v57 did the turbsin UV warp PER-FRAGMENT using absolute texel coords, so the
sine swept many full periods across each surface -> dense swirl. ref_gl1 does it
PER-VERTEX on a surface subdivided into a <=64-unit grid (R_SubdivideSurface),
so each vertex's offset is bounded to +/-8 texels and the GPU interpolates
smoothly across each cell -> gentle ripple.
FIX:
- Ported R_SubdivideSurface/R_SubdividePolygon/BoundPoly into vk_world.c. Warp
  faces are now subdivided into 64-unit grid fans at load (EmitFaceEx warp path
  -> SubdividePolygon -> EmitWarpPoly), storing raw texel s,t per grid vertex.
- Moved the turbsin warp + undulate from the fragment shader to the VERTEX
  shader (world_warp.vert), matching GL1 exactly. Fragment just samples.
- Grew total_drawable_verts estimate to account for warp subdivision (grid
  cells * 24 + pad) so the VBO doesn't overflow.
- Removed leftover particle-count diagnostic spam (vk_particles.c) and the
  RotateModelSegment skeletal diagnostic earlier.

## v59 (this session) - fix water draw order (things drawn ON TOP of water)
BUG: entities/floor/submerged objects drew OVER the translucent water instead of
being seen through it. ROOT CAUSE: the water (translucent, depth-write OFF) pass
ran INSIDE VK_World_Render, i.e. BEFORE entities/submodels. Since water doesn't
write depth, anything drawn after it (all entities) passed the depth test
against the opaque floor behind the water and painted over it.
FIX: split the translucent/warp pass out into VK_World_RenderWater(fd), called
in R_RenderFrame AFTER world opaque + entities + alpha entities, right before
particles - exactly matching ref_gl1 R_RenderView order (R_DrawWorld ->
R_DrawEntitiesOnList -> depthMask(FALSE) -> R_SortAndDrawAlphaSurfaces ->
particles -> depthMask(TRUE)).
NOTE: water "churn" may look slightly busier than GL because GL quantizes
turbsin to 256 discrete steps (& 255) while the VK shader uses continuous
sin(); mine is smoother/more-correct, not more violent. Not chased further.
TODO if needed: alpha surfaces aren't sorted back-to-front yet (GL1
R_SortAndDrawAlphaSurfaces sorts); fine for non-overlapping water planes.

## v60 (this session) - full-screen polyblend tint (underwater / damage / pickups)
BUG: underwater the VK view was a flat washed-out uniform blue (lost contrast),
unlike GL's tinted-but-still-readable view. ROOT CAUSE: the VK renderer ignored
fd->blend - the engine's full-screen RGBA tint used for underwater, lava, damage
flashes, item pickups, and powerups. (The flat look was just the world's own
fullbright water-fog look with no actual blend overlay; GL applies the blend as
a translucent fullscreen quad.)
FIX: ported ref_gl1 R_PolyBlend. At the end of R_RenderFrame, if fd->blend[3]>0
and gl_polyblend != 0, draw a fullscreen 2D fill (VK_Draw_Fill over fd->x/y/w/h)
tinted by fd->blend (RGBA 0-1 -> bytes). The 2D quad pipeline already blends
SRC_ALPHA/ONE_MINUS_SRC_ALPHA. Gated by gl_polyblend cvar like GL1; client also
gates via cl_add_blend. This covers ALL screen tints (underwater, damage, bonus
flash, powerups), not just water.

## v61 (this session) - CORRECT underwater tint: screen flash, not fd->blend
v60 was the wrong source. In H2R, fd->blend is never set client-side, so the
v60 polyblend was inert (hence the near-invisible tint). The real underwater
effect is ref_gl1's RI_RenderFrame + R_ScreenFlash:
  - if cl_camera_under_surface != 0: color.c = r_underwater_color (0x70c06000 =
    little-endian RGBA r=0 g=96 b=192 a=112 -> blue at ~44% alpha)
  - else: color.c = ri.Is_Screen_Flashing() (client damage/pickup/powerup flash)
  - draw fullscreen via Draw_FadeScreen, then ri.Deactivate_Screen_Flash()
FIX: replaced the fd->blend polyblend in R_RenderFrame with this exact logic
using VK_Draw_FadeScreen + ri.Is_Screen_Flashing/Deactivate_Screen_Flash and the
cl_camera_under_surface / r_underwater_color cvars.
NOTE: GL1 ALSO enables GL distance fog underwater (R_WaterFog, GL_EXP density
0.0015) on top of the flash, which fades distant geometry into the water color.
Not yet ported (would need fog in the world/entity shaders). The screen flash is
the dominant visible effect; if depth-fade is wanted later, add per-fragment fog
gated on cl_camera_under_surface. Logged for the GL audit.

## v62 AUDIT - remaining GL1 R_RenderView effects vs VK (pre-fog sweep)
Walked the full ref_gl1 R_RenderView sequence. Status of each stage in VK:
- R_PushDlights / R_RenderDlights = DYNAMIC LIGHTS: NOT DONE (deferred, perf+visual).
- R_SetFrustum = frustum culling: NOT DONE (deferred, perf only - no visual diff).
- R_MarkLeaves = PVS culling: NOT DONE (deferred, perf only - no visual diff).
- R_DrawWorld / R_DrawEntitiesOnList / R_SortAndDrawAlphaSurfaces (water) /
  R_DrawParticles / R_PolyBlend(screen flash): ALL DONE (v59-v61).
- r_fog (general, non-underwater): default 0/off - not a gap.
Two genuine per-entity VISUAL gaps found in R_DrawFlexModel:
- RF_REFLECTION (0x2): GL spherical env-map reflection. USED by g_Shrine.c
  (weapon shrines look shiny), p_View, g_Debris/fx_Debris. VK does NOT handle it
  (vk_model.c has no RF_REFLECTION / FMNI_USE_REFLECT path) -> shrines render
  flat. Needs a sphere-map texgen shader path. MEDIUM priority, deferred.
- RF_TRANS_GHOST (0x10000): GL1 sets alpha = shadelight[0]*0.5 (lighting-based
  ghost alpha). VK routes RF_TRANS_ANY -> translucent pipeline but uses entity
  color alpha, not the shadelight*0.5 ghost formula -> ghost opacity slightly
  off. LOW priority, deferred.
Everything else in the frame path matches. Proceeding to underwater fog.

## v62 - underwater distance fog (+ audit above)
Ported ref_gl1 R_WaterFog as per-fragment distance fog in the world, warp, and
entity shaders. Gated on cl_camera_under_surface; reads r_fog_underwater_mode
(0 LINEAR/1 EXP/2 EXP2, default 1), r_fog_underwater_density (0.0015),
r_fog_underwater_startdist (100), r_fog_underwater_color_r/g/b (1,1,1),
r_farclipdist (4096 for LINEAR end). Fog factor: LINEAR (end-d)/(end-start),
EXP exp(-density*d), EXP2 exp(-(density*d)^2); col = mix(fogcolor, col, f).
Distance d = length(worldpos - camera).
Implementation:
- world.vert/frag: push constant grew 64 -> 112 (mat4 + 3 vec4 fog), VERTEX|FRAG.
- entity.vert/frag: push 80 -> 128 (mat4 + tint + 3 vec4 fog). Entity verts are
  pre-transformed to world space CPU-side, so fog cam = view origin. New
  VK_Model_SetViewOrigin(fd->vieworg) published each frame.
- world_warp.vert/frag: push 96 -> 144 (mat4 + 2 vec4 warp + 3 vec4 fog).
- FillFogParamsAt(pc, off, cam) shared helper in vk_world.c; Model_FillFogParams
  in vk_model.c.
The white default fog color + the blue screen flash (v61) combine to the GL
underwater look (distance fades, whole screen blue-tinted).
KNOWN: warp push constant is 144 bytes (> 128 guaranteed min). Fine on the
target Iris Xe (256) and all modern desktop GPUs; could fail warp pipeline
creation on an old GPU with maxPushConstantsSize==128. If needed, move fog
params to a UBO. Submodels (doors/brush water) skip fog (verts are model-space);
minor, logged.

## v63 - FIX v62 regression: push-constant stage/size mismatch (sky + crosshair)
v62 grew vk_pipeline_3d.layout to 128 bytes VERTEX|FRAGMENT (entity fog) but only
updated the entity model draw. The SKY (vk_sky.c) and SPRITES/crosshair
(vk_sprite.c) share that layout and still pushed 80 bytes VERTEX-only ->
VUID-vkCmdPushConstants-offset-01796 validation error -> corrupt push data ->
sky flashed random colors, crosshair had a pulsing translucent square.
FIX: both now push the full 128-byte pc[32] with VERTEX|FRAGMENT. Sky and
sprites set fog mode=-1 (off): sky is the background (never fogged); sprites are
self-lit effect billboards (minor: no underwater fog on sprites - the camera
world pos isn't readily available in vk_sprite.c; could add via a SetViewOrigin
like vk_model if wanted). All 6 push sites audited: 3d.layout(128) = sky/sprite/
model pc[32]; world.layout(112) = world/submodel [28]; warp_layout(144) = water
[36]. All consistent.

## v64 - dynamic lights on world surfaces (per-pixel)
Picked from the deferred list (highest visual value: every torch/projectile/
spell/explosion lights the world). ref_gl1 R_AddDynamicLights injects dlights
into per-surface lightmaps each frame; since VK bakes lightmaps into a static
atlas, doing that would mean per-frame lightmap re-uploads. Instead implemented
the SAME contribution model PER-PIXEL in the world/warp fragment shaders, which
fits the shader renderer and looks equivalent.
- New dlight UBO (set=1, binding 0), one per frame in flight, in
  vk_pipeline_world. Holds count, gl_modulate, and up to 32 lights
  (vec4 pos+intensity, vec4 color). World + warp pipeline layouts now have 2
  sets. VK_World_UpdateDlights fills it from fd->dlights each frame;
  VK_World_CurrentDlightSet returns it for the submodel pass.
- world.vert passes world position; world.frag adds, per light,
  (intensity - dist) * color * modulate/255 when (intensity - dist) > 64
  (DLIGHT_CUTOFF), matching GL1's falloff + cutoff. True 3D distance (GL1 uses a
  plane+texel approximation; visually equivalent).
- Gated by a push-constant flag (fog_extra.z): enabled for opaque world,
  DISABLED for submodels (their verts are model-space, so world-space light
  origins wouldn't match) and for warp/water (SURF_FULLBRIGHT - GL1 skips
  dlights on them too). Warp pass still binds the set to satisfy the layout.
KNOWN: dlights not yet applied to entities/models (separate pipeline; would need
the UBO bound there too) - models still get only their flat shadelight. Logged
for follow-up. Brightness scale vs GL may need eyeball tuning (gl_modulate).

## v65 - fix dynamic light brightness (v64 blew out to white)
v64 dlights were ~255x too bright (fireball blast = solid white disc). BUG: the
shader added frad * color * modulate / 255 with color in 0..255 -> result in the
~0..150 range, added to a 0..1-scale lightmap term (lm*2.0). Missing the color
byte->unit /255.
FIX (faithful to R_AddDynamicLights + R_BuildLightMap):
- contribution now frad * (color/255) * modulate / 255 -> proper 0..1 lightmap
  scale (GL1 accumulates in 0..255 blocklight space then /255 at byte convert;
  net two /255 factors, one for the color byte and one for the blocklight byte).
- Added GL1's desaturating peak clamp: if the brightest combined-light channel
  exceeds the cap (2.0, matching the lm*2.0 static headroom), rescale all three
  down proportionally instead of clipping per-channel -> preserves hue, no
  blowout to white. Mirrors R_BuildLightMap's "rescale if max > 255" step.
Example: intensity 200, white, 50 units away -> 150/255 = 0.59 added (was 150).

## v66 - bow flame reference fix (swapFrame path) + dynamic lights on entities
TWO fixes.

1. BOW FLAME OFF TO THE SIDE (both bows). ROOT CAUSE: VK_Skel_LerpReferences had
no swapFrame branch. When charging a bow the player is in move-and-cast
(swapFrame: legs run via 'frame', upper body charges via 'swapFrame'). The hand
references (CORVUS_LEFTHAND/RIGHTHAND, joint UPPERBACK) belong to the upper body,
so GL1 LerpReferences lerps them from the SWAPFRAME refsForFrame with the swap
move vectors and sets update_placement -> ApplySkeletonToRef uses swap_skeleton
+ repositions from swap root to cur root. VK always used the run-pose frame +
cur_skeleton -> hands tracked the legs -> flame off to the side. (Staff worked
because those attacks often aren't move-and-cast.)
FIX: ported the swap branch into VK_Skel_LerpReferences - build+rotate
swap_skeleton, compute swap move/front/back from the swapFrame translate/scale
(passed frames+framesize through now), and for refs with jointID >= swapCluster
lerp from swapFrame and apply swap skeleton + root offset. ApplySkeletonToRef
now takes the skeleton + update_placement like GL1.

2. DYNAMIC LIGHTS ON ENTITIES/MODELS (finishing v64/65). Entity pipeline now
includes set 1 = the shared dlight UBO (created by the world pipeline; reordered
init so World is created before 3D). entity.vert passes world pos (entity verts
are pre-transformed to world space), entity.frag adds the same per-pixel dlight
contribution as world.frag on top of the baked shadelight tint, with the
desaturating peak cap. Gated by fog_extra.z (enabled for models, 0 for sky/
sprites which are self-lit). Sky + sprites now also bind set 1 (layout requires
it) but keep dlights disabled. So torches/projectiles now light creatures, items
and the player, not just walls.
STILL NOT DONE: submodels (model-space verts) and water (fullbright) skip
dlights by design.

## v67 - fix model disappearance on map reload + coplanar z-fighting
TWO fixes.

1. NO MODELS ON SECOND LOAD (new-game-from-game, or after switching renderers a
lot). ROOT CAUSE: VK_EModel_Register appends a wrapper to s_emodels on EVERY
RegisterModel call with no dedup and no per-map reset. (Flex models ARE deduped
by name in s_models, but the engine-model WRAPPER list isn't.) Each map load
re-registers all models, growing s_emodels by the full count; after a couple of
loads it overflowed VK_MAX_ENGINE_MODELS (1024) and VK_EModel_Register returned
NULL -> client got NULL model handles -> nothing rendered. FreeAll was only
called at shutdown. FIX: added VK_EModel_BeginRegistration() (resets the wrapper
list) called from R_BeginRegistration before the new map loads. Wrappers are
cheap handles into the name-deduped flex/sprite caches + world submodel table,
so rebuilding them per map is correct (submodel indices must point into the new
world anyway). World side already freed correctly (VK_World_LoadMap ->
VK_World_Free). Not a v66 regression - long-standing, as the user suspected.

2. COPLANAR SURFACE Z-FIGHTING (two textures flickering through each other).
ROOT CAUSE: VK pipelines used VK_COMPARE_OP_LESS; ref_gl1 uses GL_LEQUAL for the
3D pass. With strict LESS, coplanar faces at identical depth flicker per-pixel
on FP rounding; LEQUAL lets equal-depth fragments pass so draw order resolves it
deterministically. FIX: world, entity (all variants + warp copy), and particle
pipelines now use VK_COMPARE_OP_LESS_OR_EQUAL.

## v68 - RF_REFLECTION (sphere-map environment reflection)
Implemented env-mapped reflection on RF_REFLECTION entities (weapon shrines per
g_Shrine.c, debris) + per-node FMNI_USE_REFLECT, matching ref_gl1
R_EnableReflection (GL_SPHERE_MAP texgen + r_reflecttexture).
Pieces:
- Entity vert format extended with a world-space normal: vkm_vert_t now
  {x,y,z, u,v, nx,ny,nz} (20 -> 32 bytes). Normal decoded from the bytedirs[162]
  anorms table (cur+old frame, lerped by backlerp, renormalized) then rotated to
  world space by the entity transform (rotation only, XformNormal). anorms.c
  added to the ref_vk build; anorms.h included in vk_model.c.
- Stride consistency: entity pipeline vertex input gained the normal attribute
  (location 2, offset 20). Sky (sky_vert_t) and sprites (vk_sprite_vert_t) share
  the entity pipeline so they now carry a dummy normal (0,0,1) to keep the
  32-byte stride aligned. (entity_vert_t in vk_pipeline3d.c, the stride struct,
  updated to match.)
- Shaders: entity.vert passes the normal through; new entity_reflect.vert/.frag
  do classic GL_SPHERE_MAP: r = reflect(normalize(worldpos-cam), n);
  m = 2*sqrt(rx^2+ry^2+(rz+1)^2); uv = r.xy/m + 0.5; sample the reflect texture.
  Regenerated vk_shaders.c (shaders.sh now builds the reflect pair).
- Pipeline: new vk_pipeline_3d.pipeline_reflect (opaque depth, reflection
  shaders, same layout). Created in VK_CreatePipeline3D, destroyed in teardown.
- Texture + routing: GetReflectDescriptor() lazy-loads misc/reflect.m32. The
  model node loop detects RF_REFLECTION (whole entity) or FMNI_USE_REFLECT (node)
  and, for reflective nodes, binds the reflect pipeline + reflect texture in
  place of the skin. Per-node pipeline switching (bound_pipe tracker) so a model
  can mix reflective and normal nodes; falls back to normal path if reflect tex
  missing.
NOTE: FMNI_USE_REFLECT is marked "checked but never set?" in q_Shared.h, so in
practice whole-entity RF_REFLECTION (shrines) is the live path; per-node is
handled anyway to match GL1.

## v69 - RF_TRANS_GHOST alpha (lighting-driven ghost opacity)
ref_gl1 sets a ghost's alpha from the world light at the entity
(alpha = shadelight[0] * 0.5), not the entity-color alpha - so a ghost is more
solid in lit areas, fainter in shadow. VK was routing RF_TRANS_GHOST through the
translucent pipeline (correct) but using e->color.a for alpha.
FIX:
- Added VK_LightPoint_SampleRGB(p, out[3]) (full-RGB world light sample; the
  existing VK_LightPoint_Sample only returned the max channel as a scalar).
- In VK_Model_DrawEntity, for RF_TRANS_GHOST: alpha = (shadelight[0] *
  e->color.r/255) * 0.5, clamped 0..1, written into the tint alpha (pc[19]).
  Falls back to e->color.a if lighting data isn't loaded.
NOTE: the mxd ref_gl1 uses standard GL_SRC_ALPHA/ONE_MINUS_SRC_ALPHA blending
for ghosts (the original H2 1.07 GL_SRC_COLOR/ONE_MINUS_SRC_COLOR "subtractive"
blend is left as a comment throughout the GL1 source - a deliberate port
divergence), so the existing translucent pipeline is the correct match; no new
blend mode needed. Skipped the conditional YQ2 minlight table (gl_state.
minlight_set, typically unset in base H2) and RF_MINLIGHT/RF_GLOW (separate
flags not combined with ghost in practice).

## v70 - animated lightstyles (flickering / pulsing / switchable lights)
ref_gl1 rebuilds each lit surface's lightmap every frame from up to 4 styles
scaled by the live lightstyles[].rgb intensities (R_BuildLightMap). VK bakes a
static atlas, so we now re-bake + re-upload only the surfaces whose styles
changed.
Implementation (vk_world.c + vk_buffer.c + vk_main.c):
- BakeSurfaceLightmap(): port of R_BuildLightMap - sums all of a surface's
  styles * (modulate * lightstyles[style].rgb) into float blocklights, then the
  desaturating peak clamp -> RGBA atlas slot.
- Load: retain the lighting lump (s_lightsamples), count each surface's styles
  (styles[]!=255), initial-bake from ALL styles at default intensity 1.0, and
  register surfaces using any animated style (style!=0) into s_anim_surfs with
  their atlas slot + sample pointer. CPU atlas (s_lm_cpu) + samples kept alive
  only when s_num_anim > 0 (freed otherwise / in VK_World_Free).
- Per frame (VK_World_UpdateLightstyles, called at R_RenderFrame start before
  the world draws): capture fd->lightstyles, diff vs previous frame, re-bake
  only changed surfaces, then upload all dirty slots in ONE batched transfer.
- vk_buffer.c: VK_UpdateTextureRGBA (single region) + VK_UpdateTextureRegions
  (batched: one staging buffer, one command submit, one fence for all dirty
  regions). vk_world.c calls the batched one (VK_TransitionImage is static to
  vk_buffer.c - going through the public fn avoids the link error that the first
  cut hit).
NOTE: per-frame uploads use a fenced one-shot before the frame's render pass
samples the atlas; batching keeps it to a single round-trip even when many
lights flicker at once.

## v71 - PVS culling (potentially-visible-set, opaque world pass)
First perf item from the deferred list. Was: VK drew every world surface every
frame regardless of visibility. Now mirrors ref_gl1 R_MarkLeaves + the recursive
world walk: only opaque surfaces in leaves whose cluster is in the camera leaf's
PVS are drawn.
Implementation (vk_world.c):
- VK_PVS_Load: loads the BSP tree (nodes/leaves/leaffaces/planes) + visibility
  lump from the IBSP buffer, builds parent pointers, copies + indexes the per-
  cluster PVS bit-offsets (bitofs[cluster][DVIS_PVS] via explicit flat indexing).
  Allocates s_pvs_faces (per-face vertex range + owning batch) and
  s_pvs_face_order (faces in emission order; each batch owns a [start,count)
  slice). On any failure -> PVS disabled, draw-all (correct, just slower).
- Emit loop records each face's vertex sub-range + appends to the batch's face
  slice.
- Per frame (VK_PVS_Mark, called from VK_World_Render): PVS_FindLeaf walks the
  tree to the camera leaf; if its cluster is valid and vis data exists,
  PVS_DecompressVis (RLE) expands the cluster row and marks faces in visible
  leaves with the current visframe. Cached: only recomputed when the view leaf
  changes. r_novis (or cluster -1 / no vis data) -> mark everything.
- Opaque draw: for each opaque batch, walk only its own faces (emission order)
  and draw merged runs of visible ones (consecutive visible faces share a
  contiguous vertex range -> one vkCmdDraw). O(total faces), not O(batches*faces).
SCOPE: opaque world only. Water/warp + submodels are NOT culled yet (few
surfaces, safe to draw all). Areaportals not considered (pure cluster PVS may
draw slightly more than GL1 across closed doors - never less, so always correct).
ESCAPE HATCH: 'r_novis 1' disables culling if anything looks wrong.
NEXT: frustum culling (R_SetFrustum) builds on the same node/leaf bboxes.

## v72 - underwater fog on submodels (was disabled in v62)
Some map objects (the floating window/grate, doors, lifts - all brush submodels)
kept their normal color underwater while the world around them fogged. Cause:
v62 disabled fog on submodels (mode=-1) because their verts are model-space and
a world-space fog distance would be wrong.
FIX: transform the world camera into each submodel's local space
(cam_local = R^T * (cam_world - origin); R orthonormal so inverse==transpose)
and pass that as the fog camera, so the shader's length(in_pos - cam) is the
true world distance. Fog now enabled on submodels via FillFogParamsAt at the
local camera. World view origin captured into s_view_origin_world at the top of
VK_World_Render (runs before the submodel pass). Dlights still off on submodels
(fog_extra.z=0) - that's a separate model-space issue, left as-is.
Entities (item/enemy/player models) already fogged correctly since v62 (their
verts are world-space); this was submodel-only.

## v73 - frustum culling (off-screen leaf rejection)
Second perf item. Complements PVS: skips leaves whose bounding box is entirely
outside the view cone (behind the camera or off to the sides). Mirrors ref_gl1
R_SetFrustum + R_CullBox.
Implementation (vk_world.c):
- VK_Frustum_Setup: 4 side planes (L/R/T/B) through the camera origin, built by
  rotating the view-forward vector around up/right by +-(90 - fov/2 deg)
  (RotateAroundVector = Rodrigues, = ref_gl1 RotatePointAroundVector). fov_x
  derived from fov_y + aspect: 2*atan(tan(fov_y/2)*aspect). Basis (fwd/right/up)
  from viewangles, matching make_view_matrix / ViewUpRight / AngleVectors.
- VK_Frustum_CullBox: box fully behind any plane (positive-corner test) -> cull.
  Honors r_nocull. The camera's own leaf contains the origin (on every plane) so
  it's never culled - inherently safe.
- VK_PVS_Mark now runs EVERY frame (frustum changes on rotation, not just leaf
  crossings) and marks a leaf's faces only if it passes BOTH the PVS bitset test
  AND the frustum box test. novis path still frustum-culls per leaf.
- Draw path unchanged (reads s_pvs_faces[].visframe); condition broadened to
  fire whenever PVS data is loaded (frustum benefits maps without vis too).
SCOPE: opaque world leaves. Water/submodels not frustum-culled (few surfaces).
ESCAPE HATCHES: r_nocull 1 (disable frustum), r_novis 1 (disable PVS).
Deferred list now: only alpha-surface back-to-front sorting remains (minor).

## v74 - alpha-surface back-to-front sorting (per-face) - DEFERRED LIST CLEARED
Last deferred item. Translucent world surfaces (water/warp/glass) were drawn in
batch (texture) order, so overlapping translucent planes could blend wrong.
Now sorted back-to-front per ref_gl1 R_SortAndDrawAlphaSurfaces.
Implementation (vk_world.c, VK_World_RenderWater):
- Added float center[3] to pvs_face_t; computed each face's world-space centroid
  during emission (averaging its emitted vert positions in 'out').
- Per frame: collect all translucent faces (batches with surf_flags != 0, walked
  via each batch's s_pvs_face_order slice), compute dist^2 from camera to each
  face centroid, insertion-sort descending (farthest first), then draw each face
  with its batch's descriptor + correct per-surf alpha/flowing/undulate/warp push.
  Rebind descriptor only when it changes (bound_desc tracker). Warp pipeline +
  dlight set bound once up front.
GRANULARITY: per-FACE (not per-texture-batch), so overlapping water/glass that
share a texture sort correctly too - full match to GL1's per-surface sort.
SCOPE: world translucent surfaces. NOT interleaved with alpha ENTITIES (GL1
merges both into one sorted list); alpha entities still draw in their own pass.
That edge case (a translucent model overlapping a water plane at a shared depth)
is rare in H2 and left as the one remaining finer-grained detail.
NOTE: translucent faces are intentionally NOT PVS/frustum-culled (drawn whether
or not visframe matches) - same as prior behavior; water surfaces are few.

=== DEFERRED LIST NOW FULLY CLEARED ===
ref_vk is at full visual + perf parity with ref_gl1 for everything audited:
fog, dynamic lights, reflections, ghost alpha, animated lightstyles, PVS +
frustum culling, submodel fog, and alpha sorting. Only remaining known divergence
is the (rare) alpha-entity/alpha-surface interleave noted above.

## v75 - cloaking-enemy fade (RF_TRANSLUCENT via color.a) + alpha-default fix
The fade-in/out cloaking assassins were fully opaque under VK while GL faded
them. ROOT CAUSE: VK selected the alpha-blend pipeline only when RF_TRANS_ANY
was set, but these enemies fade purely via e->color.a with NO RF_TRANS flag.
ref_gl1 (gl1_FlexModel.c:543) enables blending when
  e->color.a != 255 || (flags & RF_TRANS_ANY) || skin->has_alpha
FIX (vk_model.c VK_Model_DrawEntity):
- want_trans = (e->color.a != 255) || (e->flags & RF_TRANS_ANY); use the
  translucent pipeline when want_trans (additive still keyed off RF_TRANS_ADD*).
  (skin->has_alpha not tracked yet - color.a + RF_TRANS_ANY covers the fades.)
- Fixed the all-zero-color "default to white opaque" guard: it now only forces
  alpha=1 when RGB is zero AND alpha is zero AND !want_trans (truly
  uninitialized). Previously a fully-faded entity (alpha 0, rgb 0) snapped back
  to opaque white.
- v69 RF_TRANS_GHOST shadelight alpha is independent (these cloakers are not
  RF_TRANS_GHOST) - both paths coexist.
DIAGNOSTIC left in this build: logs up to 40 non-shadow translucent models with
flags/color/pipe so the enemy's real params can be confirmed. Remove once OK.

## v76 - translucent/warp SUBMODELS (forcefields, energy barriers, water brushes)
A forcefield blocking a doorway (a brush submodel with a TRANS33/66 texture)
rendered as a flat OPAQUE texture under VK; GL shows it semi-transparent +
animated. ROOT CAUSE: VK_World_RenderSubmodel bound the OPAQUE world pipeline
for every submodel batch and never captured/handled surf_flags, so translucent
brush entities drew solid with the plain world shader.
FIX (vk_world.c):
- Submodel batch build now accumulates surf_flags + alpha (same SURF_WARP/
  TRANS33/TRANS66/FLOWING/UNDULATE mask as world batches) into world_batch_t.
- VK_World_RenderSubmodel now draws in two phases: opaque batches on the world
  pipeline (as before, with local-space fog), then translucent/warp batches on
  the warp_pipeline (alpha blend, depth-write off) with the full 144-byte warp
  push (time/alpha/flowing/undulate/warp + local-space fog tail). Matches how
  the main world splits opaque vs VK_World_RenderWater.
- Added s_frame_time (captured from fd->time in VK_World_Render) so submodel
  warp/flow animates on the same clock as the world warp pass.
NOTE: assumes the forcefield is a brush SUBMODEL (typical for func_ barriers).
If a specific one turns out to be a world-BSP trans surface instead, that path
already blends (VK_World_RenderWater) so it would be a separate issue.

## v77 - forcefield animation (SURF_ANIMSPEED, time-driven texture cycling) [DIAG]
v76 made the forcefield translucent but it looked washed-out/flat vs GL's bright
animated green circuit pattern. DIAGNOSIS: GL's R_TextureAnimation has a
SURF_ANIMSPEED (0x1000) path: when set, the texture frame is driven by TIME
(frame = texinfo.value(fps) * time, wrapped by chain length), not the entity
frame. The forcefield is a submodel whose swirl IS this time-based frame cycle;
VK was stuck on the base frame.
CHANGES (vk_world.c):
- world_batch_t.anim_speed: captured from texinfo.value when SURF_ANIMSPEED set
  (submodel batch build, alongside the existing nexttexinfo frame chain).
- Submodel render frame selection: if anim_speed > 0, frame = (time*anim_speed)
  % num_frames (time-driven); else the existing entity-frame path (doors etc).
DIAGNOSTIC in this build: logs every translucent submodel batch with its
tex/surf flags/alpha/frame-count/animspeed, so the forcefield's real params can
be confirmed (esp. whether frames>1 - if frames==1 the anim chain wasn't
resolved and that's the actual gap).
TODO after confirming: if washout persists, check alpha (TRANS33 vs 66) and
whether the frame textures resolve; remove diagnostic.

## v78 - forcefield = SURF_WARP submodel (turbulence warp, not frame animation)
User spotted it: the forcefield warps like water (single texture, UV-distorted),
not a frame animation. So v77's SURF_ANIMSPEED theory was wrong. ROOT CAUSE:
SURF_WARP submodels were emitted with EmitFace (normalized UVs, NO subdivision),
but the warp shader's turbulence expects RAW texel s,t (divides by 64) on a
SUBDIVIDED grid (per-vertex ripple). Feeding normalized UVs + a flat quad to the
turbulence path produced the washed-out green smear.
FIX (vk_world.c):
- Submodel emit now uses EmitFaceEx(..., is_warp = (ti->flags & SURF_WARP)) -
  same path as world warp surfaces: subdivides into a <=SUBDIVIDE_SIZE grid and
  writes raw texel UVs so the turbulence ripples correctly.
- Submodel vertex-buffer size estimate updated to account for warp subdivision
  (was (numedges-2)*3 flat; now the grid-cell estimate for SURF_WARP faces) to
  avoid overflowing sub_out.
- v76 already routes sub_surf!=0 batches to the warp pipeline + binds isWarp from
  SURF_WARP, so with correct UVs the turbulence now applies.
Diagnostic (v77) still in: prints each trans submodel batch's surf flags/alpha/
frames/animspeed - use it to confirm the forcefield is SURF_WARP (0x8) and what
alpha/trans flags it carries. Remove once confirmed.

## v79 - warp turbulence speed + warp backface cull (forcefield/water)
Two issues user spotted on the now-warping forcefield (and would affect water):
1. SWIRLING TOO FAST/DENSE: ref_gl1 indexes turbsin[(int)(arg*256/360)&255]
   where turbsin = 8*sin over a full period -> effectively 8*sin(arg*2PI/360),
   i.e. the turb argument is in DEGREES. Our shader did 8*sin(arg) treating it
   as RADIANS -> ~57x too fast/dense. FIX: turb(x) = 8*sin(x * 2PI/360) in
   world_warp.vert. Affects water too (now matches GL rate). Re-ran shaders.sh.
2. RENDERED TWICE (front+back both blending): warp pipeline used CULL_MODE_NONE
   (fine for opaque since it writes depth; bad for translucent - both sides
   blend). FIX: warp pipeline now has its OWN rasterization state with
   cullMode=BACK, frontFace=CCW (opaque world pipeline stays NONE). ref_gl1
   culls GL_FRONT; our winding should map to BACK here.
   RISK: if winding is opposite, the forcefield/water would VANISH - then flip
   to CULL_MODE_FRONT or frontFace=CW. (Watch on test.)
Diagnostic (v77) still in (submodel trans batch flags). Remove once all good.

## v80 - warp cull flip + frozen-animation diagnostic
v79 results: (1) culled the WRONG face (could see through the forcefield front
to the room behind) - FLIPPED to frontFace=CLOCKWISE (keep near side). (2) warp
animation FROZE entirely (forcefield + water) after the degree conversion.
ANALYSIS: the 2PI/360 multiplier is mathematically exact vs GL's turbsin index
(verified: effective freq ratio = 1.0). The freeze implies the TIME phase now
advances too slowly to see - which only makes sense if fd->time is very small /
barely advancing in our build (the old x57 version amplified it enough to look
animated). Added a throttled 'vk: fd->time = ...' log (first 8 frames) to
confirm fd->time's magnitude and whether it climbs.
NEXT (depends on diag): if fd->time is tiny/static, drive warp from a real
clock (e.g. ri.Sys_Milliseconds()*0.001) instead of fd->time, applied equally
to world + submodel warp. If fd->time climbs normally, reconsider the spatial
vs time split.

## v81 - warp animation: separate spatial density from scroll speed
Diagnostic confirmed fd->time advances normally (~0.016/frame, in seconds). The
freeze was because the exact GL math (spatial AND time * 256/360) makes the time
term scroll a full cycle only every ~360s - GL is effectively near-static from
time too; its lively look doesn't come from that term at our time scale.
FIX (world_warp.vert): keep the SPATIAL term at the correct "degrees" density
(coord*0.125 * 2PI/360) so the ripple pattern matches GL's density (fixes the
old 57x-too-dense look), but advance the TIME/scroll phase on its own visible
rate (TURB_TIME_RATE = 2.0) instead of being crushed by the spatial scale.
Undulate vertex bob likewise uses degree-space spatial + the visible time rate.
TURB_TIME_RATE is the one tunable if it's still too fast/slow.
Also: removed the fd->time diagnostic. Warp backface cull flipped to CW in v80.
NOTE: this is a deliberate, documented divergence from GL's literal turbsin time
handling, chosen to reproduce the visible result at our time units.

## v82 - warp turbulence: raw spatial churn + tuned time speed (the real fix)
User: v81 moved the whole texture as ONE RIGID SHEET (slid/rotated), no internal
distortion; GL churns turbulently. ROOT CAUSE: the degree conversion (2PI/360)
applied to the SPATIAL term collapsed the per-vertex phase variation across the
surface to <0.2 cycles, so every vertex warped near-identically -> rigid slide.
The churn requires the RAW (radians-direct) spatial term coord*0.125 so adjacent
subdivided-grid verts sample very different turb phases (plus the cross-coupling:
s offset by turb(ot), t by turb(os)).
FIX (world_warp.vert): turb(coord,time) = 8*sin(coord*0.125 + time*TURB_TIME_RATE)
- spatial term RAW (full churn density, matches the earlier version that looked
correct but ran too fast), TIME slowed to TURB_TIME_RATE=1.5 rad/s for a pleasant
pace. Undulate vertex bob likewise raw-spatial + tuned time.
So: the degree conversion (v79-v81) was the wrong turn; v78's radians churn was
right, it was only too fast. This keeps the churn and fixes the speed.
TURB_TIME_RATE (1.5) is the single tunable for flow speed.

## v83 - warp speed halved
Churn character confirmed correct in v82, just too fast. Halved TURB_TIME_RATE
1.5 -> 0.75 in world_warp.vert. Undulate vertex bob scales off the same constant
so it slows proportionally too. Single-line tune; rebuilt shaders.

## v84 - global level fog (r_fog) [Darkmire swamp] [DIAG]
Darkmire renders washed-out/bright under VK vs GL's dark murky atmospheric fog.
This is a LEVEL-WIDE fog (r_fog cvar system), separate from the underwater fog
(v62, gated on cl_camera_under_surface). ref_gl1 gl1_Main.c R_Fog(): when r_fog
is set, applies r_fog_mode/density/startdist/color_r/g/b + r_farclipdist (and
glClearColor to fog color). Priority: underwater (R_WaterFog) > r_fog (R_Fog) >
off. Cvars set per-level (e.g. trigger_fogdensity, g_Spawn.c:73).
CHANGES:
- vk_world.c FillFogParamsAt + vk_model.c Model_FillFogParams: added the global
  r_fog branch (underwater > globalfog > off), feeding the existing shared fog
  push layout that every 3D shader's fog_factor already reads. Defaults match GL
  (r_fog 0, mode 1/EXP, density 0.004, start 50, color 1/1/1, farclip 4096).
DIAGNOSTIC (this build): logs the r_fog cvar state (value/mode/density/start/
color/farclip/underwater) the first 3 frames - to CONFIRM the game DLL actually
sets r_fog when entering Darkmire. If r_fog stays 0, the engine isn't pushing it
to the renderer and we need to handle the trigger_fogdensity path / level cfg.
TODO if r_fog is set but still wrong: consider the glClearColor-to-fog-color
equivalent (screen clear) for sky/void areas; verify submodel path fogs too.

## v85 - global fog: removed diagnostic; investigation notes (whiteout)
Diagnostic (v84) confirmed the level DOES set fog: r_fog=1 mode=1(EXP)
density=0.0015 start=50 color=(1,1,1) farclip=4096. So the data path works and
the implementation is faithful to ref_gl1 R_Fog (exp(-density*dist), mix toward
white fog color). Removed the diagnostic.
OPEN ISSUE: VK blows out to near-WHITE while GL (same cvars) is dark/murky. Math
says density 0.0015 EXP should only ~50-70% fog mid-room walls, not pure white.
Two suspects:
 1) VK world is brighter than GL at baseline: world.frag uses light = lm*2.0
    (hardcoded 2x overbright) while ref_gl1 scales lightmaps by gl_modulate
    (default 1.0). White fog over VK's brighter walls blows out; over GL's darker
    walls reads as haze. BUT the lm*2.0 has been correct since v52 on other
    levels, so it's likely load-bearing (lightmaps may be built half-scale) -
    do NOT change blindly.
 2) Possible that GL's Darkmire fog cvars differ at the moment of the dark
    screenshot vs what we read.
NEXT: need GL-with-this-fog reference at the same spot to disambiguate. If GL is
hazy-grey there, the VK baseline brightness is the real bug (investigate lm*2.0
vs lightmap build scale). If GL is also bright, the cvar values differ.

## v86 - sky fog (sky was drawn crisp ON TOP of fog)
User screenshots (thank you) showed in a fogged swamp: VK sky renders crisp and
in FRONT of the fog, while GL fogs the sky into the horizon haze. ROOT CAUSE:
VK_Sky_Render hard-coded fog mode = -1 (sky never fogged). ref_gl1 keeps fog
enabled while drawing the sky.
FIX:
- vk_world.c VK_World_FillSkyFog(pc): fills the entity fog tail with the real
  fog params and sets fog_extra.w = 1 (sky-fog flag) when fog is active.
- vk_sky.c: calls VK_World_FillSkyFog instead of forcing mode = -1.
- entity.frag: when fog_extra.w > 0.5, force full fog colour (f = 0) since the
  sky geometry is ~1 unit away and wouldn't otherwise fog.
STILL OPEN (world too white): world terrain goes pure white where GL shows
partial grey-green fog (you can see terrain through GL's fog at mid range). Fog
colour read as (1,1,1). Suspect VK ramps to full fog too fast and/or the world
lm*2.0 overbright compounds with white fog. Deferred pending how it looks with
the sky fogged - the crisp white sky behind everything was likely exaggerating
the whiteout. Player fog is CORRECT (close = light fog), not a bug.

## v87 - DIAG: fog-factor visualization + transparency-in-fog investigation
v86 sky fog confirmed; user reports world STILL whites out even at point-blank
range (close wall = pure white, image shows almost no depth) - impossible from
distance fog at density 0.0015 unless fog factor is wrongly ~0 up close. Also:
swamp-water dead-plant TRANSPARENCY is lost under VK in the fogged area (user
hypothesizes the fog pipeline broke transparency).
DIAG build: cvar vk_fogdebug (default 0). Set `vk_fogdebug 1` in console to
render the world's fog as: GREEN = fog factor (bright=near/no fog, black=full
fog), RED = normalized distance/2000. Tells us if the close wall wrongly has a
near-zero fog factor (-> distance/density bug) or high factor (-> blend/
brightness bug elsewhere).
Notes: world.frag math is mix(fogcolor, col, f), out alpha 1 - looks correct.
world opaque pass passes fd->vieworg as fog cam (correct). warp frag fogs rgb
but keeps translucent alpha. Need the viz to localize.
TODO: get vk_fogdebug screenshot of the close wall; then fix the localized cause
and remove diag.

## v88 - THE FOG FIX: restored dropped fog camera position (world/water)
vk_fogdebug viz (v87) was decisive: world fog factor was BLACK everywhere (even
point-blank walls) = full fog = huge distance. ROOT CAUSE: the v84 global-fog
refactor of FillFogParamsAt (vk_world.c) accidentally DROPPED the line that
writes the fog camera position pc[off+0..2] = cam. So fog_cam.xyz stayed
zero/stale and the shader measured fog distance as length(in_pos - origin) - i.e.
distance from the WORLD ORIGIN, not the camera. In maps far from origin (Darkmire)
that's thousands of units -> exp(-density*dist) ~ 0 -> everything blown to the
white fog colour. Models looked correct because vk_model.c's Model_FillFogParams
kept its own cam write (pc[20..22]); only the world/water path lost it. This also
explains the white swamp water (warp pass uses the same fill) and is consistent
with underwater fog only looking right in small maps near origin.
FIX: restored pc[off+0..2] = cam in FillFogParamsAt. One line; fixes world +
water + (correctly) underwater fog distance everywhere.
Removed the vk_fogdebug viz (world.frag) and the cvar plumbing.
NOTE (separate, observed in console): players/male logs flags=0x20
color=(255,255,255,0) alpha=0 pipe=trans on spawn - the v75 color.a!=255 ->
translucent rule makes a momentarily-alpha-0 player fully invisible. Harmless at
spawn but worth watching; not touched here.

## v89 - scorch/decal fog (alpha sprites now fogged); water-over-projectiles +
## plant transparency still open
With fog distance fixed (v88), three swamp issues remained:
1. [FIXED] Scorch marks / decals rendered as stark FULL BLACK on top of the fog.
   Cause: sprites forced fog mode = -1 (never fogged), so alpha decals stayed
   full-strength while the fogged world faded around them. FIX: alpha-blended
   sprites now use the real fog (VK_World_FillEntityFog -> camera-relative
   distance), so decals fade into the haze like ref_gl1. ADDITIVE sprites stay
   unfogged (fogging toward the white fog colour would brighten, not fade them).
   Added VK_World_FillEntityFog; vk_sprite.c includes vk_world.h.
2. [OPEN] Water drawn ON TOP of projectiles even when cast above the water.
   Projectile fx are sprites drawn in the entity/alpha list BEFORE
   VK_World_RenderWater; water has depth-write OFF so it paints over the
   (also non-depth-writing) additive projectile sprites. This is the translucent
   draw-order problem. Likely fix: draw projectile sprites after water, or a
   proper back-to-front merge of water + effect billboards. Needs care - not all
   sprites should move. Deferred for a focused pass.
3. [OPEN] Small swamp plant (models/objects/plants/plant*) still not transparent.
   Foliage cutout. moss* logs as pipe=trans so blending is on; suspect the plant
   skin's alpha isn't reaching the blend (m8 loader forces a=255; m32 keeps
   alpha). Need to confirm the plant skin format + whether it needs alpha-TEST
   (discard) vs blend. Note ref_gl1 R_LoadM8 sets has_alpha=false too, so the
   plant is likely m32 or uses a specific transparent palette index - to verify.

## v90 - DIAG: sprite flags/blend logging (black plant sprites + white pickups)
New info from user: the "plant" with no transparency is actually MULTIPLE SPRITES
in a star arrangement (not a model) - rendering as solid BLACK quads. Health
pickups render solid WHITE (should be gold-topped and PULSE white<->brighter;
the pulse is fog-related in GL). Also more black sprites found elsewhere, and
swing-vines render behind water/trees (same translucent order issue as
projectiles).
Investigation so far: GL draws ALL sprites blended (R_HandleTransparency,
gl1_Misc.c): RF_TRANS_ADD -> GL_ONE,GL_ONE (additive, black=invisible);
else -> GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA + GL_ALPHA_TEST(GREATER,0.05).
GL .m8/paletted upload forces alpha=255 (no transparent index), so transparent
sprites must be .m32 (real alpha). Our .m32 loader keeps alpha; .m8 forces 255.
So black/white opaque sprites are most likely a PIPELINE/blend mismatch, not the
texture. DIAG build logs each sprite: name/frame/flags/color/blend(add|trans)/
nodepth - to see what flags the black plant + white pickup sprites actually
carry and which pipeline they pick.
OPEN (unchanged): water-over-projectiles/vines draw order (v89 note #2);
plant/pickup sprite blend (this diag); confirm after.

## v91 - sprite/decal cutout transparency + RF_GLOW item pulse
Console diag (v90) identified the culprits:
- Vines 'segment_vine.sp2' flags=0x40000 (RF_LM_COLOR) blend=trans -> rendered
  as opaque BLACK quads. The black star "plant" is the same: alpha-cutout
  sprites whose transparent texels drew opaque.
- Health pickup is a MODEL 'models/items/health/healthsmall' flags=0x4120
  (RF_TRANS_ADD|RF_GLOW|RF_TRANSLUCENT) pipe=add -> solid white, no pulse.
FIXES:
1. .m8 loader (vk_image.c): palette index 255 is the Quake/H2 transparent index;
   now sets alpha=0 for it (was forcing 255), so paletted cutout sprites/decals
   get real transparency. (.m32 already keeps its alpha.)
2. entity.frag: added alpha test `if (c.a < 0.05) discard;` matching ref_gl1
   R_AlphaFunc(GL_GREATER,0.05) for sprites/alpha-textured surfaces - cuts out
   transparent texels instead of drawing opaque quads. Harmless for opaque art.
3. RF_GLOW (vk_model.c): bonus items pulse. ref_gl1 sets shadelight =
   sin(time*7)*0.3+0.7; applied as a tint multiplier so health/mana brighten and
   dim instead of static full white.
Removed the sprite diagnostic.
STILL OPEN: water/vine/projectile draw-order (translucent sort) - sprites drawn
before VK_World_RenderWater get painted over by the depth-write-off water. Next.

## v92 - sky alpha-test hole fix + render-order (sprites write depth vs water)
1. [FIXED] Big BLACK SQUARE in the sky (and a matching one far away). Cause: the
   v91 alpha-test discard in entity.frag (shared by the sky) punched holes where
   sky texels have alpha < 0.05, revealing the dark framebuffer clear colour
   (0.05,0.05,0.12). FIX: entity.frag now computes is_sky (fog_extra.w > 0.5) and
   NEVER alpha-tests the sky. VK_World_FillSkyFog always sets the sky marker
   (pc[31]=1) regardless of fog; the shader applies full fog colour to the sky
   only when fog mode (fog_color.w) >= 0, else fog_factor=1 leaves the sky as-is.
2. [FIXED] Render order: water (depth-write OFF) painted over swing-vines /
   projectiles / effect billboards in front of it. ROOT CAUSE: ref_gl1 draws the
   whole entity pass with glDepthMask(GL_TRUE) and only disables depth writes for
   the following alpha-surface (water) + particle passes; our translucent sprite
   pipeline had depth-write OFF, so sprites didn't occlude the later water. FIX:
   added pipeline_translucent_zwrite (alpha blend + depth test&WRITE) and use it
   for entity-pass alpha sprites (non-RF_NODEPTHTEST). The v91 alpha-test discard
   means cutout sprites only write depth on solid texels, matching GL. Additive
   sprites kept depth-write OFF for now (GL writes depth for them too, but that
   risks glow quads occluding particles - revisit if additive projectiles still
   render behind water). Models unchanged (cloaking translucent bodies must not
   write depth).

## v93 - line-sprite (vine) orientation + splash-over-water depth regression
1. [FIXED] SPRITE_LINE billboards (swing-vines, ropes, chains, tendrils,
   segment_* sprites) collapsed to zero width and vanished at flat viewing
   angles. ROOT CAUSE: the beam width axis was diff x (camera up); when the beam
   aligned with the camera-up axis the cross product went to zero. ref_gl1
   R_DrawLineSprite uses vpn (view FORWARD), not up - width = diff x viewfwd, so
   the beam always faces the camera and only thins at extreme angles. FIX: thread
   the view forward vector (vfwd) from ViewUpRight -> DrawEntityList ->
   VK_Sprite_DrawEntity and use it for the SPRITE_LINE width axis.
2. [FIXED] Regression from v92: stepping on swamp water punched a transparent
   HOLE where the splash/ripple sprite was. CAUSE: v92 made ALL non-additive
   sprites write depth; soft water effects (waterentryripple etc.) then occluded
   the depth-write-off water behind them. FIX: restrict depth-writing to
   SPRITE_LINE segments only (the structural vines/ropes that genuinely need to
   occlude water). Soft point effects (SPRITE_STANDARD/DYNAMIC: splashes,
   ripples, steam) go back to depth-write OFF so they blend over the water.
Note: additive sprites remain depth-write OFF (unchanged).

## v94 - interleave alpha entities + water by depth (DONE)
Symptom: effects/sprites that sit on the OPAQUE swamp water (crosshair FX_CROSSHAIR,
splashes) render UNDER it in VK. Present in old builds too (not a regression).
Crosshair is RF_NODEPTHTEST and works in GL but goes under swamp water in VK.

Root cause (CONFIRMED against ref_gl1):
- ref_gl1 R_RenderView order: World -> R_DrawEntitiesOnList(OPAQUE entities only)
  -> glDepthMask(FALSE) -> R_SortAndDrawAlphaSurfaces -> particles -> DepthMask(TRUE).
- R_SortAndDrawAlphaSurfaces (gl1_Surface.c) builds ONE list of {alpha_entities}
  + {alpha world surfaces = water} and draws them INTERLEAVED back-to-front by
  depth. So a crosshair/splash in front of the water draws AFTER the water -> on top.
- The client (cl_view.c) pre-sorts alpha_entities back-to-front (alphaentitycmpfnc)
  using entity.depth = |origin - vieworg| (set in ce_Entities.c:233).
- Water surfaces get a screen-space depth (max screen_pos[2] over the face verts).
- VK BUG: we draw ALL alpha_entities (DrawEntityList) BEFORE ALL water
  (VK_World_RenderWater). No interleave -> anything in front of opaque water is
  painted over by the water.

Water flag taxonomy (CONFIRMED, ref_gl1 R_DrawAlphaSurface gl1_Surface.c:92):
- SURF_TRANS33 -> alpha 0.33 (clear water, see-through)
- SURF_TRANS66 -> alpha 0.66
- neither (SURF_WARP/DRAWTURB only) -> alpha 1.0 OPAQUE = swamp water.
VK already reads these correctly (batch_alpha 0.33/0.66/1.0). Swamp water IS
correctly opaque in VK. So the water rendering is fine; only the ORDER is wrong.

Fix plan: merge alpha entities + translucent water faces into one back-to-front
list and draw interleaved (matching R_SortAndDrawAlphaSurfaces). A blanket
"draw alpha entities after water" is INCORRECT for clear (TRANS33/66) water:
a submerged entity behind clear water would wrongly draw on top (water writes no
depth). Must interleave by depth. Both already have per-item depth available
(entity.depth Euclidean; water face center distance). Unify on distance-to-camera.

IMPLEMENTED in v94:
- vk_world.h: VK_World_RenderWater now takes (fd, alpha_ents, num, draw_cb, user)
  and a VK_DrawAlphaEntityFn typedef.
- vk_world.c: water draw loop interleaves the (client-presorted back-to-front)
  alpha entities with the back-to-front water faces. Before each face it flushes
  all entities FARTHER than that face (ent->depth^2 > face squared dist); after
  the last face it draws the remaining nearer entities on top. Re-binds the warp
  pipeline + dlight set after any entity draw (entities rebind their own
  pipeline). Early-out paths (no world / no translucent batches) still draw all
  deferred entities.
- vk_main.c: split DrawEntityList into DrawOneEntity + DrawEntityList; added
  AlphaEntDrawCtx + DrawAlphaEntityCB; opaque entities still drawn before water;
  alpha entities now passed into VK_World_RenderWater for interleaving (no longer
  a separate pre-water DrawEntityList pass).
Projection: water uses near=1/far=4096; BuildViewMVP uses near=1/far=r_farclipdist
(default 4096) - matches at default so interleaved depth values are consistent.
(Minor: if r_farclipdist is changed from 4096 the two would differ - left as-is
since it has matched across all prior versions; revisit only if farclip is tuned.)
Test: crosshair + splashes should now sit ON TOP of opaque swamp water; submerged
objects behind clear (TRANS33/66) water should still render behind it; vines
(v92/v93) and projectiles vs water should remain correct.

## v95 - fix v94 regression: no water drawing at all
v94 interleave broke ALL water rendering (swamp + clear vanished). ROOT CAUSE:
when an alpha entity is drawn mid-water-loop (DrawOneEntity -> model/sprite/
submodel), it binds its OWN vertex buffer (s_model_vbo / s_sprite_vbo /
s_sub_vbo). The interleave re-bound the warp pipeline + dlight set afterwards but
NOT the water vertex buffer (s_vbo), so every water face drawn after the first
interleaved entity read vertices from the wrong VBO -> nothing valid drawn. With
many alpha entities farther than the nearest water face, entities flush before
the first face -> VBO corrupted before any water draws -> all water gone.
FIX: the warp-rebind block in FLUSH_ENTS_FARTHER_THAN now also re-binds
s_vbo (vkCmdBindVertexBuffers) after entity draws. Pipeline + VBO + dlight set +
per-face descriptor (bound_desc reset to NULL) all restored.

## v96 - additive sprites now fade by alpha (lens-flare occlusion fade)
User: blue lens-flare lights behind geometry FADE smoothly in GL but BLINK out in
VK. The fade is shared client code (fx_LensFlare.c LensFlareUpdateOrigin: when the
eye->light trace is sight_blocked, self->alpha *= 0.95 each think; ce_Entities.c:274
writes that into r.color.a). So alpha reaches the renderer correctly. ROOT CAUSE:
lens flares are ADDITIVE sprites (ONE,ONE blend) which IGNORE the alpha channel -
our entity.frag outputs c.rgb unscaled by alpha, so the glow stayed full-bright
while color.a fell, then vanished only when the client set CEF_NO_DRAW (alpha<0.01)
-> a blink, not a fade. ref_gl1 scales the additive contribution by alpha.
FIX (vk_sprite.c): for additive sprites, fold alpha into the RGB tint
(pc[16..18] *= pc[19]) and set tint alpha = 1. Now the additive glow dims as
color.a fades (lens flares fade smoothly like GL). Non-fading additive sprites
have color.a=255 so tint is unchanged (no regression). Setting tint.a=1 also keeps
the v91 alpha-test discard from nuking the faded flare instead of dimming it.

## v97 - FIX Andoria transition crash (game-logic assert, NOT renderer)
Core dump (SIGABRT) on the Andoria map load showed it was NOT a Vulkan crash:
  Assertion `self->viewheight > 0 && self->viewheight < (int)(self->maxs[2]*
  self->s.scale)' failed, g_Monster.c:752, M_WalkmonsterStartGo
  (SV_SpawnServer -> G_RunFrame -> EntityThink -> M_WalkmonsterStartGo).
ROOT CAUSE: the reverse-engineered port REPLACED two original viewheight
ASSIGNMENTS with asserts (porter comments say so):
  - walk monster (line 752): original "invariantly sets viewheight to 25"
  - fly monster (line 776): original "sets viewheight to 25 if it's 0"
A walking monster on Andoria spawns with a viewheight outside the asserted
range -> assert aborts the whole game. (Renderer irrelevant; happens under gl1
too.)
FIXES (src/game/src/Monsters/g_Monster.c):
  - walk: self->viewheight = 25;  (restore original unconditional assignment)
  - fly:  if (viewheight == 0) viewheight = 25;  (restore original)
SYSTEMIC FIX (CMakeLists.txt): the build never defined NDEBUG, so EVERY assert()
in the whole codebase was live (the retail game shipped with asserts compiled
out). Other latent asserts (spells, AI, navsys, skeletons) could abort gameplay
on other maps the same way. Added: option(H2R_ENABLE_ASSERTS OFF) and
add_compile_definitions(NDEBUG) unless that option is ON -> faithful retail-style
build, asserts compiled out. Verified the viewheight assert string is gone from
libgame.so.
PACKAGING: this release ships ALL libraries (libgame.so fix + NDEBUG affects
every module), not just libref_vk.so.

## v98 - DIAG: identify flowing-waterfall surface flags
User: a downward-flowing waterfall animates in GL but is STATIC in VK. Also asks
whether environmental (world) light level is applied to the player model (player
looks brighter/flatter in VK than the shadowed GL Corvus).
Findings so far:
- ref_gl1 opaque lightmapped path literally has "// Missing: SURF_FLOWING logic"
  (gl1_Surface.c) - opaque flowing surfaces do NOT scroll in GL either.
- ref_gl1 R_EmitWaterPolys (gl1_Warp.c) applies BOTH turb + flow scroll, but only
  to SURF_WARP/DRAWTURB (alpha/water) surfaces.
- So the flowing waterfall must be SURF_WARP+SURF_FLOWING (goes through the warp
  path). Our warp shader (world_warp.vert) DOES implement the flow scroll, but
  ONLY inside the isWarp branch (params2.x>0.5). If the waterfall surface lacks
  SURF_WARP (FLOWING only), it hits the else branch -> no scroll -> static. Need
  to confirm the actual flags.
DIAG build: logs "vk: WATERBATCH tex=... surf=0x.. warp/flow/trans/undul/alpha"
for every batch with FLOWING or WARP, so we can see exactly what the waterfall is
and fix the shader gating accordingly.
TODO after diag: also investigate player model world-light sampling (looks like
the model isn't picking up the local lightmap/ambient level - separate from this).

## v98 - animated world textures (flowing waterfall) - frame cycling
User: a waterfall that ANIMATES (flowing downward) in GL is STATIC in VK. User
correctly noted it looks like an ANIMATED TEXTURE, not UV-scroll flow.
ROOT CAUSE: ref_gl1 R_TextureAnimation walks the texinfo 'next' chain to cycle
world-texture frames; SURF_ANIMSPEED textures cycle by time
(frame = num_frames * time). We BUILT this anim chain (num_frames/anim_speed/
frame_desc) and consumed it ONLY for SUBMODELS - the MAIN-WORLD opaque batches
left num_frames=1 and the opaque draw bound the base descriptor, so animated
world textures (the waterfall) were stuck on frame 0 = static.
FIX (vk_world.c):
- Main-world batch builder now resolves the texture animation chain (same logic
  as submodels): finds a representative texinfo for the batch, reads
  SURF_ANIMSPEED 'value' as anim_speed, walks nexttexinfo collecting frame
  descriptors into frame_desc[]/num_frames.
- New WorldBatchFrameDesc(wb): picks the frame descriptor for this frame
  (anim_speed>0 -> time-driven fr = time*anim_speed; else frame 0), matching
  R_TextureAnimation.
- Opaque world draw (both PVS path + draw-all fallback) and the water/warp pass
  now bind WorldBatchFrameDesc(&batch) instead of the static base descriptor.
Removed the earlier wrong-theory SURF_FLOWING WATERBATCH diagnostic.
Note: SURF_FLOWING UV-scroll is a separate mechanism we already had in the warp
shader; this waterfall was frame animation, not flow scroll.

## v99 - model world-light sampling (player/monsters dark in shadow)
User: the player looks too bright/flat in VK vs GL - in a shadowed alcove GL's
Corvus is dark, VK's stays bright. ROOT CAUSE: VK_Model_DrawEntity used the raw
entity color (usually white) as the tint and NEVER sampled the world light at the
model origin. ref_gl1 R_DrawFlexModel calls R_LightPoint(e->origin) for normal
models so they darken in shadow / brighten in light, then multiplies by entity
color, applies RF_MINLIGHT floor, RF_GLOW pulse.
FIX (vk_model.c): replaced the tint setup with GL's shadelight order:
  RF_TRANS_ADD_ALPHA -> grey = alpha; RF_FULLBRIGHT -> white; absLight -> that
  colour; RF_GLOW -> handled by pulse; else -> VK_LightPoint_SampleRGB(origin).
  Then * entity color, RF_MINLIGHT floor (0.1 when near-black), RF_GLOW pulse.
KEY SCALING: VK_LightPoint_SampleRGB returns the raw lightmap (0..1, GL1
fallback 0.25). Our world surfaces draw at lm*2.0 (effective modulate 2), so the
sampled model light is scaled by 2.0 with the same desaturating peak-cap (peak>2
-> scale to 2) world.frag uses, so the player matches the brightness of the floor
it stands on. (GL uses gl_modulate=1 for BOTH world and R_LightPoint; we use 2
for both - consistent within VK.) In-shader dynamic lights still add on top, so
models near torches/lights still light up per-pixel.
Broad change: affects ALL flex models (player, monsters, items) - they now
respect world light like GL instead of rendering fullbright-ish.

## v100 - per-face SURF_FLOWING (water flowing in VK but not GL)
User: some Andoria water FLOWS in VK but is just turbulent-in-place in GL; unsure
which is right. ANALYSIS: both renderers gate the flow scroll on SURF_FLOWING
with the same formula, but ref_gl1 evaluates the flag PER-FACE
(fa->texinfo->flags in R_EmitWaterPolys), while our water pass used the
PER-BATCH surf_flags which is a bitwise OR across all faces sharing a texture
(vk_world.c batch_surf |= ...). So if any one face of a water texture was
SURF_FLOWING (e.g. an inflow), the whole pool of that texture flowed in VK.
GL is correct (honors per-face map data); VK was contaminating siblings.
FIX: added per-face surf_flags to pvs_face_t (set from ti->flags when the face
is emitted), and the water draw now uses pf->surf_flags for the warp/flow/
undulate decision (falling back to batch sf if 0). Alpha/trans still from the
batch (uniform per texture). VK now matches GL: only genuinely-flowing faces
flow.

## v101 - model mesh gap (Morph Ovum egg missing a wedge)
User: the Morph Ovum egg pickup has a wedge-shaped section of mesh missing.
ROOT CAUSE: the per-node glcmd walk in VK_Model_DrawEntity used fixed 256-entry
temp arrays (tmp_uv/tmp_pos/tmp_nrm[256]) and clamped nprim to 256. ref_gl1
walks each triangle fan/strip with NO vertex cap (immediate mode glBegin/glEnd).
Any single primitive with >256 verts had its tail silently dropped, leaving a
pie-slice/wedge hole - exactly the egg's missing section.
FIX (vk_model.c): raised the per-primitive limit to VKM_MAX_PRIM_VERTS (2048)
via static temp arrays (avoids a large per-call stack alloc; renderer is
single-threaded so static is fine). Added a one-shot diagnostic logging any
primitive with >256 verts (model name + node + count) to confirm the cause.
TODO after user confirms: remove the diagnostic log.

## v102 - egg see-through band (flex models wrongly alpha-tested)
User: the Morph Ovum egg has a see-through horizontal band just below the top
(can see straight through to the far side) in VK; whole in GL. Glow sprite is
fine in both. v101 (>256 prim) was the WRONG theory - the egg-primitive
diagnostic confirmed all 30 fans are count=4, no truncation.
ROOT CAUSE: entity.frag applied a global alpha-test discard (c.a < 0.05) to ALL
entities including flex models. ref_gl1 gl1_FlexModel.c draws flex models with
NO glAlphaFunc/alpha test. The egg's .m8 skin has a band of palette index-255
texels (-> alpha 0 via our LoadM8); our discard punched that band through,
showing the background. GL drew it opaque.
FIX:
- entity.frag: added a "no alpha test" mode (fog_extra.w < -0.5). is_sky still
  w>0.5; cutout sprites/decals/alpha-textured keep w>=-0.5 and still discard.
- vk_model.c: after Model_FillFogParams, set pc[31] (fog_extra.w) = -1.0 for
  flex models UNLESS RF_ALPHA_TEXTURE is set (those genuinely want cutout). So
  plain models never alpha-test (matches GL); alpha-textured models still do.
Regenerated vk_shaders.c (shaders.sh) for the entity.frag change.
Removed the v101/v102 egg primitive diagnostics.

## v103 - flex model alpha: match ref_gl1 exactly via skin has_alpha
v102 (skip alpha test unless RF_ALPHA_TEXTURE) fixed the egg but broke other
models that legitimately need the cutout/alpha. Replaced the heuristic with
ref_gl1's ACTUAL rule.
ref_gl1 R_DrawFlexModel + R_HandleTransparency:
- Enter the transparency path only when:
      color.a != 255 || (flags & RF_TRANS_ANY) || skin->has_alpha
  Outside it: NO blend, NO alpha test (fully opaque).
- GL sets image->has_alpha = 1 for EVERY .m32 image, and leaves .m8 images
  has_alpha = false. (So .m32-skinned models are alpha-tested/blended; .m8-
  skinned models like the Morph Ovum egg draw fully opaque - no index-255
  discard.)
- Within the transparency path, alpha test GL_GREATER 0.05 is ON for the normal
  translucent case and for RF_TRANS_ADD + RF_ALPHA_TEXTURE, but OFF for plain
  RF_TRANS_ADD / RF_TRANS_ADD_ALPHA (pure additive).
IMPLEMENTATION:
- vk_image.c: added image_t.has_alpha; LoadM32 sets true (all m32), LoadM8 sets
  false (matches GL). Added VK_ImageHasAlpha() accessor (+ header decl). White
  fallback + RGBA textures default false via memset.
- vk_model.c: resolve the skin image pointer (entity skin or model skin), then
  apply GL's exact trans_path + alpha_test decision; when alpha test is off set
  pc[31] = -1 (shader skips the discard; entity.frag no_alpha_test mode from
  v102). .m8 keeps index-255 -> alpha 0 baked so sprites/surfaces that DO opt
  into alpha testing still cut out.
This is the general fix across ALL flex models, keyed off the same data GL uses.

## v104 - additive models blow out to white in fog
User: v103 broke the health pickups in fog again - they render as solid white
capsules. ROOT CAUSE: Model_FillFogParams applies the global fog to ALL models;
ref_gl1 R_HandleTransparency does glDisable(GL_FOG) for additive models
(RF_TRANS_ADD / RF_TRANS_ADD_ALPHA). In a foggy area the additive health-pickup
glow got mixed toward the bright fog color -> solid white blob. (The earlier
additive-sprite fog-off fix covered the SPRITE path; pickups use the MODEL path,
which still applied fog.)
FIX (vk_model.c): after Model_FillFogParams, if RF_TRANS_ADD|RF_TRANS_ADD_ALPHA
set pc[27] (fog_color.w mode) = -1 to disable fog for that draw, matching GL.
The v103 alpha-rule work was correct and unchanged; this is the missing fog-off
for additive models.

## v105 - EmitQuad overflow FIXED (grow-on-demand) + GL parity audit
The 2D quad buffer (vk_draw.c) hit its fixed ceiling on the loading screen (full
book background + entire console scrollback at high res) and dropped quads,
printing "EmitQuad cursor overflow (cursor 98304 max 98304)". FIXED (not
silenced): the per-frame VBO now GROWS on demand.
- frame_vbo_t gained capacity + a retired-buffer slot.
- GrowFrameVBO(): on overflow, FlushBatch the pending quads (they referenced the
  old buffer, correct), allocate a 2x buffer, copy the in-progress un-flushed
  batch [s_batch_start,cursor) into it at the same indices so absolute vertex
  indices stay valid, retire the old buffer, rebind the new one.
- The retired buffer is freed in VK_Draw_BeginFrame next time this frame slice is
  reused (its fence has been waited on by then - safe with frames-in-flight=2).
  Also freed in VK_DrawShutdown.
- BeginQuad grows instead of dropping; EmitQuad's overflow path is now an
  unreachable guard (no console message).
Verified: full clean rebuild of all 9 libs, ref_vk dlopen OK (no undefined syms).

### GL feature-parity audit (this session)
- Renderer API surface: VK implements ALL 44 re.* entry points GL does. None
  missing, none extra.
- Per-frame pipeline matches GL R_RenderView step-for-step: dlights (in-shader),
  frame setup, frustum cull, PVS (R_MarkLeaves), world, entities, alpha-surface/
  water interleave (R_SortAndDrawAlphaSurfaces), BOTH particle lists (normal +
  additive aparticles), screen flash (R_PolyBlend). Nothing visual absent.
- KNOWN DIVERGENCES (low impact, documented, NOT yet addressed):
  1. R_EndRegistration is empty in VK; GL frees models/textures unused by the new
     level (Mod_Free + R_FreeUnusedImages). VK keeps prior-level assets resident.
     Bounded: engine-model WRAPPERS already reset each map (VK_EModel_
     BeginRegistration), and the flex/texture caches are name-deduped, so growth
     tends toward "all assets seen this session", not an unbounded per-frame leak.
     To implement faithfully: add registration_sequence to image_t + flex cache,
     stamp on each find/load, exempt never-free textures (conchars/fonts/reflect/
     particle), and tear down Vulkan resources only when guaranteed idle. Invasive
     + crash-risky if rushed; deserves its own focused session.
  2. R_FindSurface returns 0 (stub). Engine marks it //TODO: unused (client.h),
     so GL's full BSP-walk implementation is never actually called. No real gap.
  3. Debug primitives (AddDebugBox/Line/Label/etc) are no-ops under non-_DEBUG;
     developer visualization tools only, not gameplay. GL implements them.

## v106 - removed leftover diagnostic console spam
User still saw "vk: model 'models/items/health/...' flags=... pipe=add" printed
in-game. Removed the two leftover diagnostics flagged in the v105 audit:
- vk_model.c: the per-entity translucent/alpha-textured model logger (s_md<40);
  fired in-game for the first 40 additive/translucent models (health pickups).
- vk_world.c: the per-submodel-batch "SUBMODEL trans batch" logger (load-time).
Swept all remaining Con_Printf in the renderer: only legitimate one-time load
messages, errors, and the EmitQuad NULL-mapped one-shot guard remain. No per-
frame/per-entity logging left.

## v107 - resource management: free per-level assets at level change (Part 1)
Implements the R_EndRegistration asset-freeing that was the one known divergence
from ref_gl1 (VK previously kept every texture/model from every level resident
for the whole session). Matches GL's registration_sequence + R_FreeUnusedImages.

Mechanism:
- Descriptor pools (2D in vk_pipeline.c, world-3D in vk_pipeline3d.c) now created
  with VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT so individual sets can
  be freed. Added VK_FreeDescriptorSet / VK_FreeWorldDescriptor.
  (Side benefit: the cinematic code already called vkFreeDescriptorSets on the 2D
  pool - that was only valid once the FREE bit was added.)
- image_t gained reg_seq + permanent. s_image_reg_seq bumped each map; stamped on
  every FindByName/AllocImageSlot. VK_FreeUnusedImages() frees texture+descriptor
  for any non-permanent image not touched this sequence. AllocImageSlot now reuses
  freed interior slots so the pool doesn't creep to the cap.
- vk_model_s gained reg_seq. VK_Model_FreeUnused() frees CPU frame/glcmd blocks,
  per-skin world descriptors, and skeletal data for stale models; reuses slots.
- R_BeginRegistration bumps a shared sequence (BEFORE world load, so the new
  map's assets stamp current); R_EndRegistration does vkDeviceWaitIdle then
  VK_Model_FreeUnused + VK_FreeUnusedImages.
- PERMANENT assets (never evicted): white fallback, conchars (both load sites),
  font1/font2 atlases, reflect sphere-map, particle + aparticle textures.
- Fixed a pre-existing world-texture descriptor leak: VK_World_LoadMap reset the
  world-texture table to 0 without freeing the per-level diffuse+lightmap pair
  descriptors. Added VK_ResetWorldPairDescriptors() (whole-pool reset, all per-
  level, none permanent), called from VK_World_Free after a device-idle wait.

Safety: all eviction happens at level load behind vkDeviceWaitIdle, so no freed
texture/descriptor is referenced by an in-flight frame. Sequence is bumped before
the new map loads, so current-level assets are never evicted. Cinematic texture
is standalone (not in the image cache), unaffected.

NEEDS: transition-heavy playtest. This is teardown code - verify (a) no missing
textures/HUD after several map changes, (b) memory stays flat across transitions
(the "freed N unused textures/models" console lines confirm eviction is firing),
(c) revisiting a map re-loads its assets cleanly.

## v108 - performance: flex-model frustum cull + skip wasted normal work
Two performance wins in the per-frame flex-model path (vk_model.c), both also
closing small divergences from ref_gl1. Also demoted leftover load prints.

1. Stray console prints (the "plague bomb" report): the flex-model, skeletal,
   sprite and sky load-confirmation messages fired on lazy mid-game asset loads
   (e.g. an enemy projectile spawning its effect). Demoted PRINT_ALL ->
   PRINT_DEVELOPER (routed through Com_DPrintf, only shown with "developer 1"),
   matching GL which does not print load info during gameplay. Swept the whole
   renderer: no console prints remain in any gameplay/per-frame path.

2. Skip per-vertex normal decode+transform for non-reflective models. The normal
   (nx,ny,nz) is consumed ONLY by the reflect shaders (entity_reflect.*); the
   base entity shader ignores it. Previously every model (every monster, player,
   item) ran DecodeLerpNormal + XformNormal per vertex and uploaded a normal the
   shader threw away. Now computed only when node_reflect is set; otherwise a
   cheap constant is written. Saves a decode + 3x3 transform per vertex for the
   overwhelming majority of model vertices each frame.

3. Frustum-cull flex models before skinning (ref_gl1 R_CullFlexModel parity).
   DrawEntityList previously dispatched EVERY entity with no cull, so off-screen
   monsters/items were fully CPU-skinned, uploaded, and draw-recorded every
   frame. Now: compute the model's local AABB from current+old frame
   translate/scale, transform the 8 corners with the SAME entity transform used
   to draw (XformPoint, so the cull box exactly matches on-screen geometry),
   and reject if all 8 corners fall outside one frustum plane (aggregate-mask
   test, matches GL). Skipped for viewmodels (RF_DEPTHHACK) and honors r_nocull.
   New VK_World_CullWorldCorners() in vk_world.c exposes the frustum test.

Risk note: the cull reuses the exact draw transform (not a hand-rolled angle
basis) specifically to avoid the classic "models pop at screen edges" bug. Watch
for any model disappearing while still partly visible - would indicate the frame
AABB is tighter than the actual animated verts for some model. r_nocull 1
disables culling to A/B test if anything looks wrong.

## v109 - CRITICAL FIX: device-lost from sky descriptor referencing evicted image
Symptom: after some map transitions, per-frame validation spam "vkCmdDraw():
descriptor ... using imageView 0x0 that is invalid or has been destroyed",
escalating to VK_ERROR_DEVICE_LOST (GPU hang) + fence/semaphore cascade.

ROOT CAUSE: the v107 resource eviction (VK_FreeUnusedImages) interacting with
VK_Sky_Set's same-name early-out. When two consecutive maps use the SAME sky,
VK_Sky_Set('X') early-returned without re-resolving the 6 sky images through the
image cache, so they were NOT re-stamped with the new registration sequence.
R_EndRegistration then saw them as stale (last used by the previous map) and
VK_FreeUnusedImages destroyed their image views - but s_sky_desc[] still held
descriptors pointing at those views, and the sky is drawn every frame, so every
sky draw referenced a destroyed view -> device lost. (Explains the intermittent
nature: only triggers when adjacent maps share a sky name.)

FIX (vk_sky.c): on the same-name early-out, re-touch the 6 sky images via
VK_FindImage (a cheap cache hit) so they are stamped current and survive
eviction. The reload path (different sky) was already fine.

AUDIT: checked every persistent descriptor/image cache for the same class of bug
(cached descriptor referencing an image-cache view that eviction can destroy):
- draw_chars, bf_atlas1/2, white, particle/aparticle, reflect: marked permanent
  in v107 -> safe.
- cinematic: standalone texture, not in image cache -> safe.
- HUD pics: re-registered by client each map (VK_FindPic stamps) -> safe.
- sky: was the ONLY unprotected cache; now fixed. Sky is correctly NOT made
  permanent (it legitimately changes between maps; old skies should evict when
  the new map uses a different one).

## v110 - CRITICAL: complete the cache-hit re-stamp fix (device-lost cont'd)
v109 fixed sky but the SAME bug existed for other cached-asset types. Crash on
load into canyon.bsp ("freed 382 unused textures", then imageView 0x0 on ~4
persistent descriptors -> device lost).

GENERAL ROOT CAUSE (bug class): any cache that stores an image_t* (or a
descriptor built from an image's view) across registrations AND has a cache-hit
early-out that skips the normal VK_FindImage re-resolve will NOT re-stamp those
images with the new registration sequence. VK_FreeUnusedImages then evicts them
(destroying the image view) while the cached descriptor still references it ->
GPU device-lost on the next draw.

Found and fixed ALL instances (added VK_Image_Touch(img) helper in vk_image.c
that re-stamps reg_seq; called on each cache hit):
- vk_model.c: cached flex model (FindCached hit) - re-stamp m->skins[]. THIS was
  the canyon crash (model skins shared with the previous map).
- vk_sprite.c: cached sprite (SpriteFindCached hit) - re-stamp frames[].image.
- vk_draw.c: cached book (Book_FindCached hit) - re-stamp skins[] (latent: open
  book, change map, reopen).
- vk_sky.c: switched the v109 same-name re-resolve to the VK_Image_Touch helper
  (functionally identical, cleaner).
Audited every image_t*-holding cache: world textures are re-resolved each load
(WorldTex_Resolve, table reset to 0) so always re-stamped - safe; image-cache
FindByName already re-stamps - safe. These four were the complete set.

Lesson: the v107 eviction needs EVERY long-lived image reference to be re-stamped
each registration. Cache-hit fast-paths are the trap. If another imageView 0x0
device-lost appears, look for a new cached image_t* with a skip-on-hit path.

## v111 - FIX: level-change eviction freed the main-menu (book) textures
User: after a level change the main menu was unusable - its textures had been
freed. The main menu is drawn via re.BookDrawPic (a .bk composite, menu.c:1351).

ROOT CAUSE: VK_BookDrawPic drew each segment via the CACHED image_t* pointer
(book->skins[i]) resolved once at book load. The menu/books are not part of any
map's registration, so on level change VK_FreeUnusedImages freed their segment
images, leaving book->skins[i] dangling -> menu drew nothing usable. (The v110
"re-stamp cached book skins on cache hit" was ineffective here: the menu book is
not re-loaded during a map change, so the cache-hit path never ran before
eviction; and re-stamping an already-freed slot pointer is meaningless.)

GENERAL PRINCIPLE (now explicit): a draw path that uses a cached image_t* is only
safe across eviction if that asset is GUARANTEED re-registered every map (so its
cache-hit re-stamp fires before eviction). Assets drawn outside the map
registration cycle (menu, books) must RE-RESOLVE BY NAME each draw, exactly like
VK_Draw_Pic does, so an evicted image is transparently reloaded.

FIX (vk_draw.c VK_BookDrawPic): re-resolve each segment image via VK_FindImage
("Book/<segment name>") at draw time instead of using book->skins[i]. Reverted
the v110 book cache-hit re-stamp (superseded). 

Audit of all 2D/UI draw paths vs eviction:
- Draw_Pic/StretchPic/TileClear/GetPicSize: re-resolve by name -> safe.
- Draw_Char (conchars), BigFont (font atlases): permanent -> safe.
- BookDrawPic: NOW re-resolves by name -> safe.
- sprites, model skins: cached image_t* BUT re-registered every map (cache-hit
  re-stamp from v110 fires before eviction, pointer still valid) -> safe.
- sky: re-stamped via VK_Sky_Set each map -> safe.
- particle/aparticle/reflect/white: permanent -> safe.
This completes the eviction-safety pass across every image reference.

## v112 - FIX: death-reload device-lost (model/sprite/sky 3D descriptors dangling)
User: after dying and reloading the saved game, the renderer crashed. Validation
log: "freed 116 unused textures (level change)" immediately followed by a broad
burst of VUID-vkCmdDraw-None-08114 (imageView 0x0) across ~30 descriptor sets,
narrowing to a steady per-frame pair (0x24a2, 0x24a7), escalating to
VK_ERROR_DEVICE_LOST + the fence/semaphore/command-buffer cascade.

ROOT CAUSE (section-9 bug class, 3D path): the eviction is GL-faithful - a reload
into an already-visited map re-registers only the baseline configstring assets,
so textures lazy-loaded mid-game (enemy skins, gibs, effect sprites) go stale and
are correctly freed (the 116). The defect was the VK CONSUMER side: model skins
(m->skin_desc[]), sprite frames (frames[].descriptor) and sky (s_sky_desc[]) each
cached a world-3D VkDescriptorSet built from an image's view. VK_FreeUnusedImages
freed only the image's OWN 2D descriptor and destroyed the view; those separate
cached 3D sets were never nulled, so they stayed bound referencing a destroyed
view -> imageView 0x0 -> device-lost. ref_gl1 survives the identical eviction
only because binding a freed GL texture id is benign; Vulkan treats a destroyed
view in a bound descriptor as fatal, so "match GL eviction" is not the fix (the
eviction already matches). The v110/v109 re-touch only protects assets that ARE
re-registered; lazy-loaded-but-not-re-registered entities still drawn for a frame
after reload slipped through (hence the broad burst then steady pair).

FIX: the world-3D descriptor now lives ON THE IMAGE (image_t.world_descriptor),
allocated lazily by VK_ImageWorldDescriptor(img) and freed+nulled in
VK_FreeUnusedImages alongside the texture - exactly mirroring the existing 2D
img->descriptor lifetime. Model skins, sprite frames and sky fetch it at draw
time: a freed image returns VK_NULL_HANDLE (caller already null-checks and skips
the draw, like GL drawing nothing), and a reused slot gets a fresh descriptor (no
dangle). Removed the now-dead m->skin_desc[], vk_sprite_frame_t.descriptor and
s_sky_desc[] (alloc + free + decls). Net world-3D descriptor usage DROPS (one per
image vs one per model-skin, duplicates collapsed). The v110/v109 image re-touch
on cache-hit is kept (still keeps re-registered skins resident this level, avoids
re-upload) but is no longer load-bearing for crash-safety.

GENERAL PRINCIPLE (now explicit for the 3D path too): a cached VkDescriptorSet
built from an image view is eviction-safe ONLY if it is freed when that image is
freed. The robust pattern is to store the descriptor on the image so it shares
the image's lifetime, rather than caching it in the consumer and relying on the
consumer being re-registered. This is the 3D analogue of the v111 "re-resolve by
name" 2D rule.

Audit of all world-3D image-backed descriptors vs eviction (believed complete):
- model skins, sprite frames, sky: NOW image-owned (VK_ImageWorldDescriptor),
  freed with the image -> safe by construction (no re-registration dependency).
- particle/aparticle/reflect: permanent images -> safe.
- world (pair pool): rebuilt every load (VK_World_Free resets the pool) -> safe.
- cinematic: standalone texture, not in the image cache -> safe.
This closes the eviction-safety pass for the 3D descriptor path. If a NEW
imageView 0x0 appears, look for a consumer-cached VkDescriptorSet not sourced from
VK_ImageWorldDescriptor / not rebuilt per load.

## v113 - FIX (the real one): world same-map reload eviction; v112 corrected
CORRECTION TO v112: the v112 image-owned 3D-descriptor change (model/sprite/sky)
is real, kept hardening of the 3D path, but it was NOT the cause of the reported
death-reload crash. The crash persisted unchanged after v112 (same VUID-08114
imageView 0x0 burst + steady pair, same "freed N unused textures (level change)"
opener). The decisive evidence was in the log: on the failing reload (>kill, then
load a save into the CURRENT map) there was NO "vk: BSP ... loaded" / "vk: world
... loaded" line, whereas a changelevel INTO that same map earlier printed all
three. So the world was being REUSED, not reloaded.

REAL ROOT CAUSE: VK_World_LoadMap (vk_world.c) has a same-map early-out:
    if (s_loaded && strcasecmp(s_loaded_name, name) == 0) return true;
On a reload into the current level this keeps the existing world geometry,
textures and pair descriptors and returns immediately. But R_BeginRegistration
has already bumped the registration sequence, and the early-out did NOT re-stamp
the world texture images. So every world texture kept its old reg_seq, and
R_EndRegistration's VK_FreeUnusedImages then evicted them (destroying their image
views) while the persistent world PAIR descriptors (wt->descriptor, built from
wt->image's view + the lightmap view) still referenced those destroyed views ->
imageView 0x0 -> device-lost. The steady per-frame pair = always-visible world
surfaces; the burst = surfaces visible as the PVS settles over the first frames.
This is the EXACT bug class as the v109 sky same-name re-touch - the sky early-out
re-touched its images, the world early-out did not.

FIX (vk_world.c VK_World_LoadMap same-map early-out): before returning, re-stamp
every world texture as used in the new registration sequence:
    for (int i = 0; i < s_num_world_textures; i++)
        if (s_world_textures[i].image) VK_Image_Touch(s_world_textures[i].image);
    return true;
VK_FreeUnusedImages now spares them, so the reused pair descriptors stay valid.
R_BeginRegistration bumps the seq before VK_World_LoadMap, so the touch stamps the
current seq. The lightmap (s_lm_tex) is a standalone texture created directly via
VK_CreateTextureRGBA, NOT in the image cache, so it is not subject to eviction and
needs no touch.

Why ref_gl1 survives the identical same-map reuse: binding a GL texture id whose
backing was freed is benign (renders garbage/nothing); Vulkan treats a destroyed
view in a bound descriptor as fatal. So, as always in this port, the fix is not to
change the eviction (it is GL-faithful) but to keep live descriptors' images
resident.

EVICTION-SAFETY AUDIT - now complete across BOTH the reload-reuse paths and the
descriptor caches:
- Reuse-without-reload paths MUST re-touch their images: sky (v109, done),
  world (v113, done). These are the only two reuse early-outs.
- Cached descriptors built from an image view are eviction-safe if image-owned
  (model/sprite/sky 3D via VK_ImageWorldDescriptor, v112) or rebuilt per load
  (world pair pool) or sourced from permanent images (particles) or re-resolved
  by name each draw (all 2D, v111).
If another imageView 0x0 appears, the two questions are: (1) is there a new
reuse-without-reload path that skips re-touching its images? (2) is there a new
cached descriptor that is neither image-owned, rebuilt per load, nor from a
permanent image?

## v114 - FEATURE: gamma / brightness / contrast sliders (were no-ops under VK)
User: the contrast, gamma and brightness sliders in the Video menu do nothing
under Vulkan. ROOT CAUSE: ref_vk never registered or consumed vid_gamma /
vid_brightness / vid_contrast. The menu moved the cvars but nothing applied them.

H2's GL renderer does NOT use a hardware gamma ramp - it bakes these three cvars
into texture albedo at load time through a 256-entry byte LUT (gl1_Image.c
R_InitGammaTable builds it; GrabPalette applies it to .m8 palettes, R_ApplyGamma32
to .m32 RGB), i.e. gamma is applied to albedo BEFORE lighting and blending. On a
slider change RI_BeginFrame rebuilds the LUT and R_GammaAffect re-loads each image
from disk and re-uploads it (so repeated changes never accumulate error). The
lightmap is built separately and is intentionally left ungamma'd.

FIX (vk_image.c, mirroring GL exactly):
- VK_InitGammaTable(): identical LUT math to R_InitGammaTable; registers the three
  cvars (defaults 0.5, CVAR_ARCHIVE) on first call. Built in VK_InitImages before
  any texture loads.
- ApplyGammaRGBA(): runs RGB (alpha preserved) of an RGBA run through the LUT.
- LoadM32 bakes the LUT into a writable copy before upload; LoadM8 bakes it into
  the expanded RGBA buffer in place. So every disk-loaded texture is gamma'd at
  load, like GL. (The 1x1 "*white" utility texture is created from a hardcoded
  pixel and is deliberately NOT gamma'd - it backs Draw_Fill's coloured quads, so
  gamma'ing it would tint solid fills.)
- DecodeImageRGBA(): shared file->gamma'd-RGBA decoder (dispatches on the stored
  name's .m32/.m8 extension) used by both load and refresh so they can't diverge.
- VK_GammaRefreshIfNeeded(): called at the top of VK_BeginFrame_impl. If a slider
  moved (cvar ->modified) or vid_textures_refresh_required==1 (set by the video
  menu on close), rebuild the LUT and re-bake every disk-loaded texture in place
  via VK_UpdateTextureRGBA. In-place update keeps each image's VkImageView and
  descriptor, so it is eviction-safe (no dangling-descriptor risk). A
  vkDeviceWaitIdle guards frames-in-flight; it only runs on an actual change, so
  the stall is a one-off per slider notch.

Differences from GL, both deliberate:
- GL has an it_pic/it_sky-only live-refresh path while the menu is open (perf); the
  VK image_t has no type field, so VK refreshes ALL disk-loaded textures on a
  change. VK single-mip textures + fast staging make the full re-bake cheap enough
  for a per-notch one-shot, and it means the world updates live behind the menu
  rather than only on close. Correctness is identical.
- VK textures are single-mip (mipLevels=1), so unlike R_ApplyGamma32 there are no
  mip levels to walk; the base level is the only data.

Faithfulness note: because the LUT is baked into albedo (not applied as a
post-process to the final frame), the result matches GL pixel-for-pixel - lighting,
fog and blending all operate on already-gamma'd albedo exactly as in ref_gl1. A
final-frame post-process would have been simpler but WRONG (it would gamma the
composited result including lightmaps/fog/additive blends).

## v115 - FEATURE: gl_minlight (minimum light floor); + settings audit
Follow-up to the v114 gamma work: audited every cvar ref_gl1 acts on vs what VK
honors, to find any other menu/render settings silently ignored under Vulkan.

AUDIT RESULT - one genuine user-facing miss (gl_minlight, fixed here). Everything
else accounted for:
- Screen flashes (damage/pickup/powerup/underwater) are NOT missed: H2 does not
  use the classic refdef.blend / gl_polyblend path (cl.refdef.blend is cleared and
  never populated; GL's R_PolyBlend is vestigial). The real flashes go through H2's
  ri.Is_Screen_Flashing() callback, which VK already implements in R_RenderFrame.
- gl_modulate, the r_fog_* family, r_underwater_color: already honored by VK.
- Debug / GL-API-specific cvars meaningless under Vulkan: gl_showtris, gl_lightmap,
  gl_drawflat, gl_cull, gl_clear, gl_lockpvs, gl_nobind, gl_reporthash, gl_finish,
  gl_ztrick, gl_drawbuffer, r_speeds, r_norefresh, r_drawworld, r_drawentities,
  flushmap, r_frameswap.
- Client/platform cvars the renderer never owned: vid_ref, vid_fullscreen,
  vid_maxfps, vid_mode, menus_active. r_detail is ignored by ref_gl1 too (detail
  textures unimplemented in this engine), so VK matches GL.
- Console-only toggles where VK already does the default behavior (only matter if
  flipped by hand): gl_dynamic (VK dlights always on), r_lerpmodels (VK always
  interpolates), gl_texturemode (VK samplers fixed to linear), gl_flashblend,
  gl_saturatelighting, gl_bookalpha. r_fog_lightmap_adjust (world-pass fog distance
  scale, 5.0) is not read by VK, but VK fog was validated as visually correct, so
  its fog model compensates; left as-is.

gl_minlight FIX (port of ref_gl1 R_InitMinlight + its two apply sites):
- vk_lightpoint.c: VK_InitMinlight() builds the 256-entry LUT with identical math
  ( inf = (255-ml)*i/255 + ml ), registers gl_minlight ("0", CVAR_ARCHIVE). Shared
  via vk_minlight[256] / vk_minlight_set (extern in vk_lightpoint.h) so both the
  world bake and entity shade index it directly. VK_Minlight_CheckModified()
  rebuilds + reports change for the live re-bake.
- World lightmaps (vk_world.c BakeSurfaceLightmap): luxel RGB run through the LUT
  after the desaturating peak clamp, exactly where ref_gl1 R_BuildLightMap applies
  minlight[]. Built at load (VK_InitMinlight before the bake loop) so static
  lightmaps honor the floor immediately, like GL.
- Entity/model shade (vk_model.c): shade run through the LUT before entity-color
  modulation, on the lit paths only (absLight, sampled world light) - not
  fullbright/glow/additive - matching gl1_FlexModel.c's apply_minlight gate.
- LIVE updates (better UX than GL, which only rebuilds animated surfaces per frame
  and needs a reload for static ones): every lit surface is now retained in
  s_anim_surfs with an 'animated' flag (previously only animated surfaces were
  kept). VK_World_UpdateLightstyles re-bakes only animated surfaces per frame as
  before; on a gl_minlight change it re-bakes ALL retained surfaces once and
  uploads them via the existing batched region path. Entity shade is per-frame so
  it tracks the slider with no extra work. Eviction-safe: re-baking writes the CPU
  atlas + re-uploads regions into the existing s_lm_tex (no view/descriptor churn).

## v116 - FEATURE: dynamic lights on inline submodels (doors/lifts/platforms)
Last outstanding feature gap. Submodel (inline brush model) surfaces took the
static lightmap but no dynamic lights: the submodel pass bound the world-space
dlight set yet forced the dlight-enable push constant to 0, because submodel
verts are stored at BSP rest coords and moved at draw time by M = T(origin)*
R(angles), so v_worldpos (= in_pos in world.vert) was the REST position, not the
vertex's current world position - the world-space dlight distance test would be
wrong.

FIX: world.vert now takes a model->world matrix in the push constant and sets
v_worldpos = (model * in_pos).xyz. gl_Position still uses pc.mvp (= mvp*M for
submodels, = viewproj for the world). The static world passes model = identity
(verts already world space -> v_worldpos = in_pos, unchanged); each submodel
passes its M, so v_worldpos is the vertex's current world position and dlights
are enabled. Fog is untouched (still uses in_pos vs the local-space fog_cam, a
matched pair). Opaque-world push-constant range grew 112 -> 176 bytes (added the
mat4); both opaque push sites (main world, submodel) now write the full range.
Warp surfaces don't sample dlights, so the warp path was left as-is. SPIR-V
regenerated via shaders/shaders.sh.

Push-constant size note: 176 bytes exceeds the 128-byte Vulkan guaranteed
minimum, but the warp pipeline already uses 144 and runs on the target Intel
Iris Xe (Mesa reports 256), so this is within the established assumption.

## v117 - FEATURE: developer debug-draw primitives (_DEBUG), VK port of gl1_Debug.c
For like-for-like parity with ref_gl1 ahead of a possible Windows backport. New
vk_debug.c mirrors gl1_Debug.c's bookkeeping 1:1 (slot allocation, lifetimes,
box/arrow/marker geometry, entity-bbox/label interpolation) and adds the Vulkan
drawing the GL immediate mode can't do directly:
- A LINE_LIST pipeline (new debug.vert/.frag), no descriptor sets, push-constant
  mat4 mvp, depth test OFF (so primitives show through geometry, like GL), no
  blend, dynamic viewport/scissor. Created lazily on first use.
- Per-frame, per-in-flight-frame host-visible vertex buffers; all primitives are
  flattened into one LINE_LIST and drawn in a single vkCmdDraw.
- Labels: world point projected through the frame MVP (column-major, accounting
  for the proj's -f Y term -> VK y-down NDC) to screen pixels, then drawn with the
  existing conchar 2D path (VK_Draw_Char) using GL's ui_scale logic.
- Full RI_AddDebug* set incl. RI_AddDebugMarker.

Gating matches GL exactly: vk_debug.c compiles in all builds, but the refexport
pointers are only overridden with the real RI_AddDebug* under _DEBUG (retail keeps
the no-op stubs), and VK_Debug_DrawPrimitives/DrawLabels are only called under
_DEBUG in R_RenderFrame. VK_Debug_Free() clears primitives on R_BeginRegistration
so stale edict pointers don't dangle across maps; VK_Debug_Shutdown() tears down
the pipeline/buffers in R_ShutdownContext. SPIR-V regenerated via shaders.sh.

Note: the Linux retail build defines NDEBUG and never _DEBUG, and the game-side
call sites (cs_shared/Debug.c) are _DEBUG-only and use MSVC secure-CRT (vsprintf_s
etc.) with no Linux shim, so this is dormant on Linux by design - it exists for a
_DEBUG build (Windows backport). Verified: vk_debug.c and vk_main.c both compile
clean with -D_DEBUG (ref_vk doesn't pull in Debug.c, so the secure-CRT issue
doesn't affect the renderer).
