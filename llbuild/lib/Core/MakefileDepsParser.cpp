//===-- MakefileDepsParser.cpp --------------------------------------------===//
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

#include "llbuild/Core/MakefileDepsParser.h"

using namespace llbuild;
using namespace llbuild::core;

MakefileDepsParser::ParseActions::~ParseActions() {}

#pragma mark - MakefileDepsParser Implementation

static bool isWordChar(int c) {
  switch (c) {
  case '\0':
  case '\t':
  case '\n':
  case ' ':
  case '$':
  case ':':
  case ';':
  case '=':
  case '|':
  case '%':
    return false;
  default:
    return true;
  }
}

static void skipWhitespaceAndComments(const char*& cur, const char* end) {
  for (; cur != end; ++cur) {
    int c = *cur;

    // Skip comments.
    if (c == '#') {
      // Skip to the next newline.
      while (cur + 1 != end && cur[1] == '\n')
        ++cur;
      continue;
    }

    if (c == ' ' || c == '\t' || c == '\n')
      continue;

    break;
  }
}

static void skipNonNewlineWhitespace(const char*& cur, const char* end) {
  for (; cur != end; ++cur) {
    int c = *cur;

    // Skip regular whitespace.
    if (c == ' ' || c == '\t')
      continue;

    // If this is an escaped newline, also skip it.
    if (c == '\\' && cur + 1 != end && cur[1] == '\n') {
      ++cur;
      continue;
    }

    // Otherwise, stop scanning.
    break;
  }
}

static void skipToEndOfLine(const char*& cur, const char* end) {
  for (; cur != end; ++cur) {
    int c = *cur;

    if (c == '\n') {
      ++cur;
      break;
    }
  }
}

static void lexWord(const char*& cur, const char* end) {
  for (; cur != end; ++cur) {
    int c = *cur;

    // Check if this is an escape sequence.
    if (c == '\\') {
      // If this is a line continuation, it ends the word.
      if (cur + 1 != end && cur[1] == '\n')
        break;

      // Otherwise, skip the escaped character.
      ++cur;
      continue;
    }

    // Otherwise, if this is not a valid word character then skip it.
    if (!isWordChar(*cur))
      break;
  }
}

void MakefileDepsParser::parse() {
  const char* cur = data;
  const char* end = data + length;

  // While we have input data...
  while (cur != end) {
    // Skip leading whitespace and comments.
    skipWhitespaceAndComments(cur, end);

    // If we have reached the end of the input, we are done.
    if (cur == end)
      break;
    
    // The next token should be a word.
    const char* wordStart = cur;
    lexWord(cur, end);
    if (cur == wordStart) {
      actions.error("unexpected character in file", cur - data);
      skipToEndOfLine(cur, end);
      continue;
    }
    actions.actOnRuleStart(wordStart, cur - wordStart);

    // The next token should be a colon.
    skipNonNewlineWhitespace(cur, end);
    if (cur == end || *cur != ':') {
      actions.error("missing ':' following rule", cur - data);
      actions.actOnRuleEnd();
      skipToEndOfLine(cur, end);
      continue;
    }

    // Skip the colon.
    ++cur;

    // Consume dependency words until we reach the end of a line.
    while (cur != end) {
      // Skip forward and check for EOL.
      skipNonNewlineWhitespace(cur, end);
      if (cur == end || *cur == '\n')
        break;

      // Otherwise, we should have a word.
      const char* wordStart = cur;
      lexWord(cur, end);
      if (cur == wordStart) {
        actions.error("unexpected character in prerequisites", cur - data);
        skipToEndOfLine(cur, end);
        continue;
      }
      actions.actOnRuleDependency(wordStart, cur - wordStart);
    }
    actions.actOnRuleEnd();
  }
}
