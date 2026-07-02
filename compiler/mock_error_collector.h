#pragma once

#include <string_view>

#include "compiler/error_collector.h"
#include "compiler/tokenizer.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

class MockErrorCollector : public ErrorCollector {
 public:
  MOCK_METHOD(void, Add, (std::string_view message, Metadata meta), (override));

  // Allows delegating mocks to the actual implementation
  inline void DelegateToFake() {
    ON_CALL(*this, Add).WillByDefault([this](std::string_view m, Metadata mt) {
      this->ErrorCollector::Add(m, mt);
    });
  }
};