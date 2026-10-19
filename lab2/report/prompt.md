# ***李东-2510718***

## 第一轮：Buddy 算法实现

[prompt]

```text
任务：在当前项目中新增 C:\Users\Tom\Desktop\操作系统实验\lab2\lab2_new\kern\mm\buddy_pmm.c，实现 Buddy System 物理页分配器，定义 const struct pmm_manager buddy_pmm_manager。
操作要求：先阅读工程现有的 pmm.h、memlayout.h、default_pmm.c 和链表接口，再直接编写实际文件。仅修改 buddy_pmm.c，不修改公共头文件、其他分配器、构建配置和实验报告，不切换 pmm.c 中的分配器，不依赖尚未创建的 buddy_pmm.h。公共接入和最终运行由其它成员负责。
输出要求：完成算法实现，简要说明管理结构、核心函数和申请／释放约定。测试将会在下一步进行。
[RELY]
使用现有 struct Page、struct pmm_manager 和内核工具接口，以当前工程的实际定义为准，不改变公共结构。初始化接收有效、可用的物理页区间；正常释放使用分配返回的首地址和原申请页数。
[GUARANTEE]
分配块连续且互不重叠，不越出可用范围、不分配保留页；空闲页统计准确。内存不足或请求过大时返回 NULL，失败不改变分配状态。不得依赖尚未实现的动态内存分配器管理自身元数据。
[SPECIFICATION]
实现初始化、可用区间登记、页面分配、页面释放及空闲页统计五个管理回调。按 2 的幂组织空闲块，申请时向上取整并按需拆分；释放时回收实际分配的整个块，只合并同阶、空闲且确为伙伴的块。支持长度非 2 的幂、起点未按大块对齐的初始区间；处理大小取整溢出，并明确零页申请的行为。预留文件内的 static void buddy_check(void)，由下一条提示词补充完整测试并接入 check 回调。
```

[response]

```text
已仅新增 [buddy_pmm.c](C:/Users/Tom/Desktop/操作系统实验/lab2/lab2_new/kern/mm/buddy_pmm.c)，定义了 `const struct pmm_manager buddy_pmm_manager`。

- **管理结构**：每阶一个静态空闲链表；块首部 `property` 保存“阶数 + 1”，`PG_property` 标记空闲首部，无动态元数据。
- **初始化**：支持非对齐、非二次幂长度及多区间登记。
- **申请**：向上取整后按需拆分；零页、取整溢出或无合适块时返回 `NULL`，失败不改变状态。
- **释放**：传入返回首地址和原申请页数，回收整个实际分配块，仅合并同阶空闲的真正伙伴。空闲计数按实际块大小维护。

五个管理回调均已实现；`buddy_check` 已留占位，`.check = NULL`，等待下一步补充并接入。已完成源码复核，未执行测试。
```

---

## 第二轮：Buddy 测试设计

[prompt]

```text
任务：为刚实现的 Buddy 分配器编写自测，将测试放入同一个 buddy_pmm.c，实现 static void buddy_check(void)，并设置 buddy_pmm_manager.check = buddy_check。
操作要求：仍只修改 C:\Users\Tom\Desktop\操作系统实验\lab2\lab2_new\kern\mm\buddy_pmm.c。根据接口约定设计断言，不照抄 Best-Fit 测试，不为迁就错误实现而降低检查要求。发现算法错误时，在本文件内修正并说明原因。
输出要求：写入实际代码，简要列出测试覆盖及每类测试能发现的问题；说明公共接入所需的接口。最终集成运行由其他成员负责，未实际运行不得认为测试通过。
[RELY]
依据当前 Buddy 实现及上一条提示词的接口约定。运行自测前，页描述符和 Buddy 分配器已经正确初始化。
[GUARANTEE]
使用真实断言检查结果；测试结束后释放所申请的内存，恢复空闲页数和必要的管理状态，不破坏仍在使用的页面。全部检查完成后才输出成功信息。
[SPECIFICATION]
覆盖单页与多页申请、非 2 的幂申请及按原页数释放、连续拆分、逐级合并、相邻但非伙伴的块不能错误合并、分配块不重叠、空间不足及失败后状态不变、释放后空间复用和页数恢复。对不规则初始区间进行隔离测试并恢复现场；覆盖实现约定的零页和超大请求行为。辅助测试函数均保留在本文件内。
```

[response]

