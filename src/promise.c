#include "src/promise.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "src/vm.h"

typedef struct vm_promise_then_t {
  vm_value_t on_fulfilled_fn;
  vm_value_t on_rejected_fn;
  vm_value_t next_promise;
  struct vm_promise_then_t* next;
} vm_promise_then_t;

typedef struct vm_job_t {
  vm_value_t fn;
  vm_value_t value;
  vm_value_t next_promise;
  bool rejected;
  struct vm_job_t* next;
} vm_job_t;

typedef struct vm_job_queue_t {
  vm_job_t* job_queue_head;
  vm_job_t* job_queue_tail;
} vm_job_queue_t;

static bool is_promise(vm_value_t value) {
  return value.type == VALUE_TYPE_PROMISE;
}

static void free_promise(void* self) {
  Promise* promise = self;
  vm_free_ref(&promise->value);
  vm_promise_then_t* entry = promise->then_list;
  while (entry != NULL) {
    vm_free_ref(&entry->on_fulfilled_fn);
    vm_free_ref(&entry->on_rejected_fn);
    vm_free_ref(&entry->next_promise);
    vm_promise_then_t* next = entry->next;
    free(entry);
    entry = next;
  }
  free(promise);
}

vm_value_t allocate_promise() {
  Promise* promise = calloc(1, sizeof(Promise));
  promise->state = PROMISE_STATE_PENDING;
  promise->ref_count.deleter = &free_promise;
  return (vm_value_t){.type = VALUE_TYPE_PROMISE, .as.promise = promise};
}

static bool is_function(vm_value_t value) {
  return value.type == VALUE_TYPE_FUNCTION;
}

vm_job_queue_t* init_job_queue() {
  vm_job_queue_t* job_queue = calloc(1, sizeof(vm_job_queue_t));
  return job_queue;
}
void free_job_queue(vm_job_queue_t* job_queue) {
  assert(job_queue->job_queue_head == NULL && "job queue not empty on free");
  free(job_queue);
}

static void enqueue_promise_job(vm_job_queue_t* job_queue,
                                vm_value_t fn,
                                vm_value_t value,
                                vm_value_t next_promise,
                                bool rejected) {
  vm_job_t* job = calloc(1, sizeof(vm_job_t));
  job->fn = fn;
  vm_adopt_ref(job->fn);
  job->value = value;
  vm_adopt_ref(job->value);
  job->next_promise = next_promise;
  vm_adopt_ref(job->next_promise);
  job->rejected = rejected;
  job->next = NULL;

  if (job_queue->job_queue_tail) {
    job_queue->job_queue_tail->next = job;
    job_queue->job_queue_tail = job;
  } else {
    job_queue->job_queue_head = job_queue->job_queue_tail = job;
  }
}

vm_value_t promise_then(vm_job_queue_t* job_queue,
                        vm_value_t source_promise,
                        vm_value_t on_fulfilled_fn,
                        vm_value_t on_rejected_fn) {
  assert(is_promise(source_promise) && "promise_then called on non-promise");

  vm_value_t new_promise = allocate_promise();

  vm_promise_then_t* then = calloc(1, sizeof(vm_promise_then_t));
  then->on_fulfilled_fn = on_fulfilled_fn;
  vm_adopt_ref(then->on_fulfilled_fn);
  then->on_rejected_fn = on_rejected_fn;
  vm_adopt_ref(then->on_rejected_fn);
  then->next_promise = new_promise;
  vm_adopt_ref(then->next_promise);

  Promise* const source = source_promise.as.promise;
  then->next = source->then_list;
  source->then_list = then;

  // If source already settled, enqueue immediately
  if (source->state != PROMISE_STATE_PENDING) {
    vm_value_t fn = source->state == PROMISE_STATE_REJECTED ? on_rejected_fn
                                                            : on_fulfilled_fn;
    enqueue_promise_job(job_queue, fn, source->value, new_promise,
                        /*rejected=*/source->state == PROMISE_STATE_REJECTED);
  }

  return new_promise;
}

