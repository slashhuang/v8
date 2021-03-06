// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

var source =
`
function fib(x) {
  if (x < 2) return 1;
  return fib(x-1) + fib(x-2);
}
(function iife() {
  return 1;
})();
fib(5);
`;

var break_source =
`
function g() {
  debugger;
}
function f(x) {
  if (x == 0) g();
  else f(x - 1);
}
function h() {
  g();
}
f(3);
`;

InspectorTest.log("Test collecting code coverage data with Profiler.collectCoverage.");

function ClearAndGC() {
  return Protocol.Runtime.evaluate({ expression: "fib = null;" }).then(GC);
}

function GC() {
  return Protocol.HeapProfiler.enable()
           .then(() => Protocol.HeapProfiler.collectGarbage())
           .then(() => Protocol.HeapProfiler.disable());
}

function LogSorted(message) {
  message.result.result.sort((a, b) => parseInt(a.scriptId) - parseInt(b.scriptId));
  return InspectorTest.logMessage(message);
}

InspectorTest.runTestSuite([
  function testPreciseBaseline(next)
  {
    Protocol.Runtime.enable()
      .then(() => Protocol.Runtime.compileScript({ expression: source, sourceURL: "0", persistScript: true }))
      .then((result) => Protocol.Runtime.runScript({ scriptId: result.result.scriptId }))
      .then(GC)
      .then(Protocol.Profiler.enable)
      .then(Protocol.Profiler.startPreciseCoverage)
      .then(Protocol.Profiler.takePreciseCoverage)
      .then(LogSorted)
      .then(Protocol.Profiler.takePreciseCoverage)
      .then(LogSorted)
      .then(Protocol.Profiler.stopPreciseCoverage)
      .then(ClearAndGC)
      .then(Protocol.Profiler.disable)
      .then(Protocol.Runtime.disable)
      .then(next);
  },
  function testPreciseCoverage(next)
  {
    Protocol.Runtime.enable()
      .then(Protocol.Profiler.enable)
      .then(Protocol.Profiler.startPreciseCoverage)
      .then(() => Protocol.Runtime.compileScript({ expression: source, sourceURL: "1", persistScript: true }))
      .then((result) => Protocol.Runtime.runScript({ scriptId: result.result.scriptId }))
      .then(ClearAndGC)
      .then(InspectorTest.logMessage)
      .then(Protocol.Profiler.takePreciseCoverage)
      .then(LogSorted)
      .then(Protocol.Profiler.takePreciseCoverage)
      .then(LogSorted)
      .then(ClearAndGC)
      .then(Protocol.Profiler.stopPreciseCoverage)
      .then(Protocol.Profiler.disable)
      .then(Protocol.Runtime.disable)
      .then(next);
  },
  function testPreciseCoverageFail(next)
  {
    Protocol.Runtime.enable()
      .then(Protocol.Profiler.enable)
      .then(() => Protocol.Runtime.compileScript({ expression: source, sourceURL: "2", persistScript: true }))
      .then((result) => Protocol.Runtime.runScript({ scriptId: result.result.scriptId }))
      .then(InspectorTest.logMessage)
      .then(ClearAndGC)
      .then(Protocol.Profiler.takePreciseCoverage)
      .then(InspectorTest.logMessage)
      .then(ClearAndGC)
      .then(Protocol.Profiler.disable)
      .then(Protocol.Runtime.disable)
      .then(next);
  },
  function testBestEffortCoverage(next)
  {
    Protocol.Runtime.enable()
      .then(() => Protocol.Runtime.compileScript({ expression: source, sourceURL: "3", persistScript: true }))
      .then((result) => Protocol.Runtime.runScript({ scriptId: result.result.scriptId }))
      .then(InspectorTest.logMessage)
      .then(ClearAndGC)
      .then(Protocol.Profiler.getBestEffortCoverage)
      .then(LogSorted)
      .then(Protocol.Profiler.getBestEffortCoverage)
      .then(LogSorted)
      .then(ClearAndGC)
      .then(Protocol.Runtime.disable)
      .then(next);
  },
  function testBestEffortCoveragePrecise(next)
  {
    Protocol.Runtime.enable()
      .then(Protocol.Profiler.enable)
      .then(Protocol.Profiler.startPreciseCoverage)
      .then(() => Protocol.Runtime.compileScript({ expression: source, sourceURL: "4", persistScript: true }))
      .then((result) => Protocol.Runtime.runScript({ scriptId: result.result.scriptId }))
      .then(InspectorTest.logMessage)
      .then(ClearAndGC)
      .then(Protocol.Profiler.getBestEffortCoverage)
      .then(LogSorted)
      .then(Protocol.Profiler.getBestEffortCoverage)
      .then(LogSorted)
      .then(ClearAndGC)
      .then(Protocol.Profiler.stopPreciseCoverage)
      .then(Protocol.Profiler.disable)
      .then(Protocol.Runtime.disable)
      .then(next);
  },
  function testEnablePreciseCoverageAtPause(next)
  {
    function handleDebuggerPause() {
      Protocol.Profiler.enable()
          .then(Protocol.Profiler.startPreciseCoverage)
          .then(Protocol.Debugger.resume)
    }
    Protocol.Debugger.enable();
    Protocol.Debugger.oncePaused().then(handleDebuggerPause);
    Protocol.Runtime.enable()
      .then(() => Protocol.Runtime.compileScript({ expression: break_source, sourceURL: "5", persistScript: true }))
      .then((result) => Protocol.Runtime.runScript({ scriptId: result.result.scriptId }))
      .then(InspectorTest.logMessage)
      .then(ClearAndGC)
      .then(Protocol.Profiler.takePreciseCoverage)
      .then(LogSorted)
      .then(Protocol.Profiler.stopPreciseCoverage)
      .then(Protocol.Profiler.disable)
      .then(Protocol.Runtime.disable)
      .then(next);
  },
]);
