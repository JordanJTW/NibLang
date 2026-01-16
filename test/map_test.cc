#include "src/map.h"

#include <cstdint>
#include <string>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "src/types.h"
#include "src/vm.h"

TEST(Map, Init) {
  Map* map = init_map(13);
  EXPECT_NE(map, nullptr);
  EXPECT_EQ(map->bucket_count, 13);
  free_map(map);
}

TEST(Map, StoreAndGet_IntKey) {
  Map* map = init_map(13);

  vm_value_t key = {.type = vm_value::VALUE_TYPE_INT, .as.i32 = 42};
  vm_value_t value = {.type = vm_value::VALUE_TYPE_INT, .as.i32 = 99};

  EXPECT_TRUE(map_insert(map, key, value));

  vm_value_t* retrieved = map_get(map, key);
  ASSERT_NE(retrieved, nullptr);
  EXPECT_EQ(retrieved->type, vm_value::VALUE_TYPE_INT);
  EXPECT_EQ(retrieved->as.i32, 99);

  free_map(map);
}

TEST(Map, StoreAndGet_StringKey) {
  Map* map = init_map(13);

  RC_AUTOFREE vm_value_t key = allocate_str_from_c("hello");
  vm_adopt_ref(key);
  RC_AUTOFREE vm_value_t value = allocate_str_from_c("world");
  vm_adopt_ref(value);

  EXPECT_TRUE(map_insert(map, key, value));

  vm_value_t* retrieved = map_get(map, key);
  ASSERT_NE(retrieved, nullptr);
  EXPECT_EQ(retrieved->type, vm_value::VALUE_TYPE_STR);
  char* str;
  size_t len = vm_as_str(retrieved, &str);
  EXPECT_EQ(std::string(str, len), "world");

  free_map(map);
}

TEST(Map, StoreAndGet_MapKey) {
  Map* map = init_map(13);

  RC_AUTOFREE vm_value_t key = allocate_map();
  vm_adopt_ref(key);
  RC_AUTOFREE vm_value_t value = allocate_str_from_c("from map key");
  vm_adopt_ref(value);

  EXPECT_TRUE(map_insert(map, key, value));

  vm_value_t* retrieved = map_get(map, key);
  ASSERT_NE(retrieved, nullptr);
  EXPECT_EQ(retrieved->type, vm_value::VALUE_TYPE_STR);
  char* str;
  size_t len = vm_as_str(retrieved, &str);
  EXPECT_EQ(std::string(str, len), "from map key");

  free_map(map);
}

TEST(Map, StoreAndGet_FullBucket) {
  Map* map = init_map(1);

  RC_AUTOFREE vm_value_t key_hello = allocate_str_from_c("hello");
  vm_adopt_ref(key_hello);
  RC_AUTOFREE vm_value_t key_foo = allocate_str_from_c("foo");
  vm_adopt_ref(key_foo);
  RC_AUTOFREE vm_value_t key_key = allocate_str_from_c("key");
  vm_adopt_ref(key_key);

  RC_AUTOFREE vm_value_t value_world = allocate_str_from_c("world");
  vm_adopt_ref(value_world);
  RC_AUTOFREE vm_value_t value_bar = allocate_str_from_c("bar");
  vm_adopt_ref(value_bar);
  RC_AUTOFREE vm_value_t value_value = allocate_str_from_c("value");
  vm_adopt_ref(value_value);

  EXPECT_TRUE(map_insert(map, key_hello, value_world));
  EXPECT_TRUE(map_insert(map, key_foo, value_bar));
  EXPECT_TRUE(map_insert(map, key_key, value_value));

  {
    vm_value_t* retrieved = map_get(map, key_hello);
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->type, vm_value::VALUE_TYPE_STR);
    char* str;
    size_t len = vm_as_str(retrieved, &str);
    EXPECT_EQ(std::string(str, len), "world");
  }
  {
    vm_value_t* retrieved = map_get(map, key_key);
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->type, vm_value::VALUE_TYPE_STR);
    char* str;
    size_t len = vm_as_str(retrieved, &str);
    EXPECT_EQ(std::string(str, len), "value");
  }
  {
    vm_value_t* retrieved = map_get(map, key_foo);
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->type, vm_value::VALUE_TYPE_STR);
    char* str;
    size_t len = vm_as_str(retrieved, &str);
    EXPECT_EQ(std::string(str, len), "bar");
  }
  free_map(map);
}

TEST(Map, StoreAndGet_ReplaceValue) {
  Map* map = init_map(13);

  RC_AUTOFREE vm_value_t key = allocate_str_from_c("hello");
  vm_adopt_ref(key);

  EXPECT_TRUE(map_insert(
      map, key, (vm_value_t){.type = vm_value::VALUE_TYPE_INT, .as.i32 = 123}));
  EXPECT_FALSE(map_insert(
      map, key, (vm_value_t){.type = vm_value::VALUE_TYPE_INT, .as.i32 = 369}));

  vm_value_t* retrieved = map_get(map, key);
  ASSERT_NE(retrieved, nullptr);
  EXPECT_EQ(retrieved->type, vm_value::VALUE_TYPE_INT);
  EXPECT_EQ(retrieved->as.i32, 369);

  free_map(map);
}
