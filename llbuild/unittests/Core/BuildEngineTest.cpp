//===- unittests/Core/BuildEngineTest.cpp ---------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "llbuild/Core/BuildEngine.h"

#include "llbuild/Core/BuildDB.h"

#include "gtest/gtest.h"

#include <unordered_map>
#include <vector>

using namespace llbuild;
using namespace llbuild::core;

namespace {

class SimpleBuildEngineDelegate : public core::BuildEngineDelegate {
  virtual core::Rule lookupRule(const core::KeyType& key) override {
    // We never expect dynamic rule lookup.
    fprintf(stderr, "error: unexpected rule lookup for \"%s\"\n",
            key.c_str());
    abort();
    return core::Rule();
  }

  virtual void cycleDetected(const std::vector<core::Rule*>& items) override {
    // We never expect a cycle.
    fprintf(stderr, "error: cycle\n");
    abort();
  }
};

static int32_t intFromValue(const core::ValueType& value) {
  assert(value.size() == 4);
  return ((value[0] << 0) |
          (value[1] << 8) |
          (value[2] << 16) |
          (value[3] << 24));
}
static core::ValueType intToValue(int32_t value) {
  std::vector<uint8_t> result(4);
  result[0] = (value >> 0) & 0xFF;
  result[1] = (value >> 8) & 0xFF;
  result[2] = (value >> 16) & 0xFF;
  result[3] = (value >> 24) & 0xFF;
  return result;
}

// Simple task implementation which takes a fixed set of dependencies, evaluates
// them all, and then provides the output.
class SimpleTask : public Task {
public:
  typedef std::function<int(const std::vector<int>&)> ComputeFnType;

private:
  std::vector<KeyType> inputs;
  std::vector<int> inputValues;
  ComputeFnType compute;

public:
  SimpleTask(const std::vector<KeyType>& inputs, ComputeFnType compute)
      : inputs(inputs), compute(compute)
  {
    inputValues.resize(inputs.size());
  }

  virtual void start(BuildEngine& engine) override {
    // Request all of the inputs.
    for (int i = 0, e = inputs.size(); i != e; ++i) {
      engine.taskNeedsInput(this, inputs[i], i);
    }
  }

  virtual void provideValue(BuildEngine&, uintptr_t inputID,
                            const ValueType& value) override {
    // Update the input values.
    assert(inputID < inputValues.size());
    inputValues[inputID] = intFromValue(value);
  }

