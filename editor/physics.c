#include "physics.h"
#include "vulkan_setup.h"
#include "scene.h"
#include "context.h"
#include "./vendor/joltc/include/joltc.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

static JPH_PhysicsSystem* physics_system = NULL;
static JPH_JobSystem* job_system = NULL;

static JPH_BroadPhaseLayerInterface* bp_interface = NULL;
static JPH_ObjectLayerPairFilter* obj_obj_filter = NULL;
static JPH_ObjectVsBroadPhaseLayerFilter* obj_bp_filter = NULL;

static const JPH_ObjectLayer OBJ_LAYER_NON_MOVING = 0;
static const JPH_ObjectLayer OBJ_LAYER_MOVING = 1;
static const JPH_BroadPhaseLayer BP_LAYER_NON_MOVING = 0;
static const JPH_BroadPhaseLayer BP_LAYER_MOVING = 1;

void physics_init(void) {
    if (!JPH_Init()) {
        fprintf(stderr, "[JOLT] Failed to initialize JoltPhysics.\n");
        return;
    }

    JobSystemThreadPoolConfig job_cfg = {
        .maxJobs = JPH_MAX_PHYSICS_JOBS,
        .maxBarriers = JPH_MAX_PHYSICS_BARRIERS,
        .numThreads = -1
    };
    job_system = JPH_JobSystemThreadPool_Create(&job_cfg);

    // Setup BroadPhase Layer Interface Table
    bp_interface = JPH_BroadPhaseLayerInterfaceTable_Create(2, 2);
    JPH_BroadPhaseLayerInterfaceTable_MapObjectToBroadPhaseLayer(bp_interface, OBJ_LAYER_NON_MOVING, BP_LAYER_NON_MOVING);
    JPH_BroadPhaseLayerInterfaceTable_MapObjectToBroadPhaseLayer(bp_interface, OBJ_LAYER_MOVING, BP_LAYER_MOVING);

    // Setup Object Layer Pair Filter Table
    obj_obj_filter = JPH_ObjectLayerPairFilterTable_Create(2);
    JPH_ObjectLayerPairFilterTable_EnableCollision(obj_obj_filter, OBJ_LAYER_MOVING, OBJ_LAYER_MOVING);
    JPH_ObjectLayerPairFilterTable_EnableCollision(obj_obj_filter, OBJ_LAYER_MOVING, OBJ_LAYER_NON_MOVING);
    JPH_ObjectLayerPairFilterTable_EnableCollision(obj_obj_filter, OBJ_LAYER_NON_MOVING, OBJ_LAYER_MOVING);
    JPH_ObjectLayerPairFilterTable_DisableCollision(obj_obj_filter, OBJ_LAYER_NON_MOVING, OBJ_LAYER_NON_MOVING);

    // Setup Object vs BroadPhase Layer Filter Table
    obj_bp_filter = JPH_ObjectVsBroadPhaseLayerFilterTable_Create(bp_interface, 2, obj_obj_filter, 2);

    // Initialize Physics System
    JPH_PhysicsSystemSettings sys_settings = {
        .maxBodies = 10240,
        .numBodyMutexes = 0,
        .maxBodyPairs = 65536,
        .maxContactConstraints = 10240,
        .broadPhaseLayerInterface = bp_interface,
        .objectLayerPairFilter = obj_obj_filter,
        .objectVsBroadPhaseLayerFilter = obj_bp_filter
    };
    physics_system = JPH_PhysicsSystem_Create(&sys_settings);

    fprintf(stdout, "\033[32m[JOLT] Physics Subsystem Initialized.\033[0m\n");
}

