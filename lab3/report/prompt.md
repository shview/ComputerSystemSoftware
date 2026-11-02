# ***李东***

## 第一轮 Trap上下文回复与入口调用

[prompt]

```text
现在要进行lab3的实验，我负责模块一任务，即Trap 上下文恢复与入口调用。

请先阅读工作区内 Lab3 手册的模块一要求，结合已有 SAVE_ALL、struct trapframe 和 trap() 理解调用流程，然后补齐以下文件中的两处 function1：
D:\PERSONAL_FILE\课程学习\大二上\操作系统实验\lab3\code\kern\trap\trapentry.S

我的核心思路是：以已有的保存现场方式为依据，按 RISCV 调用约定将当前 Trap frame 交给 trap()；处理结束后，从栈帧恢复寄存器和返回所需状态。重点注意栈指针的特殊处理，以及使用 C 处理后的返回地址，让已有的 sret 正确返回。

只完成这两处，保留 SAVE_ALL、Trap frame 布局和既有接口。

完成后做可独立进行的汇编检查。如果完整运行受其他模块未完成影响，说明原因即可。最后简要解释实现思路、实际验证结果和仍需联调的内容。
```

[response]

```text
已补齐 [trapentry.S](D:/PERSONAL_FILE/课程学习/大二上/操作系统实验/lab3/code/kern/trap/trapentry.S:59) 的两处 `function1`，保留 `SAVE_ALL`、Trap frame 布局及既有接口。

- **入口调用**：用 `mv a0, sp` 传入当前 Trap frame，再执行 `jal trap`；返回后进入 `__trapret`。
- **上下文恢复**：从栈帧恢复 `sstatus` 和 C 处理后的 `epc`，再恢复通用寄存器，最后从第 2 槽恢复 `sp`，由原有 `sret` 返回。
- **实际验证**：使用已安装的 RISC-V GCC 10.2.0，按项目编译参数（含 `-Werror`）独立汇编通过，生成 ELF64 RISC-V 目标文件；反汇编确认调用、CSR 恢复及最后恢复 `sp` 的顺序正确。
- **仍需联调**：模块二的中断、异常处理函数及 Lab2 内存管理依赖尚未补齐，因此未进行完整运行。补齐后需验证断点、非法指令的 EPC 更新与返回，以及连续时钟中断中的现场恢复。
```

# ***郑贺文***

## 第一轮：中断处理实现与验证

[prompt]

```text
任务：在 kern/trap/trap.c 的 lab3 标记处实现 interrupt_handler()，标记为 //lab3 function2 2511170。
操作要求：先阅读 trap.c、trap.h、clock.c、riscv.h 和 sbi.h，再直接修改实际文件。保留已有函数、接口和测试，补充必要头文件、宏及 exception_handler() 声明。
输出要求：完成中断处理，说明计时和关机条件，验证周期输出、关机边界、未知原因和上下文保持，并运行框架持续时钟用例。
[RELY]
tf 为有效中断现场；单核运行，开启中断前已完成时钟初始化，ticks 从 0 开始。使用工程现有的中断常量、计时、打印和关机接口。
[GUARANTEE]
保持现场和返回地址不变；每次管理者时钟中断重新设定下一次事件，ticks 恰好加一；其他中断不改变时钟统计。未知中断输出原因编号和完整现场。
[SPECIFICATION]
清除 cause 最高位后分类。每累计 TICK_NUM=100 次时钟中断调用 print_ticks()，用静态变量累计输出次数，默认 SHUTDOWN_AFTER=10，达到上限后调用 sbi_shutdown()。区分工程已定义的其他中断并提示。
```

[response]

```text
已在 kern/trap/trap.c 中实现 interrupt_handler()，完成中断分类、持续时钟事件设置、计数、周期输出和关机。

每次管理者时钟中断设置下一次事件，ticks 加一；每 100 次输出一行，累计输出 10 行后关机。其他中断输出类型提示，未知中断输出原因编号和完整现场，处理过程中保持上下文不变。

函数边界测试通过：第 999 次中断不关机，第 1000 次完成第 10 次输出后关机；其他中断分类和未知原因处理符合约定。框架持续时钟用例在 ticks=200 时通过，得分 40/40；普通启动与测试内核均在输出 10 行后自动退出，退出状态为 0。
```

# ***沈弋皓***

## 第一轮：Lab2依赖补齐与Lab3适配

[prompt]

