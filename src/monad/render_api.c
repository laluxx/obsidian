/// render_api.c — Graphics policy as a capability, never raw Vulkan handles
/// Implementation
#include "render_api.h"
#include "effect_policy.h"
#include "foreign.h"
#include "vulkan_setup.h"
#define RENDER_REF "obsidian.render-policy"
/* Stable opaque identity: no VkDevice, image, pipeline or transient command
 * buffer is ever passed to Monad as a foreign reference. */
static unsigned char render_identity;
extern bool cullingFrozen;
extern bool iblLightingEnabled;
static monad_status_t policy(monad_object_space_t *sp, size_t argc,
                             monad_value_t *const *args, monad_value_t **out,
                             void *ctx, monad_error_t *e) {
  (void)args;
  ObsMonadRuntime *host = ctx;
  if (argc != 0 || !out || !obs_host_owner(host))
    return obs_host_invalid(e, "render policy requires the engine thread");
  if (!obs_host_borrow(sp, host, RENDER_REF, &render_identity, host->generation,
                       out, e))
    return e ? e->status : MONAD_ERROR_INVALID_ARGUMENT;
  return MONAD_OK;
}
static bool valid(monad_value_t *v, ObsMonadRuntime *host, monad_error_t *e) {
  return obs_host_require(v, host, RENDER_REF, &render_identity,
                          host->generation, e);
}
static monad_status_t ao(monad_object_space_t *sp, size_t argc,
                         monad_value_t *const *args, monad_value_t **out,
                         void *ctx, monad_error_t *e) {
  if (argc != 1 || !args || !out || !valid(args[0], ctx, e))
    return obs_host_invalid(e, "expected live render policy");
  *out = monad_value_bool(sp, ambientOcclusionEnabled);
  return MONAD_OK;
}
static monad_status_t ibl(monad_object_space_t *sp, size_t argc,
                          monad_value_t *const *args, monad_value_t **out,
                          void *ctx, monad_error_t *e) {
  if (argc != 1 || !args || !out || !valid(args[0], ctx, e))
    return obs_host_invalid(e, "expected live render policy");
  *out = monad_value_bool(sp, iblLightingEnabled);
  return MONAD_OK;
}
static monad_status_t culling(monad_object_space_t *sp, size_t argc,
                              monad_value_t *const *args, monad_value_t **out,
                              void *ctx, monad_error_t *e) {
  if (argc != 1 || !args || !out || !valid(args[0], ctx, e))
    return obs_host_invalid(e, "expected live render policy");
  *out = monad_value_bool(sp, cullingFrozen);
  return MONAD_OK;
}
static monad_status_t antialiasing(monad_object_space_t *sp, size_t argc,
                                   monad_value_t *const *args,
                                   monad_value_t **out, void *ctx,
                                   monad_error_t *e) {
  ObsMonadRuntime *host = ctx;
  if (argc != 1 || !args || !out || !valid(args[0], host, e))
    return obs_host_invalid(e, "expected a live render policy");
  if (context.rasterizationSamples == VK_SAMPLE_COUNT_1_BIT) {
    *out = monad_value_nil(sp);
    return MONAD_OK;
  }
  return monad_value_make_i64(sp, context.rasterizationSamples, out, e);
}
static monad_status_t set_antialiasing(monad_object_space_t *sp, size_t argc,
                                       monad_value_t *const *args,
                                       monad_value_t **out, void *ctx,
                                       monad_error_t *e) {
  if (!obs_monad_effect_allowed(OBS_EFFECT_WRITE | OBS_EFFECT_GPU,
                                OBS_LIFETIME_ENGINE, false))
    return obs_host_invalid(
        e, "renderer mutation is forbidden during Monad publication");
  if (argc != 2 || !args || !out || !valid(args[0], ctx, e))
    return obs_host_invalid(
        e, "expected a live render policy and nil or sample count");

  int64_t requested = 0;
  if (monad_value_kind(args[1]) != MONAD_VALUE_NIL &&
      monad_value_as_i64(args[1], &requested, e) != MONAD_OK)
    return obs_host_invalid(e, "antialiasing must be nil, 2, 4, or 8");
  if (!obs_vk_set_antialiasing_request(&context, requested))
    return obs_host_invalid(e, "antialiasing sample count is invalid, "
                               "unsupported, or renderer is unavailable");
  *out = monad_value_nil(sp);
  return MONAD_OK;
}
static monad_status_t set_policy_bool(monad_object_space_t *sp, size_t argc,
                                      monad_value_t *const *args,
                                      monad_value_t **out, void *ctx,
                                      monad_error_t *e, bool *setting) {
  if (!obs_monad_effect_allowed(OBS_EFFECT_WRITE | OBS_EFFECT_GPU,
                                OBS_LIFETIME_ENGINE, false))
    return obs_host_invalid(
        e, "renderer mutation is forbidden during Monad publication");
  if (argc != 2 || !args || !out || !setting || !valid(args[0], ctx, e) ||
      monad_value_kind(args[1]) != MONAD_VALUE_BOOL)
    return obs_host_invalid(e,
                            "expected a live render policy and Boolean value");
  *setting = monad_value_is_true(args[1]);
  *out = monad_value_nil(sp);
  return MONAD_OK;
}
static monad_status_t set_ao(monad_object_space_t *sp, size_t argc,
                             monad_value_t *const *args, monad_value_t **out,
                             void *ctx, monad_error_t *e) {
  return set_policy_bool(sp, argc, args, out, ctx, e, &ambientOcclusionEnabled);
}
static monad_status_t set_ibl(monad_object_space_t *sp, size_t argc,
                              monad_value_t *const *args, monad_value_t **out,
                              void *ctx, monad_error_t *e) {
  return set_policy_bool(sp, argc, args, out, ctx, e, &iblLightingEnabled);
}
static monad_status_t set_culling(monad_object_space_t *sp, size_t argc,
                                  monad_value_t *const *args,
                                  monad_value_t **out, void *ctx,
                                  monad_error_t *e) {
  return set_policy_bool(sp, argc, args, out, ctx, e, &cullingFrozen);
}
bool obs_monad_register_render_api(ObsMonadRuntime *host,
                                   monad_error_t *error) {
  static const ObsHostBinding entries[] = {
      {"obs-render-policy",
       "Borrow the long-lived high-level renderer controls.", 0, policy, NULL},
      {"render", "Borrow the long-lived high-level renderer controls.", 0,
       policy, NULL},
      {"obs-render-ao?", "Whether ambient occlusion is requested.", 1, ao,
       NULL},
      {"render-ao?", "Whether ambient occlusion is requested.", 1, ao, NULL},
      {"obs-render-ibl?", "Whether IBL is requested (not necessarily loaded).",
       1, ibl, NULL},
      {"render-ibl?", "Whether IBL is requested (not necessarily loaded).", 1,
       ibl, NULL},
      {"obs-render-culling-frozen?", "Whether frustum culling is frozen.", 1,
       culling, NULL},
      {"render-culling-frozen?", "Whether frustum culling is frozen.", 1,
       culling, NULL},
      {"obs-render-antialiasing",
       "Current antialiasing sample count; nil means off.", 1, antialiasing,
       NULL},
      {"render-antialiasing",
       "Current antialiasing sample count; nil means off.", 1, antialiasing,
       NULL},
      {"obs-render-set-ao",
       "Set ambient occlusion request for subsequent frames.", 2, set_ao, NULL},
      {"render-ao!", "Set ambient occlusion request for subsequent frames.", 2,
       set_ao, NULL},
      {"obs-render-set-ibl", "Set IBL request for subsequent frames.", 2,
       set_ibl, NULL},
      {"render-ibl!", "Set IBL request for subsequent frames.", 2, set_ibl,
       NULL},
      {"obs-render-set-culling-frozen",
       "Set whether frustum culling is frozen.", 2, set_culling, NULL},
      {"render-culling-frozen!", "Set whether frustum culling is frozen.", 2,
       set_culling, NULL},
      {"obs-render-set-antialiasing",
       "Set exact antialiasing sample count: nil, 2, 4, or 8.", 2,
       set_antialiasing, NULL},
      {"render-antialiasing!",
       "Set exact antialiasing sample count: nil, 2, 4, or 8.", 2,
       set_antialiasing, NULL},
  };
  return obs_host_register(host, entries, sizeof(entries) / sizeof(entries[0]),
                           error);
}