```text
已仅修改 [buddy_pmm.c](C:/Users/Tom/Desktop/操作系统实验/lab2/lab2_new/kern/mm/buddy_pmm.c)，实现 `buddy_check()` 并接入 `.check = buddy_check`。

| 测试覆盖 | 可发现的问题 |
|---|---|
| 单页、多页及 3／5／9 页申请，按原页数释放 | 取整错误、补齐页重叠、释放不完整 |
| 连续拆分、逐级合并至 32 页 | 拆分丢块、合并遗漏、首部元数据残留 |
| 相邻但非伙伴的两页 | 错把地址相邻当作伙伴 |
| 耗尽、碎片不足、零页、超大及溢出请求 | 返回值错误、失败修改状态；逐字段比较描述符及链表快照 |
| 非对齐 13 页区间、保留洞、跨登记区间合并 | 越界、分配保留页、对齐或伙伴定位错误 |
| 释放后复用、计数与状态恢复 | 内存泄漏、空闲统计及链表损坏 |

隔离测试使用静态描述符，所有测试申请均释放；恢复真实 `pages`、`npage`、空闲链表、计数和中断状态后才输出成功信息。本次未改动分配算法。

公共接入需要声明 `extern const struct pmm_manager buddy_pmm_manager;`，确保源文件参与链接并选择该管理器；初始化及可用区间登记完成后，在启动期单 hart 独占环境调用 `check`。

**未实际运行自测。** 本机 GCC 因 DLL 重定位错误无法启动，语法检查也未完成；最终编译、集成运行由其他成员完成。
```

---

## 第三轮：SLUB 算法实现

[prompt]

```text
任务：在当前项目中新增 C:\Users\Tom\Desktop\操作系统实验\lab2\lab2_new\kern\mm\slub.c，实现简化的 SLUB 内存对象分配器，体现底层按页分配、上层按对象分配的两层结构。
操作要求：先阅读同目录的 pmm.h、memlayout.h 和 buddy_pmm.c，然后直接编写实际文件。仅创建或修改 slub.c，不修改其他模块、公共头文件、构建配置或实验报告，不依赖尚未创建的 slub.h。公共接入和最终运行由其他成员负责。
输出要求：完成 void slub_init(void)、void *slub_alloc(size_t size)、void slub_free(void *ptr)，测试接口 void slub_check(void) 留待下一条提示词补充。简要说明实现思路、管理结构和使用约定，如实说明验证情况。
[RELY]
以当前工程实际接口为准，通过现有 alloc_pages()、free_pages() 获取和归还页面，遵守 Buddy 的申请与释放约定。底层页分配器已初始化，按启动阶段单 hart 环境设计。
[GUARANTEE]
返回可实际读写、满足必要对齐且容量足够的内核地址，在用对象互不重叠。零字节、请求溢出或内存不足时返回 NULL，失败不得破坏状态或泄漏内存；释放 NULL无操作。正常释放使用本分配器返回且尚未释放的地址。
不要把Page描述符当作对象存储空间，不覆盖 Buddy 使用的页面管理信息，也不依赖尚未实现的动态内存分配功能。
[SPECIFICATION]
自行设计对象大小分类和空闲对象管理方式。小对象从已有页面中分配并支持释放后复用；没有空间时申请新页。较大对象通过底层页分配接口处理，并保存正确释放所需的信息。
释放时能够根据对象地址找到所属分配；小对象所在页面全部空闲后归还底层，大对象释放时归还整个分配。考虑管理信息占用和地址对齐，无须完整复刻 Linux SLUB。
```

[response]

```text
已仅新增 [slub.c](C:/Users/Tom/Desktop/操作系统实验/lab2/lab2_new/kern/mm/slub.c)，实现 `slub_init`、`slub_alloc`、`slub_free`。

- **小对象**：按 16～2048 字节分为 8 档，每页保存管理头和空闲对象链表；释放后可复用，整页空闲即归还底层。
- **大对象**：连同管理头计算所需页数，保存原申请页数，释放时由 Buddy 回收整个实际分配块。
- **地址与对齐**：通过 `page2pa() + va_pa_offset` 访问实际页面，检查映射范围，返回地址按 16 字节对齐，不改写 `Page` 管理信息。
- **使用约定**：页分配器及映射初始化后调用 `slub_init`，启动阶段单 hart 串行使用；对象不清零。零字节、溢出或空间不足返回 `NULL`，`slub_free(NULL)` 无操作。

`slub_check()` 已留空占位。

已完成源码复核，**未运行测试**；本机 GCC 因 DLL 重定位错误无法启动，语法检查亦未完成。
```