void promise_resolve(vm_job_queue_t* job_queue,
                     vm_value_t promise,
                     vm_value_t value,
                     bool rejected) {
  assert(is_promise(promise) && "promise_resolve called on non-promise");
  Promise* p = promise.as.promise;

  if (p->state != PROMISE_STATE_PENDING)
    return;

  p->state = rejected ? PROMISE_STATE_REJECTED : PROMISE_STATE_FULFILLED;
  p->value = value;

  for (vm_promise_then_t* t = p->then_list; t; t = t->next) {
    enqueue_promise_job(job_queue,
                        rejected ? t->on_rejected_fn : t->on_fulfilled_fn,
                        value, t->next_promise, rejected);
  }
}

static void promise_chain(vm_t* vm,
                          vm_job_queue_t* job_queue,
                          vm_value_t source_promise,
                          vm_value_t target_promise) {
  assert(is_promise(source_promise) && is_promise(target_promise) &&
         "promise_chain called on non-promise");
  if (source_promise.as.promise == target_promise.as.promise) {
    // TODO: Resolve with an Exception!
    return;
  }

  promise_then(
      job_queue, source_promise,
      bind_to_function(vm, 1 /*Promise.fulfill*/ | VM_BUILTIN_SELECT_BITMASK,
                       &target_promise, /*argc=*/1),
      bind_to_function(vm, 2 /*Promise.reject*/ | VM_BUILTIN_SELECT_BITMASK,
                       &target_promise, /*argc=*/1));
}

static void free_job(vm_job_t* job) {
  vm_free_ref(&job->fn);
  vm_free_ref(&job->value);
  vm_free_ref(&job->next_promise);
  free(job);
}

bool run_promise_jobs(vm_t* vm, vm_job_queue_t* job_queue) {
  bool had_jobs = job_queue->job_queue_head != NULL;
  while (job_queue->job_queue_head) {
    vm_job_t* job = job_queue->job_queue_head;
    job_queue->job_queue_head = job->next;
    if (!job_queue->job_queue_head)
      job_queue->job_queue_tail = NULL;

    vm_value_t result;
    bool is_exception = false;

    if (job->fn.type == VALUE_TYPE_FUNCTION) {
      result = vm_call_function(vm, job->fn.as.fn, &job->value, /*argc=*/1);
      if (vm_get_exception(vm, &result))
        is_exception = true;
    } else {
      // If no handler is found, forward the result (and state) of the last
      result = job->value;
      is_exception = job->rejected;
    }

    if (is_promise(result)) {
      promise_chain(vm, job_queue, result, job->next_promise);
    } else {
      promise_resolve(job_queue, job->next_promise, result, is_exception);
    }
    free_job(job);
  }
  return had_jobs;
}

vm_value_t vm_promise_fulfill(vm_value_t* argv, size_t argc, void* userdata) {
  assert(argc == 2 && is_promise(argv[0]) &&
         "Promise.fulfill must be invoked with $this and a value");

  RC_AUTOFREE vm_value_t this = argv[0];
  promise_resolve(vm_get_job_queue(userdata), this, argv[1] /*takes ownership*/,
                  /*rejected=*/false);
  return (vm_value_t){.type = VALUE_TYPE_VOID};
}
vm_value_t vm_promise_reject(vm_value_t* argv, size_t argc, void* userdata) {
  assert(argc == 2 && is_promise(argv[0]) &&
         "Promise.reject must be invoked with $this and a value");

  RC_AUTOFREE vm_value_t this = argv[0];
  promise_resolve(vm_get_job_queue(userdata), this, argv[1] /*takes ownership*/,
                  /*rejected=*/true);
  return (vm_value_t){.type = VALUE_TYPE_VOID};
}
vm_value_t vm_promise_then(vm_value_t* argv, size_t argc, void* userdata) {
  assert(argc == 3 && is_promise(argv[0]) &&
         "Promise.then must be invoked with $this and two functions");

  RC_AUTOFREE vm_value_t this = argv[0];
  vm_value_t new_promise =
      promise_then(vm_get_job_queue(userdata), this, argv[1], argv[2]);
  vm_free_ref(&argv[1]);  // Adopts a new ref in `promise_then`
  vm_free_ref(&argv[2]);  // Adopts a new ref in `promise_then`
  return new_promise;
}