```text
接下来进行Lab3，我负责异常处理、Lab2依赖补齐、总代码整合和最终测试。

Lab2原代码位于 /mnt/e/wsl/OS/oslab/lab2，Lab3初始代码位于 /mnt/e/wsl/OS/oslab/lab3，继续使用WSL环境。

请先对照两版代码，补齐Lab3中标有lab2注释的位置。以我在Lab2完成的page_init()和Best-fit实现为基础，只移植Lab3实际需要的部分，不要直接用Lab2整个pmm.c覆盖Lab3。重点保留Lab3在alloc_pages()、free_pages()和nr_free_pages()外增加的中断保存与恢复，因为启用时钟中断后，修改物理页空闲链表的过程不能被异步中断破坏。

如果移植后出现缺少宏或其他依赖，先比较两版头文件，确认语义后补齐，不要删除范围检查，也不要修改测试来绕过错误。完成后执行make clean和make，并说明实际遇到的问题和处理结果。
```

[response]

```text
已将Lab2中的page_init()适配到Lab3，保留了DTB内存探测、pages数组初始化、空闲页区间注册和初始化结果自检，同时保留Lab3已有的local_intr_save()/local_intr_restore()临界区。

编译过程中继续发现best_fit_pmm.c中的free_list和nr_free未定义，原因是该文件也保留了Lab2待补充位置，因此补入了Lab2的Best-fit空闲链表、分配、释放、相邻块合并和管理器接口。page_init()还依赖Lab3缺少的PHYSICAL_MEMORY_END，核对QEMU 128 MiB内存布局后补入0x88000000，没有删除原有边界检查。

完成上述适配后，物理内存相关代码成功通过编译。Lab3新增的中断临界区和atomic位操作实现均得到保留。
```

## 第二轮：异常处理实现与EPC验证

[prompt]

```text
现在实现我负责的exception_handler(struct trapframe *tf)，位置在kern/trap/trap.c，标记为//lab3 function2 2511102。

请先阅读trap.c、trap.h、riscv.h、tools/test_lab3.c和测试汇编，处理非法指令、断点和其他未知异常。非法指令和断点都要输出发生地址，并正确修改tf->epc，使sret后不会重新执行同一条异常指令；未知异常输出完整Trapframe。

不要直接假设所有RISC-V指令都是4字节。使用当前实验实际采用的交叉工具链编译并反汇编测试汇编，确认评分用例中ebreak的实际长度后再决定EPC偏移。输出格式需要满足评分脚本检查，不修改trap_dispatch()、函数签名和测试用例。完成后先进行编译检查，并解释tf->epc的修改如何通过RESTORE_ALL写回sepc。
```

[response]

```text
已在trap.c中实现exception_handler()。非法指令分支输出“Illegal instruction at”并将tf->epc增加4；断点分支输出“Breakpoint at”并将tf->epc增加2；其他异常输出原因并调用print_trapframe(tf)。

非法指令测试使用.word 0x0000007b，明确占4字节。使用实验的RISC-V工具链反汇编test_lab3_context.S后，评分用例中的ebreak显示为编码0x9002的压缩指令，地址从0x6a到0x6c，因此实际长度为2字节，断点EPC增加2有反汇编结果作为依据。

tf->epc位于Trapframe偏移33。异常处理修改该内存字段后，RESTORE_ALL会从同一位置加载并写回sepc，随后sret从更新后的地址继续执行。
```

## 第三轮：三人代码合并与完整验证

[prompt]

```text
现在进行Lab3最终整合。乙的interrupt_handler()已经完成，甲提交了trapentry.S。请先审查甲的RESTORE_ALL和__alltraps是否与SAVE_ALL及struct trapframe布局一致，重点检查sstatus、sepc、通用寄存器和原始sp的恢复顺序，以及是否按照RISC-V调用约定把Trapframe传给trap()。

确认无明显问题后，将甲的trapentry.S合入当前Lab3工程，与乙的中断处理、我的异常处理和Lab2依赖组成最终版本。检查kern目录中不能残留YOUR CODE和未完成的lab2标记。

依次执行make clean、make、make grade和普通make qemu。不要修改评分脚本或注入测试。说明三项评分分别验证了什么，并确认普通运行能够完成内存管理自检、持续输出10行100 ticks并通过SBI正常关机。
```

[response]

```text
已完成三人代码合并。甲的__alltraps使用mv a0, sp传递Trapframe并调用trap()；RESTORE_ALL先恢复sstatus和C处理后的sepc，再恢复除sp外的通用寄存器，最后恢复原始sp，顺序与SAVE_ALL一致。

最终版本普通构建成功，kern目录不存在未完成标记。框架测试结果为：Trap入口、寄存器恢复和断点EPC 35/35；非法指令EPC更新和返回25/25；持续Supervisor时钟中断40/40；总分100/100。

普通QEMU运行成功识别128 MiB物理内存，建立32768个页面描述符，注册31928个空闲物理页并通过check_alloc_page()自检。时钟初始化后连续输出10行100 ticks，随后通过sbi_shutdown()正常退出并返回Shell。
```