---
id: 4
title: Performance Benchmarking & Statistics Readback Optimization
type: prototype
status: open
blocked_by: []
---

## Question

How can we measure and minimize CPU/GPU overhead of the frame read-back stats path (currently 160x90 RGBA8 CPU readback smoothed at 10Hz), and can asynchronous GPU texture mapping or frame throttling be improved?

## Spec & Requirements to Resolve
1. Benchmark CPU cycle usage and render frame latency during auto-stats gathering (Grey World / Shades of Grey).
2. Evaluate if frame downsampling rate (currently 10Hz) or texture staging queue can be tuned to eliminate any micro-stutters during 60 FPS / 120 FPS video encoding.