void physics_cleanup(void) {
    if (physics_system) {
        JPH_PhysicsSystem_Destroy(physics_system);
        physics_system = NULL;
    }
    if (obj_bp_filter) {
        JPH_ObjectVsBroadPhaseLayerFilter_Destroy(obj_bp_filter);
        obj_bp_filter = NULL;
    }
    if (obj_obj_filter) {
        JPH_ObjectLayerPairFilter_Destroy(obj_obj_filter);
        obj_obj_filter = NULL;
    }
    if (bp_interface) {
        JPH_BroadPhaseLayerInterface_Destroy(bp_interface);
        bp_interface = NULL;
    }
    if (job_system) {
        JPH_JobSystem_Destroy(job_system);
        job_system = NULL;
    }
    JPH_Shutdown();
}

void physics_rebuild_mesh(Mesh* m) {
    if (!physics_system) return;
    JPH_BodyInterface* bi = JPH_PhysicsSystem_GetBodyInterface(physics_system);

    if (m->physics_body_id != 0) {
        JPH_BodyInterface_RemoveBody(bi, m->physics_body_id);
        JPH_BodyInterface_DestroyBody(bi, m->physics_body_id);
        m->physics_body_id = 0;
    }

    if (m->collider_type == 0) return;

    vec3 size;
    glm_vec3_sub(m->aabbMax, m->aabbMin, size);
    glm_vec3_scale(size, 0.5f, size);

    vec3 scale = {
        glm_vec3_norm((vec3){m->local_transform[0][0], m->local_transform[1][0], m->local_transform[2][0]}),
        glm_vec3_norm((vec3){m->local_transform[0][1], m->local_transform[1][1], m->local_transform[2][1]}),
        glm_vec3_norm((vec3){m->local_transform[0][2], m->local_transform[1][2], m->local_transform[2][2]})
    };

    JPH_Shape* shape = NULL;

    if (m->collider_type == 1) { // Box
        float hx = fmaxf(size[0] * scale[0], 0.01f);
        float hy = fmaxf(size[1] * scale[1], 0.01f);
        float hz = fmaxf(size[2] * scale[2], 0.01f);
        JPH_Vec3 extents = {hx, hy, hz};

        JPH_BoxShapeSettings* box_settings = JPH_BoxShapeSettings_Create(&extents, JPH_DEFAULT_CONVEX_RADIUS);
        shape = (JPH_Shape*)JPH_BoxShapeSettings_CreateShape(box_settings);
        JPH_ShapeSettings_Destroy((JPH_ShapeSettings*)box_settings);
    } else if (m->collider_type == 2) { // Sphere
        float r = fmaxf(size[0]*scale[0], fmaxf(size[1]*scale[1], size[2]*scale[2]));
        if (r < 0.01f) r = 0.01f;

        JPH_SphereShapeSettings* sphere_settings = JPH_SphereShapeSettings_Create(r);
        shape = (JPH_Shape*)JPH_SphereShapeSettings_CreateShape(sphere_settings);
        JPH_ShapeSettings_Destroy((JPH_ShapeSettings*)sphere_settings);
    }

    if (!shape) return;

    JPH_RVec3 jph_pos = {m->local_transform[3][0], m->local_transform[3][1], m->local_transform[3][2]};

    vec3 euler;
    glm_euler_angles(m->local_transform, euler);
    versor q;
    glm_euler_xyz_quat(euler, q);
    JPH_Quat jph_quat = {q[0], q[1], q[2], q[3]};

    JPH_BodyCreationSettings* body_settings = NULL;
    if (m->mass > 0.0f) {
        body_settings = JPH_BodyCreationSettings_Create3(shape, &jph_pos, &jph_quat, JPH_MotionType_Dynamic, OBJ_LAYER_MOVING);
        JPH_BodyCreationSettings_SetOverrideMassProperties(body_settings, JPH_OverrideMassProperties_CalculateInertia);

        JPH_MassProperties mass_props;
        JPH_BodyCreationSettings_GetMassPropertiesOverride(body_settings, &mass_props);
        mass_props.mass = m->mass;
        JPH_BodyCreationSettings_SetMassPropertiesOverride(body_settings, &mass_props);
    } else {
        body_settings = JPH_BodyCreationSettings_Create3(shape, &jph_pos, &jph_quat, JPH_MotionType_Static, OBJ_LAYER_NON_MOVING);
    }

    JPH_BodyCreationSettings_SetFriction(body_settings, m->friction);
    JPH_BodyCreationSettings_SetRestitution(body_settings, m->restitution);

    JPH_Body* body = JPH_BodyInterface_CreateBody(bi, body_settings);
    JPH_BodyCreationSettings_Destroy(body_settings);
    JPH_Shape_Destroy(shape);

    m->physics_body_id = JPH_Body_GetID(body);
    JPH_BodyInterface_AddBody(bi, m->physics_body_id, JPH_Activation_Activate);
}

