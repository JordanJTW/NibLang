// Copyright (c) 2026, Jordan Werthman <jordanwerthman@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "src/map.h"

#include <string>
#include <string_view>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "src/types.h"
#include "src/vm.h"
#include "test/gtest_helpers.h"

// Automatically call `free_gc` at scope exit :^)
#define AUTOFREE_GC __attribute__((cleanup(free_gc)))

TEST(Map, Init) {
  AUTOFREE_GC vm_gc_t gc{0};
  RC_AUTOFREE vm_value_t map = allocate_map(&gc, 13);
  EXPECT_THAT(map, HasType(vm_value_t::VALUE_TYPE_MAP));
  EXPECT_EQ(map.as.map->bucket_count, 13);
}

TEST(Map, StoreAndGet_IntKey) {
  AUTOFREE_GC vm_gc_t gc{0};
  RC_AUTOFREE vm_value_t map = allocate_map(&gc, 13);
  ASSERT_THAT(map, HasType(vm_value_t::VALUE_TYPE_MAP));

  vm_value_t key = {.type = vm_value::VALUE_TYPE_INT, .as.i32 = 42};
  vm_value_t value = {.type = vm_value::VALUE_TYPE_INT, .as.i32 = 99};

  EXPECT_TRUE(map_insert(map.as.map, key, value));

  vm_value_t* retrieved = map_get(map.as.map, key);
  ASSERT_NE(retrieved, nullptr);
  EXPECT_EQ(retrieved->type, vm_value::VALUE_TYPE_INT);
  EXPECT_EQ(retrieved->as.i32, 99);
}

TEST(Map, StoreAndGet_StringKey) {
  AUTOFREE_GC vm_gc_t gc{0};
  RC_AUTOFREE vm_value_t map = allocate_map(&gc, 13);
  ASSERT_THAT(map, HasType(vm_value_t::VALUE_TYPE_MAP));

  // map_insert() takes ownership
  vm_value_t key = allocate_str_from_c("hello");
  vm_value_t value = allocate_str_from_c("world");
  EXPECT_TRUE(map_insert(map.as.map, key, value));

  vm_value_t* retrieved = map_get(map.as.map, key);
  ASSERT_NE(retrieved, nullptr);
  EXPECT_EQ(retrieved->type, vm_value::VALUE_TYPE_STR);
  char* str;
  size_t len = vm_as_str(retrieved, &str);
  EXPECT_EQ(std::string(str, len), "world");
}

TEST(Map, StoreAndGet_MapKey) {
  AUTOFREE_GC vm_gc_t gc{0};
  RC_AUTOFREE vm_value_t map = allocate_map(&gc, 13);
  ASSERT_THAT(map, HasType(vm_value_t::VALUE_TYPE_MAP));

  // map_insert() takes ownership
  vm_value_t key = allocate_map(&gc, 13);
  vm_value_t value = allocate_str_from_c("from map key");
  EXPECT_TRUE(map_insert(map.as.map, key, value));

  vm_value_t* retrieved = map_get(map.as.map, key);
  ASSERT_NE(retrieved, nullptr);
  EXPECT_EQ(retrieved->type, vm_value::VALUE_TYPE_STR);
  char* str;
  size_t len = vm_as_str(retrieved, &str);
  EXPECT_EQ(std::string(str, len), "from map key");
}

TEST(Map, StoreAndGet_FullBucket) {
  AUTOFREE_GC vm_gc_t gc{0};
  RC_AUTOFREE vm_value_t map = allocate_map(&gc, 1);
  ASSERT_THAT(map, HasType(vm_value_t::VALUE_TYPE_MAP));

  RC_AUTOFREE vm_value_t key_hello = allocate_str_from_c("hello");
  RC_AUTOFREE vm_value_t key_foo = allocate_str_from_c("foo");
  RC_AUTOFREE vm_value_t key_key = allocate_str_from_c("key");

  vm_adopt_ref(key_hello);
  vm_value_t value_world = allocate_str_from_c("world");
  EXPECT_TRUE(map_insert(map.as.map, key_hello, value_world));

  vm_adopt_ref(key_foo);
  vm_value_t value_bar = allocate_str_from_c("bar");
  EXPECT_TRUE(map_insert(map.as.map, key_foo, value_bar));

  vm_adopt_ref(key_key);
  vm_value_t value_value = allocate_str_from_c("value");
  EXPECT_TRUE(map_insert(map.as.map, key_key, value_value));

  {
    vm_value_t* retrieved = map_get(map.as.map, key_hello);
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->type, vm_value::VALUE_TYPE_STR);
    char* str;
    size_t len = vm_as_str(retrieved, &str);
    EXPECT_EQ(std::string(str, len), "world");
  }
  {
    vm_value_t* retrieved = map_get(map.as.map, key_key);
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->type, vm_value::VALUE_TYPE_STR);
    char* str;
    size_t len = vm_as_str(retrieved, &str);
    EXPECT_EQ(std::string(str, len), "value");
  }
  {
    vm_value_t* retrieved = map_get(map.as.map, key_foo);
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->type, vm_value::VALUE_TYPE_STR);
    char* str;
    size_t len = vm_as_str(retrieved, &str);
    EXPECT_EQ(std::string(str, len), "bar");
  }
}

TEST(Map, StoreAndGet_ReplaceValue) {
  AUTOFREE_GC vm_gc_t gc{0};
  RC_AUTOFREE vm_value_t map = allocate_map(&gc, 13);
  ASSERT_THAT(map, HasType(vm_value_t::VALUE_TYPE_MAP));

  RC_AUTOFREE vm_value_t key = allocate_str_from_c("hello");

  vm_adopt_ref(key);
  EXPECT_TRUE(map_insert(
      map.as.map, key,
      (vm_value_t){.type = vm_value::VALUE_TYPE_INT, .as.i32 = 123}));

  vm_adopt_ref(key);
  EXPECT_FALSE(map_insert(
      map.as.map, key,
      (vm_value_t){.type = vm_value::VALUE_TYPE_INT, .as.i32 = 369}));

  vm_value_t* retrieved = map_get(map.as.map, key);
  ASSERT_NE(retrieved, nullptr);
  EXPECT_EQ(retrieved->type, vm_value::VALUE_TYPE_INT);
  EXPECT_EQ(retrieved->as.i32, 369);
}
