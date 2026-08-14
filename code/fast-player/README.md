# Fast GoldRush strategy

这个目录是极速策略的独立构建单元，不会覆盖上一级目录中的
`code/player.cpp` 或 `code/player.so`。

## 构建与检查

从仓库根目录执行：

```bash
make -C code/fast-player
make -C code/fast-player verify
```

默认生成可提交的 `code/fast-player/player.so`。Release 构建使用 C++17、
`-O3 -DNDEBUG -fPIC -flto`、隐藏符号以及固定的 `-march=x86-64-v3`，不依赖
本机 `-march=native`。

## 本地基准

```bash
make -C code/fast-player bench
make -C code/fast-player compare
```

`bench` 默认执行三轮、每轮 50,000 次调用。`compare` 会在本目录以相同参数
把 `../player.cpp` 编译为临时基线，再执行三轮、每轮 100,000 次调用，并要求
极速策略与基线交叉测量后的 P90 比值不超过 0.50。样本数和门槛可覆盖，例如：

```bash
make -C code/fast-player compare \
  COMPARE_SAMPLES=200000 COMPARE_RUNS=5 MAX_RATIO=0.50
```

这些结果只能称为本地实测，最终延迟以赛事服务器为准。

## 策略摘要

- u0 向中心 `(8,8)` 迁移后守点；没有局部金币时，每三轮做一次两步往返。
- u1 扫描当前完整 5x5 视野，并围绕中心 3x3 小步移动。
- 不购买视野，不使用 BFS、历史地图或堆分配。
- 避免进入迷雾、已知墙/炸弹、可见敌人、3 个以上 NPC 的格子以及另一己方
  角色执行后的最终占位格。

清理本目录构建产物：

```bash
make -C code/fast-player clean
```