void physics_step(float dt) {
    if (!physics_system || dt <= 0.0f) return;

    JPH_PhysicsSystem_Update(physics_system, dt, 1, job_system);

    JPH_BodyInterface* bi = JPH_PhysicsSystem_GetBodyInterface(physics_system);
    bool dirty = false;

    extern Scene scene;
    extern bool gizmo_is_dragging_mesh(int mesh_index);
    extern bool editor_is_grabbing_mesh(int mesh_index);

    for (uint32_t i = 0; i < scene.meshes.count; i++) {
        Mesh* m = &scene.meshes.items[i];
        if (m->physics_body_id != 0 && m->mass > 0.0f && !gizmo_is_dragging_mesh((int)i) && !editor_is_grabbing_mesh((int)i)) {
            JPH_RVec3 jph_pos;
            JPH_Quat jph_rot;
            JPH_BodyInterface_GetPosition(bi, m->physics_body_id, &jph_pos);
            JPH_BodyInterface_GetRotation(bi, m->physics_body_id, &jph_rot);

            vec3 pos = {jph_pos.x, jph_pos.y, jph_pos.z};
            versor q = {jph_rot.x, jph_rot.y, jph_rot.z, jph_rot.w};

            glm_mat4_identity(m->local_transform);
            glm_translate(m->local_transform, pos);

            mat4 rot_mat;
            glm_quat_mat4(q, rot_mat);
            glm_mat4_mul(m->local_transform, rot_mat, m->local_transform);

            vec3 scale = {
                glm_vec3_norm((vec3){m->model[0][0], m->model[1][0], m->model[2][0]}),
                glm_vec3_norm((vec3){m->model[0][1], m->model[1][1], m->model[2][1]}),
                glm_vec3_norm((vec3){m->model[0][2], m->model[1][2], m->model[2][2]})
            };
            glm_scale(m->local_transform, scale);

            glm_mat4_copy(m->local_transform, m->model);
            dirty = true;
        }
    }

    if (dirty) markMeshesSSBODirty(&context);
}

void physics_set_transform(Mesh* m) {
    if (!physics_system || m->physics_body_id == 0) return;

    JPH_BodyInterface* bi = JPH_PhysicsSystem_GetBodyInterface(physics_system);

    JPH_RVec3 jph_pos = {m->model[3][0], m->model[3][1], m->model[3][2]};

    vec3 euler;
    glm_euler_angles(m->model, euler);
    versor q;
    glm_euler_xyz_quat(euler, q);
    JPH_Quat jph_rot = {q[0], q[1], q[2], q[3]};

    // Teleport the rigid body to the new position
    JPH_BodyInterface_SetPositionAndRotation(bi, m->physics_body_id, &jph_pos, &jph_rot, JPH_Activation_Activate);

    // Kill momentum so it doesn't fight us while dragging
    JPH_Vec3 zero = {0.0f, 0.0f, 0.0f};
    JPH_BodyInterface_SetLinearVelocity(bi, m->physics_body_id, &zero);
    JPH_BodyInterface_SetAngularVelocity(bi, m->physics_body_id, &zero);
}
