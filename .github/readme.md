# Parallelising Godot to get 6x speedup

I needed an application for my multithreading library Macrame. A game engine. I know Unreal Engine pretty well, but it's way too complicated for this exercise. So I tried Godot, for the first time. It's the most popular open-source game engine, and it's C++, but I was worried it's also too much work to get any meaningful result.

Godot doesn't have something like UE's CitySample or Lyra sample projects. The best I could find is the official Godot Benchmarks project, so I took it as a base. Being used as a benchmark means it doesn't have any obvious and easy to fix perf issues, which suits me well. But it was designed to perform pretty well on an average H/W, so I cranked it up a bit, 500 NPCs instead of 200. Also for this experiment I wasn't interested in GPU optimisation, so I reduced GPU pressure a bit to avoid being GPU-bound. Though the rendering pipeline parallelisation was still fair game.

The result of this exercise was 6 times speedup, 32 ms -> 5.5 ms, or ~30 fps -> ~180 fps. It can be optimised much further, despite 5.5ms is nearing the area of diminishing returns. This sample didn't provide enough material for parallelisation because it lacks lots of typical game systems or doesn't stress them enough: audio, animation depth, perception raycasts, scripts, skeleton modifiers/IK, event traffic, HUD, second viewport, crowd etc.

First I stripped out or disabled any existing parallelisation in Godot, as I was going to do this "Macrame way". To my great surprise, the single-threaded version turned out to be at least as fast as the default Godot config, or even slightly faster. A brief investigation showed that the official Godot team position on multithreading/parallelisation is to keep scene-layer single-threaded, so their users don't need to deal with it. I kinda get this, multithreading is hard. Multithreading in game engines is extra hard. Godot's audience is mainly solo/indies or at best AA gamedev studios. They don't have resources or experience to deal with this. On the other hand, probably most games are CPU bound. Steam surveys show that an average gamer has 8+ CPU cores. This means that while the CPU is a bottleneck, most of available CPU resources are not used at all, hard capping what devs can achieve with Godot. Engine parallelisation doesn't have to affect its users.

I split the simulation into 64 shards which provided sufficient granularity for my mid-range laptop (22 logical cores, P/E cores and SMT). Each shard is a guarded object, which means that Macrame can schedule them properly avoiding conflicts and can detect undeclared access. I separated other systems and declared what data they access. Put everything into a static task graph. This was the most fruitful optimisation as it split the heavy part of frame into concurrent tasks with minimal dependencies.

The second step was rendering. Recording and submission on separate guarded objects. Submit one frame behind, so the two long bodies run side-by-side. Then rendering pipeline was further split into three nodes: scene update, cull and record. The key was to add frame latency. Technically speaking, it's two frames latency. Sounds bad until we add up numbers. Original latency was 34ms (single frame), in the optimised version it's two frames latency, but the frame is 5.5ms, so the total latency is 16.5ms, half the original.

The third step was wiring Jolt physics sim into Macrame scheduler. This didn't provide any significant gains, because the sample doesn't put enough pressure on physics. Still, this would be required for most games.

Right now frames are heavy at the beginning but have long tails. This can be improved by partially overlapping frames. Macrame doesn't provide a full solution yet, lessons learned and I'll be working on this. This alone could provide up to 2x on this scene.

I could construct a better sample/benchmark project and get much more impressive results. Maybe I will in the future. My goal wasn't improving Godot performance, but to validate how usable and viable Macrame can be for such parallelisation. And especially how well versioning separate systems would work (when a system maintains a stable read-only snapshot of its data during the frame while building the next version in the background). A technique widely used for rendering but rarely for anything else. I'm pretty satisfied with the results.

The library: [Macrame](https://github.com/macrame-ts/macrame)

The Godot fork: [Andriy06/godot, `macrame` branch](https://github.com/Andriy06/godot/tree/macrame)

> [!WARNING]
> **Disclaimer:** This was an experiment, an exercise, and the fork is not suitable to be used in production. I won't try to merge any of this into Godot, for many reasons, one of them is that this work was heavily assisted by AI, and Godot doesn't accept this kind of contribution.
