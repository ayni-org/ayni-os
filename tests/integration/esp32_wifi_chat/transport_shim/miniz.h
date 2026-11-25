#pragma once
// MotorPanel interaction tests never construct Canvas. Its rendering endpoints are replaced;
// the native preview separately compiles the real Canvas with its zlib adapter.
struct tinfl_decompressor {};
