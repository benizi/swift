//===-- BuildSystemFrontend.cpp -------------------------------------------===//
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

#include "llbuild/BuildSystem/BuildSystemFrontend.h"

#include "llbuild/Basic/LLVM.h"
#include "llbuild/BuildSystem/BuildExecutionQueue.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <atomic>
#include <memory>

#include <unistd.h>

using namespace llbuild;
using namespace llbuild::buildsystem;

#pragma mark - BuildSystemInvocation implementation

void BuildSystemInvocation::getUsage(int optionWidth, raw_ostream& os) {
  const struct Options {
    llvm::StringRef option, helpText;
  } options[] = {
    { "--help", "show this help message and exit" },
    { "-C <PATH>, --chdir <PATH>", "change directory to PATH before building" },
    { "--no-db", "disable use of a build database" },
    { "--db <PATH>", "enable building against the database at PATH" },
    { "-f <PATH>", "load the build task file at PATH" },
    { "--serial", "do not build in parallel" },
    { "-v, --verbose", "show verbose status information" },
    { "--trace <PATH>", "trace build engine operation to PATH" },
  };
  
  for (const auto& entry: options) {
    os << "  " << llvm::format("%-*s", optionWidth, entry.option) << " "
       << entry.helpText << "\n";
  }
}

void BuildSystemInvocation::parse(llvm::ArrayRef<std::string> args,
                                  llvm::SourceMgr& sourceMgr) {
  auto error = [&](const Twine &message) {
    sourceMgr.PrintMessage(llvm::SMLoc{}, llvm::SourceMgr::DK_Error, message);
    hadErrors = true;
  };

  while (!args.empty()) {
    const auto& option = args.front();
    args = args.slice(1);

    if (option == "-") {
      for (const auto& arg: args) {
        positionalArgs.push_back(arg);
      }
      break;
    }

    if (!option.empty() && option[0] != '-') {
      positionalArgs.push_back(option);
      continue;
    }
    
    if (option == "--help") {
      showUsage = true;
      break;
    } else if (option == "--no-db") {
      dbPath = "";
    } else if (option == "--db") {
      if (args.empty()) {
        error("missing argument to '" + option + "'");
        break;
      }
      dbPath = args[0];
      args = args.slice(1);
    } else if (option == "-C" || option == "--chdir") {
      if (args.empty()) {
        error("missing argument to '" + option + "'");
        break;
      }
      chdirPath = args[0];
      args = args.slice(1);
    } else if (option == "-f") {
      if (args.empty()) {
        error("missing argument to '" + option + "'");
        break;
      }
      buildFilePath = args[0];
      args = args.slice(1);
    } else if (option == "--serial") {
      useSerialBuild = true;
    } else if (option == "-v" || option == "--verbose") {
      showVerboseStatus = true;
    } else if (option == "--trace") {
      if (args.empty()) {
        error("missing argument to '" + option + "'");
        break;
      }
      traceFilePath = args[0];
      args = args.slice(1);
    } else {
      error("invalid option '" + option + "'");
      break;
    }
  }
}

#pragma mark - BuildSystemFrontendDelegate implementation

namespace {

struct BuildSystemFrontendDelegateImpl {
  llvm::SourceMgr& sourceMgr;
  const BuildSystemInvocation& invocation;
  
  StringRef bufferBeingParsed;
  std::atomic<unsigned> numErrors{0};
  std::atomic<unsigned> numFailedCommands{0};

  BuildSystemFrontendDelegateImpl(llvm::SourceMgr& sourceMgr,
                                  const BuildSystemInvocation& invocation)
      : sourceMgr(sourceMgr), invocation(invocation) {}
};

}

BuildSystemFrontendDelegate::
BuildSystemFrontendDelegate(llvm::SourceMgr& sourceMgr,
                            const BuildSystemInvocation& invocation,
                            StringRef name,
                            uint32_t version)
    : BuildSystemDelegate(name, version),
      impl(new BuildSystemFrontendDelegateImpl(sourceMgr, invocation))
{
  
}

BuildSystemFrontendDelegate::~BuildSystemFrontendDelegate() {
  delete static_cast<BuildSystemFrontendDelegateImpl*>(this->impl);
}

void
BuildSystemFrontendDelegate::setFileContentsBeingParsed(StringRef buffer) {
  auto impl = static_cast<BuildSystemFrontendDelegateImpl*>(this->impl);

  impl->bufferBeingParsed = buffer;
}

llvm::SourceMgr& BuildSystemFrontendDelegate::getSourceMgr() {
  auto impl = static_cast<BuildSystemFrontendDelegateImpl*>(this->impl);
  
  return impl->sourceMgr;
}

unsigned BuildSystemFrontendDelegate::getNumErrors() {
  auto impl = static_cast<BuildSystemFrontendDelegateImpl*>(this->impl);
  
  return impl->numErrors;
}

unsigned BuildSystemFrontendDelegate::getNumFailedCommands() {
  auto impl = static_cast<BuildSystemFrontendDelegateImpl*>(this->impl);

  return impl->numFailedCommands;
}