  virtual void inputsAvailable(core::BuildEngine& engine) override {
    engine.taskIsComplete(this, intToValue(compute(inputValues)));
  }
};

// Helper function for creating a simple action.
typedef std::function<Task*(BuildEngine&)> ActionFn;

static ActionFn simpleAction(const std::vector<KeyType>& inputs,
                             SimpleTask::ComputeFnType compute) {
  return [=] (BuildEngine& engine) {
    return engine.registerTask(new SimpleTask(inputs, compute)); };
}

TEST(BuildEngineTest, basic) {
  // Check a trivial build graph.
  std::vector<std::string> builtKeys;
  SimpleBuildEngineDelegate delegate;
  core::BuildEngine engine(delegate);
  engine.addRule({
      "value-A", simpleAction({}, [&] (const std::vector<int>& inputs) {
          builtKeys.push_back("value-A");
          return 2; }) });
  engine.addRule({
      "value-B", simpleAction({}, [&] (const std::vector<int>& inputs) {
          builtKeys.push_back("value-B");
          return 3; }) });
  engine.addRule({
      "result",
      simpleAction({"value-A", "value-B"},
                   [&] (const std::vector<int>& inputs) {
                     EXPECT_EQ(2U, inputs.size());
                     EXPECT_EQ(2, inputs[0]);
                     EXPECT_EQ(3, inputs[1]);
                     builtKeys.push_back("result");
                     return inputs[0] * inputs[1] * 5;
                   }) });

  // Build the result.
  EXPECT_EQ(2 * 3 * 5, intFromValue(engine.build("result")));
  EXPECT_EQ(3U, builtKeys.size());
  EXPECT_EQ("value-A", builtKeys[0]);
  EXPECT_EQ("value-B", builtKeys[1]);
  EXPECT_EQ("result", builtKeys[2]);

  // Check that we can get results for already built nodes, without building
  // anything.
  builtKeys.clear();
  EXPECT_EQ(2, intFromValue(engine.build("value-A")));
  EXPECT_TRUE(builtKeys.empty());
  builtKeys.clear();
  EXPECT_EQ(3, intFromValue(engine.build("value-B")));
  EXPECT_TRUE(builtKeys.empty());
}

TEST(BuildEngineTest, basicWithSharedInput) {
  // Check a build graph with a input key shared by multiple rules.
  //
  // Dependencies:
  //   value-C: (value-A, value-B)
  //   value-R: (value-A, value-C)
  std::vector<std::string> builtKeys;
  SimpleBuildEngineDelegate delegate;
  core::BuildEngine engine(delegate);
  engine.addRule({
      "value-A", simpleAction({}, [&] (const std::vector<int>& inputs) {
          builtKeys.push_back("value-A");
          return 2; }) });
  engine.addRule({
      "value-B", simpleAction({}, [&] (const std::vector<int>& inputs) {
          builtKeys.push_back("value-B");
          return 3; }) });
  engine.addRule({
      "value-C",
      simpleAction({"value-A", "value-B"},
                   [&] (const std::vector<int>& inputs) {
                     EXPECT_EQ(2U, inputs.size());
                     EXPECT_EQ(2, inputs[0]);
                     EXPECT_EQ(3, inputs[1]);
                     builtKeys.push_back("value-C");
                     return inputs[0] * inputs[1] * 5;
                   }) });
  engine.addRule({
      "value-R",
      simpleAction({"value-A", "value-C"},
                   [&] (const std::vector<int>& inputs) {
                     EXPECT_EQ(2U, inputs.size());
                     EXPECT_EQ(2, inputs[0]);
                     EXPECT_EQ(2 * 3 * 5, inputs[1]);
                     builtKeys.push_back("value-R");
                     return inputs[0] * inputs[1] * 7;
                   }) });

  // Build the result.
  EXPECT_EQ(2 * 2 * 3 * 5 * 7, intFromValue(engine.build("value-R")));
  EXPECT_EQ(4U, builtKeys.size());
  EXPECT_EQ("value-A", builtKeys[0]);
  EXPECT_EQ("value-B", builtKeys[1]);
  EXPECT_EQ("value-C", builtKeys[2]);
  EXPECT_EQ("value-R", builtKeys[3]);
}

TEST(BuildEngineTest, veryBasicIncremental) {
  // Check a trivial build graph responds to incremental changes appropriately.
  //
  // Dependencies:
  //   value-R: (value-A, value-B)
  std::vector<std::string> builtKeys;
  SimpleBuildEngineDelegate delegate;
  core::BuildEngine engine(delegate);
  int valueA = 2;
  int valueB = 3;
  engine.addRule({
      "value-A", simpleAction({}, [&] (const std::vector<int>& inputs) {
          builtKeys.push_back("value-A");
          return valueA; }),
      [&](const Rule& rule, const ValueType value) {
        return valueA == intFromValue(value);
      } });
  engine.addRule({
      "value-B", simpleAction({}, [&] (const std::vector<int>& inputs) {
          builtKeys.push_back("value-B");
          return valueB; }),
      [&](const Rule& rule, const ValueType& value) {
        return valueB == intFromValue(value);
      } });
  engine.addRule({
      "value-R",
      simpleAction({"value-A", "value-B"},
                   [&] (const std::vector<int>& inputs) {
                     EXPECT_EQ(2U, inputs.size());
                     EXPECT_EQ(valueA, inputs[0]);
                     EXPECT_EQ(valueB, inputs[1]);
                     builtKeys.push_back("value-R");
                     return inputs[0] * inputs[1] * 5;
                   }) });

  // Build the first result.
  builtKeys.clear();
  EXPECT_EQ(valueA * valueB * 5, intFromValue(engine.build("value-R")));
  EXPECT_EQ(3U, builtKeys.size());
  EXPECT_EQ("value-A", builtKeys[0]);
  EXPECT_EQ("value-B", builtKeys[1]);
  EXPECT_EQ("value-R", builtKeys[2]);

  // Mark value-A as having changed, then rebuild and sanity check.
  valueA = 7;
  builtKeys.clear();
  EXPECT_EQ(valueA * valueB * 5, intFromValue(engine.build("value-R")));
  EXPECT_EQ(2U, builtKeys.size());
  EXPECT_EQ("value-A", builtKeys[0]);
  EXPECT_EQ("value-R", builtKeys[1]);

  // Check that a subsequent build is null.
  builtKeys.clear();
  EXPECT_EQ(valueA * valueB * 5, intFromValue(engine.build("value-R")));
  EXPECT_EQ(0U, builtKeys.size());
}

TEST(BuildEngineTest, basicIncremental) {
  // Check a build graph responds to incremental changes appropriately.
  //
  // Dependencies:
  //   value-C: (value-A, value-B)
  //   value-R: (value-A, value-C)
  //   value-D: (value-R)
  //   value-R2: (value-D)
  //
  // If value-A or value-B change, then value-C and value-R are recomputed when
  // value-R is built, but value-R2 is not recomputed.
  //
  // If value-R2 is built, then value-B changes and value-R is built, then when
  // value-R2 is built again only value-D and value-R2 are rebuilt.
  std::vector<std::string> builtKeys;
  SimpleBuildEngineDelegate delegate;
  core::BuildEngine engine(delegate);
  int valueA = 2;
  int valueB = 3;
  engine.addRule({
      "value-A", simpleAction({}, [&] (const std::vector<int>& inputs) {
          builtKeys.push_back("value-A");
          return valueA; }),
      [&](const Rule& rule, const ValueType& value) {
        return valueA == intFromValue(value);
      } });
  engine.addRule({
      "value-B", simpleAction({}, [&] (const std::vector<int>& inputs) {
          builtKeys.push_back("value-B");
          return valueB; }),
      [&](const Rule& rule, const ValueType& value) {
        return valueB == intFromValue(value);
      } });
  engine.addRule({
      "value-C",
      simpleAction({"value-A", "value-B"},
                   [&] (const std::vector<int>& inputs) {
                     EXPECT_EQ(2U, inputs.size());
                     EXPECT_EQ(valueA, inputs[0]);
                     EXPECT_EQ(valueB, inputs[1]);
                     builtKeys.push_back("value-C");
                     return inputs[0] * inputs[1] * 5;
                   }) });
  engine.addRule({
      "value-R",
      simpleAction({"value-A", "value-C"},
                   [&] (const std::vector<int>& inputs) {
                     EXPECT_EQ(2U, inputs.size());
                     EXPECT_EQ(valueA, inputs[0]);
                     EXPECT_EQ(valueA * valueB * 5, inputs[1]);
                     builtKeys.push_back("value-R");
                     return inputs[0] * inputs[1] * 7;
                   }) });
  engine.addRule({
      "value-D",
      simpleAction({"value-R"},
                   [&] (const std::vector<int>& inputs) {
                     EXPECT_EQ(1U, inputs.size());
                     EXPECT_EQ(valueA * valueA * valueB * 5 * 7,
                               inputs[0]);
                     builtKeys.push_back("value-D");
                     return inputs[0] * 11;
                   }) });
  engine.addRule({
      "value-R2",
      simpleAction({"value-D"},
                   [&] (const std::vector<int>& inputs) {
                     EXPECT_EQ(1U, inputs.size());
                     EXPECT_EQ(valueA * valueA * valueB * 5 * 7 * 11,
                               inputs[0]);
                     builtKeys.push_back("value-R2");
                     return inputs[0] * 13;
                   }) });

  // Build the first result.
  builtKeys.clear();
  EXPECT_EQ(valueA * valueA * valueB * 5 * 7,
            intFromValue(engine.build("value-R")));
  EXPECT_EQ(4U, builtKeys.size());
  EXPECT_EQ("value-A", builtKeys[0]);
  EXPECT_EQ("value-B", builtKeys[1]);
  EXPECT_EQ("value-C", builtKeys[2]);
  EXPECT_EQ("value-R", builtKeys[3]);

  // Mark value-A as having changed, then rebuild and sanity check.
  valueA = 17;
  builtKeys.clear();
  EXPECT_EQ(valueA * valueA * valueB * 5 * 7,
            intFromValue(engine.build("value-R")));
  EXPECT_EQ(3U, builtKeys.size());
  EXPECT_EQ("value-A", builtKeys[0]);
  EXPECT_EQ("value-C", builtKeys[1]);
  EXPECT_EQ("value-R", builtKeys[2]);

  // Mark value-B as having changed, then rebuild and sanity check.
  valueB = 19;
  builtKeys.clear();
  EXPECT_EQ(valueA * valueA * valueB * 5 * 7,
            intFromValue(engine.build("value-R")));
  EXPECT_EQ(3U, builtKeys.size());
  EXPECT_EQ("value-B", builtKeys[0]);
  EXPECT_EQ("value-C", builtKeys[1]);
  EXPECT_EQ("value-R", builtKeys[2]);

  // Build value-R2 for the first time.
  builtKeys.clear();
  EXPECT_EQ(valueA * valueA * valueB * 5 * 7 * 11 * 13,
            intFromValue(engine.build("value-R2")));
  EXPECT_EQ(2U, builtKeys.size());
  EXPECT_EQ("value-D", builtKeys[0]);
  EXPECT_EQ("value-R2", builtKeys[1]);

  // Now mark value-B as having changed, then rebuild value-R, then build
  // value-R2 and sanity check.
  valueB = 23;
  builtKeys.clear();
  EXPECT_EQ(valueA * valueA * valueB * 5 * 7,
            intFromValue(engine.build("value-R")));
  EXPECT_EQ(3U, builtKeys.size());
  EXPECT_EQ("value-B", builtKeys[0]);
  EXPECT_EQ("value-C", builtKeys[1]);
  EXPECT_EQ("value-R", builtKeys[2]);
  builtKeys.clear();
  EXPECT_EQ(valueA * valueA * valueB * 5 * 7 * 11 * 13,
            intFromValue(engine.build("value-R2")));
  EXPECT_EQ(2U, builtKeys.size());
  EXPECT_EQ("value-D", builtKeys[0]);
  EXPECT_EQ("value-R2", builtKeys[1]);

  // Final sanity check.
  builtKeys.clear();
  EXPECT_EQ(valueA * valueA * valueB * 5 * 7,
            intFromValue(engine.build("value-R")));
  EXPECT_EQ(valueA * valueA * valueB * 5 * 7 * 11 * 13,
            intFromValue(engine.build("value-R2")));
  EXPECT_EQ(0U, builtKeys.size());
}

TEST(BuildEngineTest, incrementalDependency) {
  // Check that the engine properly clears the individual result dependencies
  // when a rule is rerun.
  //
  // Dependencies:
  //   value-R: (value-A)
  //
  // FIXME: This test is rather cumbersome for all it is trying to check, maybe
  // we need a logging BuildDB implementation or something that would be easier
  // to test against (or just update the trace to track it).

  SimpleBuildEngineDelegate delegate;
  core::BuildEngine engine(delegate);

  // Attach a custom database, used to get the results.
  class CustomDB : public BuildDB {
  public:
    std::unordered_map<core::KeyType, Result> ruleResults;

    virtual uint64_t getCurrentIteration() override {
      return 0;
    }
    virtual void setCurrentIteration(uint64_t value) override {}
    virtual bool lookupRuleResult(const Rule& rule,
                                  Result* result_out) override {
      return false;
    }
    virtual void setRuleResult(const Rule& rule,
                               const Result& result) override {
      ruleResults[rule.key] = result;
    }
    virtual void buildStarted() override {}
    virtual void buildComplete() override {}
  };
  CustomDB *db = new CustomDB();
  engine.attachDB(std::unique_ptr<CustomDB>(db));

  int valueA = 2;
  engine.addRule({
      "value-A", simpleAction({}, [&] (const std::vector<int>& inputs) {
          return valueA; }),
      [&](const Rule& rule, const ValueType& value) {
        return valueA == intFromValue(value);
      } });
  engine.addRule({
      "value-R",
      simpleAction({"value-A"},
                   [&] (const std::vector<int>& inputs) {
                     EXPECT_EQ(1U, inputs.size());
                     EXPECT_EQ(valueA, inputs[0]);
                     return inputs[0] * 3;
                   }) });

  // Build the first result.
  EXPECT_EQ(valueA * 3, intFromValue(engine.build("value-R")));

  // Mark value-A as having changed, then rebuild.
  valueA = 5;
  EXPECT_EQ(valueA * 3, intFromValue(engine.build("value-R")));

  // Check the rule results.
  const Result& valueRResult = db->ruleResults["value-R"];
  EXPECT_EQ(valueA * 3, intFromValue(valueRResult.value));
  EXPECT_EQ(1U, valueRResult.dependencies.size());
}

TEST(BuildEngineTest, deepDependencyScanningStack) {
  // Check that the engine can handle dependency scanning of a very deep stack,
  // which would probably crash blowing the stack if the engine used naive
  // recursion.
  //
  // FIXME: It would be nice to run this on a thread with a small stack to
  // guarantee this, and to be able to make the depth small so the test runs
  // faster.
  int depth = 10000;

  SimpleBuildEngineDelegate delegate;
  core::BuildEngine engine(delegate);
  int lastInputValue = 0;
  for (int i = 0; i != depth; ++i) {
    char name[32];
    sprintf(name, "input-%d", i);
    if (i != depth-1) {
      char inputName[32];
      sprintf(inputName, "input-%d", i+1);
      engine.addRule({
          name, simpleAction({ inputName },
                             [] (const std::vector<int>& inputs) {
                               return inputs[0]; }) });
    } else {
      engine.addRule({
          name,
          simpleAction({},
                       [&] (const std::vector<int>& inputs) {
                         return lastInputValue; }),
          [&](const Rule& rule, const ValueType& value) {
            // FIXME: Once we have custom ValueType objects, we would like to
            // have timestamps on the value and just compare to a timestamp
            // (similar to what we would do for a file).
            return lastInputValue == intFromValue(value);
          } });
    }
  }

  // Build the first result.
  lastInputValue = 42;
  EXPECT_EQ(lastInputValue, intFromValue(engine.build("input-0")));

  // Perform a null build on the result.
  EXPECT_EQ(lastInputValue, intFromValue(engine.build("input-0")));

  // Perform a full rebuild on the result.
  lastInputValue = 52;
  EXPECT_EQ(lastInputValue, intFromValue(engine.build("input-0")));
}

TEST(BuildEngineTest, discoveredDependencies) {
  // Check basic support for tasks to report discovered dependencies.

  // This models a task which has some out-of-band way to read the input.
  class TaskWithDiscoveredDependency : public Task {
    int& valueB;
    int computedInputValue = -1;

  public:
    TaskWithDiscoveredDependency(int& valueB) : valueB(valueB) { }

    virtual void start(BuildEngine& engine) override {
      // Request the known input.
      engine.taskNeedsInput(this, "value-A", 0);
    }

    virtual void provideValue(BuildEngine&, uintptr_t inputID,
                              const ValueType& value) override {
      assert(inputID == 0);
      computedInputValue = intFromValue(value);
    }

    virtual void inputsAvailable(core::BuildEngine& engine) override {
      // Report the discovered dependency.
      engine.taskDiscoveredDependency(this, "value-B");
      engine.taskIsComplete(this, intToValue(computedInputValue * valueB * 5));
    }
  };

  std::vector<std::string> builtKeys;
  SimpleBuildEngineDelegate delegate;
  core::BuildEngine engine(delegate);
  int valueA = 2;
  int valueB = 3;
  engine.addRule({
      "value-A",
      simpleAction({ },
                   [&] (const std::vector<int>& inputs) {
                     builtKeys.push_back("value-A");
                     return valueA;
                   }),
      [&](const Rule& rule, const ValueType& value) {
        return valueA == intFromValue(value);
      } });
  engine.addRule({
      "value-B",
      simpleAction({ },
                   [&] (const std::vector<int>& inputs) {
                     builtKeys.push_back("value-B");
                     return valueB;
                   }),
      [&](const Rule& rule, const ValueType& value) {
        return valueB == intFromValue(value);
      } });
  engine.addRule({
      "output",
      [&valueB, &builtKeys] (BuildEngine& engine) {
        builtKeys.push_back("output");
        return engine.registerTask(new TaskWithDiscoveredDependency(valueB));
      } });

  // Build the first result.
  builtKeys.clear();
  EXPECT_EQ(valueA * valueB * 5, intFromValue(engine.build("output")));
  EXPECT_EQ(std::vector<std::string>({ "output", "value-A", "value-B" }),
            builtKeys);

  // Verify that the next build is a null build.
  builtKeys.clear();
  EXPECT_EQ(valueA * valueB * 5, intFromValue(engine.build("output")));
  EXPECT_EQ(std::vector<std::string>(), builtKeys);

  // Verify that the build depends on valueB.
  valueB = 7;
  builtKeys.clear();
  EXPECT_EQ(valueA * valueB * 5, intFromValue(engine.build("output")));
  EXPECT_EQ(std::vector<std::string>({ "value-B", "output" }),
            builtKeys);


  // Verify again that the next build is a null build.
  builtKeys.clear();
  EXPECT_EQ(valueA * valueB * 5, intFromValue(engine.build("output")));
  EXPECT_EQ(std::vector<std::string>(), builtKeys);
}

TEST(BuildEngineTest, unchangedOutputs) {
  // Check building with unchanged outputs.
  std::vector<std::string> builtKeys;
  SimpleBuildEngineDelegate delegate;
  core::BuildEngine engine(delegate);
  engine.addRule({
      "value",
      simpleAction({}, [&] (const std::vector<int>& inputs) {
        builtKeys.push_back("value");
        return 2; }),
      [&](const Rule& rule, const ValueType& value) {
        // Always rebuild
        return false;
      } });
  engine.addRule({
      "result",
      simpleAction({"value"},
                   [&] (const std::vector<int>& inputs) {
                     EXPECT_EQ(1U, inputs.size());
                     EXPECT_EQ(2, inputs[0]);
                     builtKeys.push_back("result");
                     return inputs[0] * 3;
                   }) });

  // Build the result.
  EXPECT_EQ(2 * 3, intFromValue(engine.build("result")));
  EXPECT_EQ(2U, builtKeys.size());
  EXPECT_EQ("value", builtKeys[0]);
  EXPECT_EQ("result", builtKeys[1]);

  // Rebuild the result.
  //
  // Only "value" should rebuild, as it explicitly declares itself invalid each
  // time, but "result" should not need to rerun.
  builtKeys.clear();
  EXPECT_EQ(2 * 3, intFromValue(engine.build("result")));
  EXPECT_EQ(1U, builtKeys.size());
  EXPECT_EQ("value", builtKeys[0]);
}

TEST(BuildEngineTest, StatusCallbacks) {
  unsigned numScanned = 0;
  unsigned numComplete = 0;
  SimpleBuildEngineDelegate delegate;
  core::BuildEngine engine(delegate);
  engine.addRule({
      "input",
      simpleAction({}, [&] (const std::vector<int>& inputs) {
          return 2; }),
      nullptr,
      [&] (core::Rule::StatusKind status) {
        if (status == core::Rule::StatusKind::IsScanning) {
          ++numScanned;
        } else {
          assert(status == core::Rule::StatusKind::IsComplete);
          ++numComplete;
        }
      } });
  engine.addRule({
      "output",
      simpleAction({"input"},
                   [&] (const std::vector<int>& inputs) {
                     return inputs[0] * 3;
                   }),
      nullptr,
      [&] (core::Rule::StatusKind status) {
        if (status == core::Rule::StatusKind::IsScanning) {
          ++numScanned;
        } else {
          assert(status == core::Rule::StatusKind::IsComplete);
          ++numComplete;
        }
      } });

  // Build the result.
  EXPECT_EQ(2 * 3, intFromValue(engine.build("output")));
  EXPECT_EQ(2U, numScanned);
  EXPECT_EQ(2U, numComplete);
}

}