---

## 第四轮：SLUB 测试设计

[prompt]

```text
任务：为刚实现的 SLUB 编写自测，在 C:\Users\Tom\Desktop\操作系统实验\lab2\lab2_new\kern\mm\slub.c 中实现 void slub_check(void)。
操作要求：仅修改该文件，辅助测试函数也放在文件内。根据接口约定设计断言；发现实现错误时在本文件内修正并说明，不为迁就错误实现降低测试要求。
输出要求：写入实际测试代码，简要说明测试覆盖了什么、能够发现哪些错误，以及如何调用。集成运行由其他成员负责。
[RELY]
运行前，真实页分配器和 SLUB 已初始化，SLUB 没有未释放对象。使用真实可读写的内存进行测试，不使用只有模拟页描述符的测试区，不重置底层分配器或改动全局页面信息。
[GUARANTEE]
通过断言检查结果，所有测试申请最终都要释放，底层空闲页数应恢复到测试开始时的值。读写限定在对象申请范围内，全部检查完成后才输出成功信息。
[SPECIFICATION]
覆盖不同大小及分类边界的申请、地址对齐、对象不重叠、写入后读回、释放后复用、多个页面的扩展与回收，以及大对象分配释放。
同时检查零字节、NULL 释放、超大请求、计算溢出和内存不足等情况，确认失败不会破坏已有对象或造成泄漏。测试预期依据接口约定独立判断，不只重复实现内部的计算过程。
```

[response]

```text
已仅修改 [slub.c](C:/Users/Tom/Desktop/操作系统实验/lab2/lab2_new/kern/mm/slub.c:616)，完成 `slub_check()` 及文件内辅助函数。

| 测试覆盖 | 可发现的问题 |
|---|---|
| 分类边界、16 字节对齐、对象互斥、逐字节写入读回 | 容量不足、错位、重叠和数据损坏 |
| 页面扩展、满页释放后复用、空页回收 | 空闲槽丢失、提前回收、页面泄漏 |
| 跨页大对象申请与释放 | 连续空间不足、释放页数错误 |
| 零字节、NULL 释放、超大及溢出请求 | 返回值或失败状态处理错误 |
| 真实耗尽内存、耗尽时复用已有槽位、回收后重新申请 | 失败破坏已有对象、泄漏或无法恢复分配 |

所有测试使用真实对象，读写限于申请范围；全部释放并确认空闲页数恢复后才打印成功。未修改分配算法。

调用方式：真实 PMM 和 SLUB 初始化、无未释放对象后，在启动期单 hart 环境调用 `slub_check()`。成功用例需要至少 **9 个空闲页，并能分配两块连续 4 页**。

已完成源码复核；GCC 启动失败、WSL 访问被拒绝，**尚未编译或运行测试，不能认定测试通过**。
```

---

# ***郑贺文-2511170***

## 第一轮：Best-Fit 算法实现与验证

[prompt]

```text
在 kern/mm/best_fit_pmm.c 中完成 Best-Fit 连续物理页分配器并注册 best_fit_pmm_manager。先阅读现有框架，直接修改实际文件；已有实现满足要求时保留。仅修改该文件，保留 basic_check()、best_fit_check()、头文件及公共接口。用现有验证环境运行检查，简要回复实现与结果。

[RELY]
使用现有 struct Page、free_area_t、pmm_manager 和链表接口。初始化区间合法、连续且各页为保留页；释放区间连续、已分配且无重复释放。原测试直接保存和恢复 free_list、nr_free，兼容临时空链表。

[GUARANTEE]
提供初始化、区间登记、分配、释放、空闲页统计五个回调，check 指向原 best_fit_check。只链接空闲块首页，维护地址递增、块长、首页标志和准确计数；分配失败返回 NULL，状态不变。

[SPECIFICATION]
初始化建立空链表并清零计数；登记时清理页状态和引用计数。申请 n>0 页时选择最小足够块，同长取低地址，精确匹配可结束搜索；拆分余块并保持地址顺序，清除已分配首页标志，空闲计数减 n。释放 n>0 页时清理元数据，插入一次并合并物理相邻的前后块，计数只加 n；支持子区间释放及相邻已分配页整体释放。零页操作触发断言。统计回调无副作用。
```

