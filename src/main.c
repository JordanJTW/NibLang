#include <stdio.h>
#include <stdlib.h>

#include "src/map.h"
#include "src/promise.h"
#include "src/types.h"
#include "src/vm.h"

void print_result(vm_value_t result, int indent) {
  switch (result.type) {
    case VALUE_TYPE_BOOL:
      printf("%s", result.as.boolean ? "true" : "false");
      break;
    case VALUE_TYPE_FLOAT:
      printf("%f", result.as.f32);
      break;
    case VALUE_TYPE_FUNCTION:
      printf("fn %s", result.as.fn->fn->name);
      break;
    case VALUE_TYPE_INT:
      printf("%d", result.as.i32);
      break;
    case VALUE_TYPE_STR:
      printf("\"%s\"", result.as.str->c_str);
      break;
    case VALUE_TYPE_ARRAY:
      printf("[ ");
      for (size_t i = 0; i < result.as.array->len; ++i) {
        print_result(result.as.array->data[i], 0);
        printf(", ");
      }
      printf("]");
      break;
    case VALUE_TYPE_MAP:
      printf("{\n");
      for (size_t i = 0; i < result.as.map->bucket_count; ++i) {
        for (MapNode* node = result.as.map->buckets[i]; node;
             node = node->next) {
          printf("%*skey: ", indent + 2, "");
          print_result(node->key, indent + 2);

          printf(" value: ");
          print_result(node->value, indent + 2);
          printf("\n");
        }
      }
      printf("%*s}", indent, "");
      break;
    case VALUE_TYPE_PROMISE:
      printf("Promise {\n%*sstate: %d\n%*sresult: ", indent + 2, "",
             result.as.promise->state, indent + 2, "");
      print_result(result.as.promise->value, indent + 2);
      printf("\n%*s}", indent, "");
      break;
    case VALUE_TYPE_NULL:
      printf("(null)\n");
      break;
  }
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    fprintf(stderr, "%s <path>\n", argv[0]);
    return 1;
  }

  FILE* file = fopen(argv[1], "rb");
  if (!file) {
    fprintf(stderr, "Error opening: %s\n", argv[1]);
    return 1;
  }

  fseek(file, 0, SEEK_END);
  long file_size = ftell(file);
  rewind(file);

  uint8_t* buffer = (uint8_t*)malloc(file_size);
  if (!buffer) {
    perror("Memory allocation failed");
    fclose(file);
    return 1;
  }

  size_t bytes_read = fread(buffer, 1, file_size, file);
  if (bytes_read != file_size) {
    perror("Error reading file");
    fclose(file);
    return 1;
  }

  vm_t* vm = init_vm(buffer, file_size);
  if (vm != NULL) {
    vm_value_t result = vm_run(vm, 0, true);
    run_promise_jobs(vm, vm_get_job_queue(vm));
    printf("Result: ");
    print_result(result, 0);
    printf("\n");
  }

  fclose(file);
  free(buffer);
}