void
BuildSystemFrontendDelegate::error(const Twine& message) {
  error("", {}, message.str());
}

void
BuildSystemFrontendDelegate::error(StringRef filename,
                                   const Token& at,
                                   const Twine& message) {
  auto impl = static_cast<BuildSystemFrontendDelegateImpl*>(this->impl);
  
  ++impl->numErrors;

  // If we have a file and token, resolve the location and range to one
  // accessible by the source manager.
  //
  // FIXME: We shouldn't need to do this, but should switch llbuild to using
  // SourceMgr natively.
  llvm::SMLoc loc{};
  llvm::SMRange range{};
  if (!filename.empty() && at.start) {
    // FIXME: We ignore errors here, for now, this will be resolved when we move
    // to SourceMgr completely.
    auto buffer = llvm::MemoryBuffer::getFile(filename);
    if (!buffer.getError()) {
      unsigned offset = at.start - impl->bufferBeingParsed.data();
      if (offset + at.length < (*buffer)->getBufferSize()) {
        range.Start = loc = llvm::SMLoc::getFromPointer(
            (*buffer)->getBufferStart() + offset);
        range.End = llvm::SMLoc::getFromPointer(
            (*buffer)->getBufferStart() + (offset + at.length));
        getSourceMgr().AddNewSourceBuffer(std::move(*buffer), llvm::SMLoc{});
      }
    }
  }

  if (range.Start.isValid()) {
    getSourceMgr().PrintMessage(loc, llvm::SourceMgr::DK_Error, message, range);
  } else {
    getSourceMgr().PrintMessage(loc, llvm::SourceMgr::DK_Error, message);
  }
}

std::unique_ptr<BuildExecutionQueue>
BuildSystemFrontendDelegate::createExecutionQueue() {
  auto impl = static_cast<BuildSystemFrontendDelegateImpl*>(this->impl);
  
  if (impl->invocation.useSerialBuild) {
    return std::unique_ptr<BuildExecutionQueue>(
        createLaneBasedExecutionQueue(1));
  }
    
  // Get the number of CPUs to use.
  long numCPUs = sysconf(_SC_NPROCESSORS_ONLN);
  unsigned numLanes;
  if (numCPUs < 0) {
    error("<unknown>", {}, "unable to detect number of CPUs");
    numLanes = 1;
  } else {
    numLanes = numCPUs + 2;
  }
    
  return std::unique_ptr<BuildExecutionQueue>(
      createLaneBasedExecutionQueue(numLanes));
}

bool BuildSystemFrontendDelegate::isCancelled() {
  // Stop the build after any command failures.
  return getNumFailedCommands() > 0;
}

void BuildSystemFrontendDelegate::hadCommandFailure() {
  auto impl = static_cast<BuildSystemFrontendDelegateImpl*>(this->impl);
  
  // Increment the failed command count.
  ++impl->numFailedCommands;
}

bool BuildSystemFrontendDelegate::showVerboseStatus() {
  auto impl = static_cast<BuildSystemFrontendDelegateImpl*>(this->impl);
  
  return impl->invocation.showVerboseStatus;
}

#pragma mark - BuildSystemFrontend implementation

BuildSystemFrontend::
BuildSystemFrontend(BuildSystemFrontendDelegate& delegate,
                    const BuildSystemInvocation& invocation)
    : delegate(delegate), invocation(invocation)
{
}

bool BuildSystemFrontend::build(StringRef targetToBuild) {
  // Honor the --chdir option, if used.
  if (!invocation.chdirPath.empty()) {
    if (::chdir(invocation.chdirPath.c_str()) < 0) {
      getDelegate().error(Twine("unable to honor --chdir: ") + strerror(errno));
      return false;
    }
  }

  // Create the build system.
  BuildSystem system(delegate, invocation.buildFilePath);

  // Enable tracing, if requested.
  if (!invocation.traceFilePath.empty()) {
    std::string error;
    if (!system.enableTracing(invocation.traceFilePath, &error)) {
      getDelegate().error(Twine("unable to enable tracing: ") + error);
      return false;
    }
  }

  // Attach the database.
  if (!invocation.dbPath.empty()) {
    // If the database path is relative, always make it relative to the input
    // file.
    SmallString<256> tmp;
    StringRef dbPath = invocation.dbPath;
    if (llvm::sys::path::has_relative_path(invocation.dbPath)) {
      llvm::sys::path::append(
          tmp, llvm::sys::path::parent_path(invocation.buildFilePath),
          invocation.dbPath);
      dbPath = tmp.str();
    }
    
    std::string error;
    if (!system.attachDB(dbPath, &error)) {
      getDelegate().error(Twine("unable to attach DB: ") + error);
      return false;
    }
  }

  // If something unspecified failed about the build, return an error.
  if (!system.build(targetToBuild)) {
    return false;
  }

  // If there were failed commands, report the count and return an error.
  if (delegate.getNumFailedCommands()) {
    getDelegate().error("build had " + Twine(delegate.getNumFailedCommands()) +
                        " command failures");
    return false;
  }

  // Otherwise, return an error only if there were unspecified errors.
  return delegate.getNumErrors() == 0;
}
