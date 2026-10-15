#ifndef OBS_TEST_MONAD_EMBED
#define OBS_TEST_MONAD_EMBED
typedef struct monad_value monad_value_t;
void monad_value_retain(monad_value_t *);
void monad_value_release(monad_value_t *);
#endif
