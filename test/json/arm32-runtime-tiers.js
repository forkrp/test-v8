// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license in LICENSE.
// Flags: --allow-natives-syntax --no-concurrent-recompilation
// Argument: "neon" for the full ARMv7 feature set, "scalar" for no NEON/D16.
// This intentionally does not call JSON: the compatible engine must keep
// ordinary arithmetic correct and retain TurboFan when skipping Maglev.
'use strict';

const expectMaglev = arguments[0] === 'neon';
if (%IsMaglevEnabled() !== expectMaglev) throw Error('Maglev CPU gate');

function calculate(x) { return (x * 3 + 0.125) / 8; }
%PrepareFunctionForOptimization(calculate);
for (let i = 0; i < 100; ++i) calculate(i);
%OptimizeFunctionOnNextCall(calculate);
if (calculate(4096) !== 1536.015625) throw Error('optimized arithmetic');

// OptimizationStatus::kOptimized and kTurboFanned from src/runtime/runtime.h.
const status = %GetOptimizationStatus(calculate);
const required = (1 << 4) | (1 << 6);
if ((status & required) !== required) throw Error('TurboFan fallback disabled');
print('ARM32_TIERS_OK maglev=' + %IsMaglevEnabled() + ' turbofan=true');
