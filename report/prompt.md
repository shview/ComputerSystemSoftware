# Lab 1 提示词记录

## 说明

本文件汇总 Lab 1 中由本人发送给 AI 的任务要求、问题、观察结果和后续整理要求。终端输出型消息作为调试上下文保留其关键命令与结果；AI 的回复不计入“本人提示词”。

---

## Prompt 01：整体任务与协作要求

```text
以下是整体任务描述：
1. 实验交付物和提交方式：源代码和实验报告均需上传到自己小组的 git 仓库中，一个分支对应一次实验，并以“labx”命名（例如：lab1）。每一个分支中上传两个文件夹，分别以“code”和“report”命名。code 文件夹包含对应实验的实现后的代码，report 文件夹中包含实验报告（report.md）、提示词（prompt.md）以及测试截图文件夹（images）。仓库为 https://github.com/shview/ComputerSystemSoftware。
2. 本地使用 WSL + Ubuntu，在 E:\wsl 下，代码在 E:\wsl\OS，教程为 http://8.135.34.58/lab2026/_book/。
3. 报告使用给定模板，我是沈弋皓，学号 2511102。
4. 我之前没有接触过 OS 方面的知识，需要深入浅出讲解。

现在要进行 Lab 1 的实验。指导我而不是替我一键完成。
```

## Prompt 02：反馈环境检查结果

```text
代码目录为 /mnt/e/wsl/OS，找到 ./oslab/lab1。
关键文件包括 kern/init/entry.S、kern/init/init.c 和 Makefile。
RISC-V GCC 为 10.2.0，QEMU 为 4.1.1，RISC-V GDB 为 10.1。
/mnt/e/wsl/OS 当前不是 Git 仓库。请根据这些结果继续指导。
```

## Prompt 03：反馈首次运行成功

```text
make qemu 成功了，截图位于 E:\wsl\OS\oslab\lab1\make_qemu.png。
```

## Prompt 04：反馈入口符号和反汇编结果

```text
使用 nm、readelf 和 objdump 得到：

kern_entry   = 0x80200000
kern_init    = 0x8020000a
bootstack    = 0x80201000
bootstacktop = 0x80203000
ELF Entry point = 0x80200000

kern_entry 的真实指令为：
0x80200000: auipc sp,0x3
0x80200004: mv sp,sp
0x80200008: j 0x8020000a <kern_init>

我的判断：
1. kern_entry 确实位于 0x80200000。
2. la 被展开成 auipc 和 mv。
3. tail 没有保存返回地址。
```

## Prompt 05：提交练习 1 的个人草稿并请求最终整理

```text
我写然后你整理吧。

通过 nm、readelf、objdump 等指令查看了具体地址与汇编语言。
la sp, bootstacktop 完成的操作是设定好 sp 的初始值，目的是把 sp 设定到正确的位置。
tail kern_init 跳转到 kern_init 的位置，同时不设置跳回地址，直接完全跳转，目的是将之后的运行权交给 kern_init。

最后整体整理好。
```

## Prompt 06：反馈 GDB 复位入口

```text
GDB 连接 QEMU 后停在 0x1000。
info registers pc 显示 pc = 0x1000。
x/10i $pc 显示最初的指令为：

0x1000: auipc t0,0x0
0x1004: addi a1,t0,32
0x1008: csrr a0,mhartid
0x100c: ld t0,24(t0)
0x1010: jr t0

截图位于 E:\wsl\OS\oslab\lab1\gdb_debug.png。
```

## Prompt 07：反馈复位代码单步执行过程

```text
x/gx 0x1018 得到 0x0000000080000000。
随后使用 si 逐条执行复位指令。
当前在 0x1010，即 jr t0 之前。
```

## Prompt 08：反馈跳入 OpenSBI

```text
在执行 jr t0 前：
pc = 0x1010
t0 = 0x80000000
a0 = 0
a1 = 0x1020

执行 si 后：
pc = 0x80000000
第一条指令为 csrr a6,mhartid。
```

## Prompt 09：反馈内核入口断点

```text
在 0x80200000 可以反汇编出 kern_entry。
设置 watch *(unsigned int *)0x80200000 和 break *0x80200000 后继续运行。
写监视点没有先触发，执行断点在 kern_entry 命中：

Breakpoint 2, kern_entry () at kern/init/entry.S:7
0x80200000 <kern_entry>: auipc sp,0x3
```

