/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "velox/exec/tests/utils/OperatorTestBase.h"
#include "velox/common/caching/AsyncDataCache.h"
#include "velox/common/file/FileSystems.h"
#include "velox/common/memory/MallocAllocator.h"
#include "velox/common/testutil/TestValue.h"
#include "velox/dwio/common/FileSink.h"
#include "velox/exec/Exchange.h"
#include "velox/exec/PartitionedOutputBufferManager.h"
#include "velox/exec/tests/utils/AssertQueryBuilder.h"
#include "velox/functions/prestosql/aggregates/RegisterAggregateFunctions.h"
#include "velox/functions/prestosql/registration/RegistrationFunctions.h"
#include "velox/parse/Expressions.h"
#include "velox/parse/ExpressionsParser.h"
#include "velox/parse/TypeResolver.h"
#include "velox/serializers/PrestoSerializer.h"

using namespace facebook::velox::common::testutil;

DECLARE_bool(velox_enable_memory_usage_track_in_default_memory_pool);

namespace facebook::velox::exec::test {

OperatorTestBase::OperatorTestBase() {
  parse::registerTypeResolver();
}

void OperatorTestBase::registerVectorSerde() {
  velox::serializer::presto::PrestoVectorSerde::registerVectorSerde();
}

OperatorTestBase::~OperatorTestBase() {
  // Wait for all the tasks to be deleted.
  exec::test::waitForAllTasksToBeDeleted();
  // Revert to default process-wide MemoryAllocator.
  memory::MemoryAllocator::setDefaultInstance(nullptr);
}

void OperatorTestBase::SetUpTestCase() {
  FLAGS_velox_enable_memory_usage_track_in_default_memory_pool = true;
  memory::MemoryArbitrator::registerAllFactories();
  functions::prestosql::registerAllScalarFunctions();
  aggregate::prestosql::registerAllAggregateFunctions();
  TestValue::enable();
}

void OperatorTestBase::TearDownTestCase() {
  waitForAllTasksToBeDeleted();
  memory::MemoryArbitrator::unregisterAllFactories();
}

void OperatorTestBase::SetUp() {
  if (!isRegisteredVectorSerde()) {
    this->registerVectorSerde();
  }
  driverExecutor_ = std::make_unique<folly::CPUThreadPoolExecutor>(3);
  ioExecutor_ = std::make_unique<folly::IOThreadPoolExecutor>(3);
  allocator_ = std::make_shared<memory::MallocAllocator>(8L << 30);
  if (!asyncDataCache_) {
    asyncDataCache_ = cache::AsyncDataCache::create(allocator_.get());
    cache::AsyncDataCache::setInstance(asyncDataCache_.get());
  }
  memory::MemoryAllocator::setDefaultInstance(allocator_.get());
}

void OperatorTestBase::TearDown() {
  if (asyncDataCache_ != nullptr) {
    asyncDataCache_->shutdown();
  }
}

std::shared_ptr<Task> OperatorTestBase::assertQuery(
    const core::PlanNodePtr& plan,
    const std::vector<std::shared_ptr<connector::ConnectorSplit>>&
        connectorSplits,
    const std::string& duckDbSql,
    std::optional<std::vector<uint32_t>> sortingKeys) {
  std::vector<exec::Split> splits;
  splits.reserve(connectorSplits.size());
  for (const auto& connectorSplit : connectorSplits) {
    splits.emplace_back(exec::Split(folly::copy(connectorSplit), -1));
  }

  return assertQuery(plan, std::move(splits), duckDbSql, sortingKeys);
}

namespace {
/// Returns the plan node ID of the only leaf plan node. Throws if 'root' has
/// multiple leaf nodes.
core::PlanNodeId getOnlyLeafPlanNodeId(const core::PlanNodePtr& root) {
  const auto& sources = root->sources();
  if (sources.empty()) {
    return root->id();
  }

  VELOX_CHECK_EQ(1, sources.size());
  return getOnlyLeafPlanNodeId(sources[0]);
}

std::function<void(Task* task)> makeAddSplit(
    bool& noMoreSplits,
    std::unordered_map<core::PlanNodeId, std::vector<exec::Split>>&& splits) {
  return [&](Task* task) {
    if (noMoreSplits) {
      return;
    }
    for (auto& [nodeId, nodeSplits] : splits) {
      for (auto& split : nodeSplits) {
        task->addSplit(nodeId, std::move(split));
      }
      task->noMoreSplits(nodeId);
    }
    noMoreSplits = true;
  };
}
} // namespace

std::shared_ptr<Task> OperatorTestBase::assertQuery(
    const core::PlanNodePtr& plan,
    std::vector<exec::Split>&& splits,
    const std::string& duckDbSql,
    std::optional<std::vector<uint32_t>> sortingKeys) {
  const auto splitNodeId = getOnlyLeafPlanNodeId(plan);
  return assertQuery(
      plan, {{splitNodeId, std::move(splits)}}, duckDbSql, sortingKeys);
}

std::shared_ptr<Task> OperatorTestBase::assertQuery(
    const core::PlanNodePtr& plan,
    std::unordered_map<core::PlanNodeId, std::vector<exec::Split>>&& splits,
    const std::string& duckDbSql,
    std::optional<std::vector<uint32_t>> sortingKeys) {
  bool noMoreSplits = false;
  return test::assertQuery(
      plan,
      makeAddSplit(noMoreSplits, std::move(splits)),
      duckDbSql,
      duckDbQueryRunner_,
      sortingKeys);
}

// static
std::shared_ptr<core::FieldAccessTypedExpr> OperatorTestBase::toFieldExpr(
    const std::string& name,
    const RowTypePtr& rowType) {
  return std::make_shared<core::FieldAccessTypedExpr>(
      rowType->findChild(name), name);
}

core::TypedExprPtr OperatorTestBase::parseExpr(
    const std::string& text,
    RowTypePtr rowType,
    const parse::ParseOptions& options) {
  auto untyped = parse::parseExpr(text, options);
  return core::Expressions::inferTypes(untyped, rowType, pool_.get());
}

/*static*/ void OperatorTestBase::deleteTaskAndCheckSpillDirectory(
    std::shared_ptr<Task>& task) {
  const auto spillDirectoryStr = task->spillDirectory();
  // Nothing to do if there is no spilling directory was set.
  if (spillDirectoryStr.empty()) {
    return;
  }

  // Wait for the task to go.
  task.reset();
  waitForAllTasksToBeDeleted();

  // If a spilling directory was set, ensure it was removed after the task is
  // gone.
  auto fs = filesystems::getFileSystem(spillDirectoryStr, nullptr);
  EXPECT_FALSE(fs->exists(spillDirectoryStr));
}

} // namespace facebook::velox::exec::test
