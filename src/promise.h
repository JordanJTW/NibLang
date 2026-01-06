#pragma once

#include <stdbool.h>

#include "src/types.h"
#include "src/vm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vm_job_queue_t vm_job_queue_t;

vm_job_queue_t* init_job_queue();
void free_job_queue(vm_job_queue_t* job_queue);

vm_value_t allocate_promise();
void free_promise(Promise*);

// Resolves the state of a PENDING promise with `value` (to be executed on the
// next call to `run_promise_jobs()`). If `rejected` is true, then `value` is
// expected to be an exception.
void promise_resolve(vm_job_queue_t* job_queue,
                     vm_value_t promise,
                     vm_value_t value,
                     bool rejected);

// Queues `on_fulfilled_fn()` or `on_rejected_fn()` (callbacks) to be called
// when resolving `source_promise` (to be executed on the next call to
// `run_promise_jobs()`). Returns a new Promise which will resolve to the output
// of the callbacks. If the callbacks return a Promise then it is linked to the
// Promise returned (i.e. if one Promise resolves the other resolves to the
// same value).
vm_value_t promise_then(vm_job_queue_t* job_queue,
                        vm_value_t source_promise,
                        vm_value_t on_fulfilled_fn,
                        vm_value_t on_rejected_fn);

// Runs any pending callbacks queued by `promise_resolve()`. This should be
// called at a fairly regular cadence. If any jobs were executed returns true.
bool run_promise_jobs(vm_t* vm, vm_job_queue_t* job_queue);


#ifdef __cplusplus
}  // extern "C"
#endif