## Prompt 10：反馈栈初始化

```text
命中 kern_entry 时：
pc = 0x80200000
sp = 0x8001bd80

执行 auipc 后：
pc = 0x80200004
sp = 0x80203000

执行 mv 后：
pc = 0x80200008
sp = 0x80203000

执行 j 后：
pc = 0x8020000a <kern_init>
sp = 0x80203000

截图均位于 E:\wsl\OS\oslab\lab1 下。
```

## Prompt 11：反馈 Makefile 测试目标与截图列表

```text
Makefile 中存在 qemu、debug、gdb、grade 和 clean 目标。
当前截图为：
- gdb_kernel_0x80200000.png
- gdb_reset_0x1000.png
- gdb_stack_init.png
- make_qemu.png
```

## Prompt 12：反馈 make grade 失败

```text
grade 目标先执行 clean，再执行 tools/grade.sh。
运行 make grade 得到：

sh: 0: cannot open tools/grade.sh: No such file
make: *** [Makefile:195: grade] Error 2
```

## Prompt 13：确认评分脚本确实缺失

```text
lab1/tools 中只有 function.mk 和 kernel.ld。
在整个 /mnt/e/wsl/OS/oslab 中搜索 grade.sh，没有任何结果。
```

## Prompt 14：提供 kern_init 源码

```text
kern_init 中：
- 声明 edata 和 end；
- 执行 memset(edata, 0, end - edata)；
- 使用 cprintf 输出 "(THU.CST) os is loading ..."；
- 最后进入 while (1)。
请继续解释和指导。
```

## Prompt 15：反馈段布局和链接脚本

```text
nm 显示 edata = end = 0x80203008。
readelf 显示：
.text   = 0x80200000
.rodata = 0x802004c8
.data   = 0x80201000

kernel.ld 使用 ENTRY(kern_entry)、BASE_ADDRESS = 0x80200000，
依次放置 .text、.rodata、对齐后的 .data/.sdata、.bss，
并提供 edata 与 end 符号。
```

## Prompt 16：反馈输出函数搜索结果

```text
使用 grep 找到了：
- kern/init/init.c 中的 cprintf；
- kern/libs/stdio.c 中的 cputch、vcprintf、cprintf；
- kern/driver/console.c 中的 cons_putc；
- libs/sbi.c 中的 sbi_call 和 sbi_console_putchar。
```

## Prompt 17：提供输出链关键源码

```text
提供了 kern/libs/stdio.c、kern/driver/console.c、libs/sbi.c 的相关源码。
其中 cprintf 使用 va_list，vcprintf 调用 vprintfmt，cputch 调用 cons_putc，
cons_putc 调用 sbi_console_putchar，sbi_call 准备 x17/x10/x11/x12 并执行 ecall。
请继续深入浅出解释。
```

## Prompt 18：提交个人理解

```text
1. 因为现在内核是建立一个自己的系统，无法使用其他系统的功能。
2. 使用回调便于上下层接口抽象。
3. ecall 用于提高权限以输出字符。
```

其中第三点经过后续讨论修正为：`ecall` 发起一次受控的特权级调用，由 OpenSBI 代为输出，内核不会永久获得 M 模式权限。

## Prompt 19：反馈空仓库状态

```text
已克隆 https://github.com/shview/ComputerSystemSoftware.git。
Git 提示这是空仓库；当前为 main 分支，没有提交，origin 地址正确。
```

## Prompt 20：反馈创建 lab1 分支和目录

```text
已执行 git switch --orphan lab1。
当前分支为 lab1，并建立：
- code/
- report/
- report/images/
```

## Prompt 21：反馈复制后的提交结构

```text
report/images 中有四张截图：
- gdb_kernel_0x80200000.png
- gdb_reset_0x1000.png
- gdb_stack_init.png
- make_qemu.png

code 中保留 kern、libs、tools 等源码目录，没有 bin 和 obj。
git status 显示 code/ 与 report/ 为未跟踪内容。
```

## Prompt 22：提供报告成员与日期信息

```text
这个实验本来就是个人完成，没有涉及分工。
日期写今天。

小组成员：
2510718 李东
2511102 沈弋皓
2511170 郑贺文
```

## Prompt 23：调整 AI 模型并插入实验图片

```text
AI 改为 5.6 Sol。
图片可以插入 report 吗？如果可以，请插入到合适位置。
```
