#include "mygames/room_directory.h"

#include <cflow/executor.h>
#include <salts/clock.h>

/* Assertions are the TinyTest-like test oracle here: keep them enabled even
 * when this installed-SDK qualification is built in Release. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct fixture {
    cflow_executor executor;
    cflow_executor_control control;
    mygames_room_directory rooms; /* accessed exclusively by CFlow worker */
    atomic_bool allow_first_finish;
    atomic_bool first_started;
    atomic_uint running;
    atomic_uint peak_running;
    atomic_uint completed;
    atomic_uint finalized;
    atomic_uint cancelled;
} fixture;

typedef struct submitted {
    fixture *owner;
    unsigned int index;
} submitted;

static void room_run(void *user) {
    submitted *task = (submitted *)user;
    fixture *f = task->owner;
    const unsigned int running = atomic_fetch_add(&f->running, 1u) + 1u;
    unsigned int peak = atomic_load(&f->peak_running);
    while (running > peak &&
           !atomic_compare_exchange_weak(&f->peak_running, &peak, running)) {}

    if (task->index == 1u) {
        atomic_store(&f->first_started, true);
        while (!atomic_load(&f->allow_first_finish)) cmeta_sleep_ms(1u);
    }

    /* A trivial authoritative-room mutation, only ever on one worker. */
    {
        const mygames_room *room = NULL;
        const mygames_room_status result =
            mygames_room_directory_create(&f->rooms, (mygames_player_id)task->index, &room);
        assert(result == MYGAMES_ROOM_OK);
        assert(room && room->players == 1u);
    }
    atomic_fetch_add(&f->completed, 1u);
    atomic_fetch_sub(&f->running, 1u);
}

static void room_cancel(void *user) {
    submitted *task = (submitted *)user;
    atomic_fetch_add(&task->owner->cancelled, 1u);
}

static void room_finalize(void *user) {
    submitted *task = (submitted *)user;
    atomic_fetch_add(&task->owner->finalized, 1u);
    free(task);
}

static cflow_admission_status post(fixture *f, unsigned int index) {
    submitted *task = (submitted *)calloc(1u, sizeof(*task));
    cflow_admission_status admitted;
    cflow_executor_task desc;
    assert(task != NULL);
    task->owner = f;
    task->index = index;
    desc = (cflow_executor_task){
        .run = room_run, .cancel = room_cancel,
        .finalize = room_finalize, .user = task};
    admitted = cflow_executor_try_post_task(&f->executor, &desc);
    if (admitted != CFLOW_ADMISSION_ACCEPTED) free(task);
    return admitted;
}

static void test_serial_bounded_admission_and_shutdown(void) {
    fixture *f = (fixture *)calloc(1u, sizeof(*f));
    unsigned int accepted = 0u;
    unsigned int i;
    uint64_t start;
    assert(f != NULL);
    mygames_room_directory_init(&f->rooms);
    atomic_init(&f->allow_first_finish, false);
    atomic_init(&f->first_started, false);
    atomic_init(&f->running, 0u);
    atomic_init(&f->peak_running, 0u);
    atomic_init(&f->completed, 0u);
    atomic_init(&f->finalized, 0u);
    atomic_init(&f->cancelled, 0u);

    assert(cflow_executor_serial_init_with_capacity(&f->executor, 2u));
    assert(cflow_executor_as_control(&f->executor, &f->control));

    assert(post(f, 1u) == CFLOW_ADMISSION_ACCEPTED);
    ++accepted;
    start = cmeta_monotonic_ms();
    while (!atomic_load(&f->first_started) &&
           cmeta_monotonic_ms() - start < 3000u) cmeta_sleep_ms(1u);
    assert(atomic_load(&f->first_started));
    /* Proof of independent worker progress: test thread has not driven any
       executor run() method but the task is already running. */
    assert(atomic_load(&f->running) == 1u);

    /* Fill the queue while its sole worker is blocked. An overflow must
       return FULL instead of waiting in the CHttp/transport callback. */
    for (i = 2u; i <= 15u; ++i) {
        const cflow_admission_status st = post(f, i);
        if (st == CFLOW_ADMISSION_ACCEPTED) ++accepted;
        else {
            assert(st == CFLOW_ADMISSION_FULL);
            break;
        }
    }
    assert(i <= 15u);
    assert(accepted <= 3u);
    assert(atomic_load(&f->completed) == 0u);

    atomic_store(&f->allow_first_finish, true);
    assert(cflow_executor_control_shutdown(
        &f->control, CFLOW_EXECUTOR_SHUTDOWN_DRAIN));
    assert(cflow_executor_control_wait_idle(&f->control) == CFLOW_EXECUTOR_WAIT_IDLE);
    assert(post(f, 30u) == CFLOW_ADMISSION_CLOSED);
    assert(atomic_load(&f->running) == 0u);
    assert(atomic_load(&f->completed) == accepted);
    assert(atomic_load(&f->finalized) == accepted);
    assert(atomic_load(&f->cancelled) == 0u);
    assert(atomic_load(&f->peak_running) == 1u);
    cflow_executor_destroy(&f->executor);
    free(f);
}

static void test_cancel_pending_terminal_ownership(void) {
    fixture *f = (fixture *)calloc(1u, sizeof(*f));
    uint64_t started;
    assert(f != NULL);
    mygames_room_directory_init(&f->rooms);
    atomic_init(&f->allow_first_finish, false);
    atomic_init(&f->first_started, false);
    atomic_init(&f->running, 0u);
    atomic_init(&f->peak_running, 0u);
    atomic_init(&f->completed, 0u);
    atomic_init(&f->finalized, 0u);
    atomic_init(&f->cancelled, 0u);
    assert(cflow_executor_serial_init_with_capacity(&f->executor, 4u));
    assert(cflow_executor_as_control(&f->executor, &f->control));
    assert(post(f, 1u) == CFLOW_ADMISSION_ACCEPTED);
    started = cmeta_monotonic_ms();
    while (!atomic_load(&f->first_started) &&
           cmeta_monotonic_ms() - started < 3000u) cmeta_sleep_ms(1u);
    assert(atomic_load(&f->first_started));
    assert(post(f, 2u) == CFLOW_ADMISSION_ACCEPTED);
    assert(cflow_executor_control_shutdown(
        &f->control, CFLOW_EXECUTOR_SHUTDOWN_CANCEL_PENDING));
    atomic_store(&f->allow_first_finish, true);
    assert(cflow_executor_control_wait_idle(&f->control) == CFLOW_EXECUTOR_WAIT_IDLE);
    assert(atomic_load(&f->completed) == 1u);
    assert(atomic_load(&f->cancelled) == 1u);
    assert(atomic_load(&f->finalized) == 2u);
    assert(atomic_load(&f->running) == 0u);
    cflow_executor_destroy(&f->executor);
    free(f);
}

int main(void) {
    test_serial_bounded_admission_and_shutdown();
    test_cancel_pending_terminal_ownership();
    puts("mygames.cflow_owner: PASS");
    return 0;
}
