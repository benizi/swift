// RUN: not %target-swift-frontend %s -parse

// Distributed under the terms of the MIT license
// Test case submitted to project by https://github.com/practicalswift (practicalswift)
// Test case found by fuzzing

import Foundation
class k<f>: NSObject {
    d e: f
    g(e: f) {
        j        h.g()
    }
}
d
protocol i : d { func d
i