[response]

```text
已核对 Best-Fit 的五个回调及管理器注册，最小足够块选择、同长取低地址、拆分、双向合并和空闲计数符合约束。basic_check()、best_fit_check() 与原框架一致。主机验证中原测试得 6/6，定向测试及 25,000 次随机操作全部通过，内存与未定义行为检查未发现异常。
```

---
# ***沈弋皓***

## 第一轮：确认分工与制定集成计划

[prompt]

```text
我是甲。

查看 lab2 分工、实验报告模板、队友提交的 buddy_pmm.c、slub.c、提示词和评审材料，确认其他人的完成情况，并一起完成分配给甲的任务。之后按照适合零基础学习者的方式讲解 Lab2，不要只给出一键完成方案。
```

[response]

```text
确认甲负责 page_init()、公共接口、代码集成、运行验证和 GDB 取证；乙负责 First-Fit 分析与 Best-Fit；丙负责 Buddy、SLUB 及总报告统筹。

检查队友文件后确定集成顺序：先完成 page_init()，再合入 Best-Fit，之后为 Buddy 和 SLUB 建立统一接口，最后分别编译、运行和评分。报告中甲负责 4.1、4.2 与 5.1—5.4，并向丙提供调试材料。
```

---

## 第二轮：实现物理内存初始化 page_init

[prompt]

```text
在当前工作目录下的 kern/mm/pmm.c 中实现 page_init() 函数，只修改其内内容而不修改其余实现方式，并且源代码中为 YOUR CODE 的地方替换为 2511102。

先阅读与之相关的文件，如 pmm、mmu、init.c、entry.S、kernel.ld 等文件。

针对其功能：初始化时检查原先的数据是否可以正确初始化，失败则内核恐慌；然后进行初始化；完成后再次进行检查，失败则内核恐慌。

由我编写提示词，你完成实现并对提示词进行指正。不要修改测试样例来绕过检查。
```

[response]

```text
完成 kern/mm/pmm.c 中的 page_init()，并将实验标记改为 2511102。实现先通过 DTB 接口取得物理内存起点与大小，检查零值、地址加法溢出、平台起点和物理内存上界；随后设置 va_pa_offset 和 npage，将 pages 数组放在内核 end 后的页对齐位置。

所有 Page 描述符先初始化为 Reserved，避免未知页面被提前分配；再计算描述符数组末端，将剩余完整物理页通过 init_memmap() 交给当前 PMM。初始化后检查对齐、保留页、空闲页计数以及物理地址和 Page 之间的往返转换，失败时调用 panic()。

提示词优点是限定了文件、函数、阅读范围和前后置检查；可改进处是应进一步明确 DTB 为内存信息来源、内核和元数据必须排除、所有地址运算需要检查溢出，并要求给出具体编译与运行验收标准。
```

---

## 第三轮：接入 Buddy 物理页分配器

[prompt]

```text
在 kern/mm 下创建 buddy_pmm.h，注册对象为 buddy_pmm_manager，注意不要破坏其他已经完成的代码功能。

修改 kern/mm/pmm.c 中的 init_pmm_manager()，将原来选择的 best_fit_pmm_manager 改为 Buddy 的注册名，然后进行编译测试。检查是否通过；通过后说明结果，没有通过则找到原因并说明，不要通过修改测试样例绕过测试。
```

[response]

```text
创建 buddy_pmm.h，使用 extern const struct pmm_manager buddy_pmm_manager 声明统一注册对象，并在 pmm.c 中接入 Buddy。构建系统原本会自动收集 kern/mm 下的 C 文件，因此无需手工增加源文件列表。

编译和 QEMU 运行成功，实际输出 memory management: buddy_pmm_manager、buddy_check() succeeded! 和 check_alloc_page() succeeded!。接口接入没有修改 Buddy 算法内部或测试样例。

提示词已说明接口名、修改位置、回归要求和禁止绕过测试。可改进处是不要直接永久替换 Best-Fit，而应从一开始要求通过编译宏选择分配器，并明确默认配置，以便基础评分和多配置回归。
```

---

## 第四轮：接入 SLUB 对象分配器

[prompt]

```text
根据丙同学完成的 slub.c 文件与对应函数功能，在 kern/mm 创建 slub.h 并接入 slub.c，同时根据之前 Buddy 的方式创建相同的接口。

分配策略初始化应在 PMM 初始化完成后调用，通过统一接口调用其检查示例。
```

