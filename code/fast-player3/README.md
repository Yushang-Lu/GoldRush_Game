# Fast Player 3

这是 fast-player2 日志复盘后的“极速档”独立提交单元。默认产物为
`code/fast-player3/player.so`，不会覆盖其他策略目录。

## 策略

- 固定 3+3、主力 u1 先执行，`vp=0`。
- 每个单位只探测脚下和十字四邻格；发现金币时优先安全回踩，否则沿各自的
  中心环相位直线移动。
- 两个单位使用不同中心环和相位，减少长期重合；第二个单位避开第一个单位的
  预计终点。
- 直线路径只采用第一个可行方向，常见分支只检查一条三格路径；金币回踩额外
  检查前、后和前二格，因此单个静态格阻挡不会把后续动作带进已知危险格。
- 不做 BFS、历史记忆、全局扫描、NPC 聚集统计或视野购买。

它以抢执行顺序为第一目标。对 3+ NPC 和 5x5 内非相邻金币不敏感，策略收益
明显低于 fast-player2，适合作为服务器速度探针或面对速度门槛型对手的候选，
不应把本地微基准直接当作胜率依据。

## 构建与验证

从仓库根目录执行：

```bash
make -C code/fast-player3
make -C code/fast-player3 verify
make -C code/fast-player3 test
make -C code/fast-player3 sanitize
make -C code/fast-player3 bench
make -C code/fast-player3 compare
make -C code/fast-player3 replay
```

Release 使用 C++17、`-O3 -DNDEBUG -flto -march=x86-64-v3`；官方参考
运行机支持该指令级。`test` 默认运行 50,000 个确定性模糊输入，`sanitize`
运行 20,000 个 ASan/UBSan 输入。`compare` 与 fast-player2 交错测量，默认
要求 P90 比值不高于 0.70。

## 当前本地结果

开发机为 Intel Xeon Platinum 8255C。固定 CPU 后，Release 版本每轮
1,000,000 次调用、共 5 轮：

- P50 0.094-0.095 us；
- P90 0.109-0.110 us；
- P99 0.141-0.152 us；
- 最大值 22.200-55.868 us，属于调度离群点；首次调用 2.168 us。

同一次交错测量中，fast-player2 P90 为 0.184 us，fast-player3 为
0.107 us，比值 0.582，即本地 P90 降低约 41.8%。这只是本机相对结果，
不是赛事服务器成绩。

对 26 局、13,000 回合旧输入的反事实回放：fast-player2/fast-player3 的
可见拾取代理为 46,921/24,022，移动步数为 50,519/50,391，可见 3+ NPC
踩踏代理为 1,369/1,205。回放不会生成新轨迹，也无法验证先手优势能否弥补
扫描简化造成的收益下降。
