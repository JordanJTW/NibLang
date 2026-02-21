#include <stdio.h>
#include <stdlib.h>

#include "src/map.h"
#include "src/types.h"
#include "src/vm.h"

void print_result(vm_value_t result, int indent) {
  printf("%*s", indent, "");

  switch (result.type) {
    case VALUE_TYPE_BOOL:
      printf("%s\n", result.as.boolean ? "true" : "false");
      break;
    case VALUE_TYPE_FLOAT:
      printf("%f\n", result.as.f32);
      break;
    case VALUE_TYPE_FUNCTION:
      printf("fn %s\n", result.as.fn->fn->name);
      break;
    case VALUE_TYPE_INT:
      printf("%d\n", result.as.i32);
      break;
    case VALUE_TYPE_STR:
      printf("%.*s\n", (int)result.as.str->len, result.as.str->c_str);
      break;
    case VALUE_TYPE_ARRAY:
      printf("[\n");
      for (size_t i = 0; i < result.as.array->len; ++i) {
        print_result(result.as.array->data[i], indent + 2);
      }
      printf("]\n");
      break;
    case VALUE_TYPE_MAP:
      DumpMap(result.as.map);
      break;
    case VALUE_TYPE_PROMISE:
      printf("(promise)\n");
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
    printf("Result: ");
    print_result(result, 0);
  }

  fclose(file);
  free(buffer);
}