[response]

```text
创建 slub.h，声明 slub_init()、slub_alloc()、slub_free() 和 slub_check()。在 kern/init/init.c 中包含该头文件，并保证 slub_init() 与 slub_check() 仅在 pmm_init() 完成后调用。

使用 ENABLE_SLUB_TEST 控制是否运行 SLUB 自检，避免默认评分配置被选做测试改变。分别在 Best-Fit 和 Buddy 底层配置上完成验证，Buddy + SLUB 输出 buddy_check() succeeded!、check_alloc_page() succeeded! 和 slub_check() succeeded!。

提示词正确指出了初始化顺序和公开接口。可改进处是明确条件编译宏、SLUB 不替换 PMM而是建立在 alloc_pages()/free_pages() 之上，以及验收必须确认自检后底层空闲页数恢复。
```

---

## 第五轮：使用双 GDB 跟踪 Sv39 页表与 QEMU 软件 TLB（多轮汇总）

[prompt]

```text
根据 Lab2 指导书继续学习页表查询和 QEMU MMU 调试。指导我使用带调试信息的 QEMU、宿主 GNU GDB 和 riscv64-unknown-elf-gdb，从实际运行中观察一次虚拟地址到物理地址的转换，以及 QEMU 软件 TLB 的填充过程。

我之前没有系统学习过操作系统，需要解释每个地址、页表项、调用函数和调试现象。遇到命令、断点或编译优化问题时，先定位真实原因，不要修改测试或伪造结果。截图保存在 Lab2 目录，供实验报告使用。
```

[response]

```text
确认现有 QEMU 4.1.1 源码构建产物包含调试信息，无需重新编译或安装；安装宿主 GDB 15.1，并使用三个终端分别运行 QEMU、宿主 GDB 和 RISC-V GDB。

调试中发现 Makefile 将 -s 与 addr=0x80200000 粘连，修正空格后参数成为 addr=0x80200000 -s -S。宿主 GDB 附加真正的 QEMU PID，而不是 make 的 PID。

实际跟踪虚拟地址 0xffffffffc02000d8：satp 为 0x8000000000080205，Sv39 根页表物理地址为 0x80205000；根级索引为 0x1ff，页表项地址为 0x80205ff8，PTE 为 0x200000cf，PPN 为 0x80000。QEMU返回物理页基址0x80200000，加页内偏移0xd8后得到物理地址0x802000d8。

进一步跟踪 get_page_addr_code → tlb_fill → riscv_cpu_tlb_fill → tlb_set_page → tlb_set_page_with_attrs。TLB映射参数为虚拟页0xffffffffc0200000、物理页0x80200000、权限7、页大小0x1000。观察到临时表项tn及TLB_NOTDIRTY标志。部分局部变量因编译优化显示<optimized out>，通过函数参数、调用栈、上层栈帧和源码赋值关系完成交叉确认。

生成的证据截图为 breakpoint12.png、pa.png、tlb_set_page.png 和 tlb.png。
```

---

## 第六轮：增加 First-Fit 配置并完成互斥验证

[prompt]

```text
在 init_pmm_manager() 中新增 USE_DEFAULT_PMM 配置。默认配置仍为 Best-Fit，但可以进行设定，不同配置互斥，同时只能使用一种。分别对不同配置进行验证。
```

[response]

```text
在 pmm.c 中增加 USE_DEFAULT_PMM 分支：定义该宏时选择 default_pmm_manager，定义 USE_BUDDY_PMM 时选择 buddy_pmm_manager，不定义选择宏时仍使用 best_fit_pmm_manager。

若同时定义 USE_DEFAULT_PMM 与 USE_BUDDY_PMM，则通过 #error 在编译期报告 mutually exclusive。切换配置前执行 make clean，防止复用使用其他宏编译出的目标文件。

实际验证结果：First-Fit 的 check_alloc_page() 成功；Best-Fit 默认运行成功且 make grade 为20/20；Buddy的 buddy_check() 与统一检查成功；Buddy + SLUB 的 slub_check() 成功；冲突配置按预期编译失败。最后恢复默认 Best-Fit 构建。

提示词已经明确功能、默认行为和互斥要求。可改进处是补充“同时定义宏时必须编译期报错”“每次切换前make clean”“不得修改分配器算法和测试样例”以及需要检查的具体输出。
```