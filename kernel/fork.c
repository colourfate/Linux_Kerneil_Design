/*
 *  linux/kernel/fork.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  'fork.c' contains the help-routines for the 'fork' system call
 * (see also system_call.s), and some misc functions ('verify_area').
 * Fork is rather simple, once you get the hang of it, but the memory
 * management can be a bitch. See 'mm/mm.c': 'copy_page_tables()'
 */
#include <errno.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>
#include <asm/system.h>

// 写页面验证。若页面不可写，则复制页面。
extern void write_verify(unsigned long address);

long last_pid=0;    // 最新进程号，其值会由get_empty_process生成。

// 进程空间区域写前验证函数
// 对于80386 CPU，在执行特权级0代码时不会理会用户空间中的页面是否是也保护的，
// 因此在执行内核代码时用户空间中数据页面来保护标志起不了作用，写时复制机制
// 也就失去了作用。verify_area()函数就用于此目的。但对于80486或后来的CPU，其
// 控制寄存器CRO中有一个写保护标志WP(位16)，内核可以通过设置该标志来禁止特权
// 级0的代码向用户空间只读页面执行写数据，否则将导致发生写保护异常。从而486
// 以上CPU可以通过设置该标志来达到本函数的目的。
// 该函数对当前进程逻辑地址从addr到addr+size这一段范围以页为单位执行写操作前
// 的检测操作。由于检测判断是以页面为单位进行操作，因此程序首先需要找出addr所
// 在页面开始地址start，然后start加上进程数据段基址，使这个start变成CPU 4G线性
// 空间中的地址。最后循环调用write_verify()对指定大小的内存空间进行写前验证。
// 若页面是只读的，则执行共享检验和复制页面操作。
void verify_area(void * addr,int size)
{
	unsigned long start;

    // 首先将起始地址start调整为其所在左边界开始位置，同时相应地调整验证区域
    // 大小。下句中的start& 0xfff 用来获得指定起始位置addr(也即start)在所在
    // 页面中的偏移值，原验证范围size加上这个偏移值即扩展成以addr所在页面起始
    // 位置开始的范围值。因此在下面也需要把验证开始位置start调整成页面边界值。
	start = (unsigned long) addr;
	size += start & 0xfff;
	start &= 0xfffff000;            // 此时start是当前进程空间中的逻辑地址。
    // 下面start加上进程数据段在线性地址空间中的起始基址，变成系统整个线性空间
    // 中的地址位置。对于linux-0.11内核，其数据段和代码在线性地址空间中的基址
    // 和限长均相同。
	start += get_base(current->ldt[2]);
	while (size>0) {
		size -= 4096;
		write_verify(start);
		start += 4096;
	}
}

// 复制内存页表
// 参数nr是新任务号：p是新任务数据结构指针。该函数为新任务在线性地址空间中
// 设置代码段和数据段基址、限长，并复制页表。由于Linux系统采用了写时复制
// (copy on write)技术，因此这里仅为新进程设置自己的页目录表项和页表项，而
// 没有实际为新进程分配物理内存页面。此时新进程与其父进程共享所有内存页面。
// 操作成功返回0，否则返回出错号。
int copy_mem(int nr,struct task_struct * p)
{
	unsigned long old_data_base,new_data_base,data_limit;
	unsigned long old_code_base,new_code_base,code_limit;

    
    /* 1. 取得父进程的代码段数据段限长 */
	code_limit=get_limit(0x0f);
	data_limit=get_limit(0x17);
	/* 2. 取得父进程的代码段、数据段基地址 */
	old_code_base = get_base(current->ldt[1]);
	old_data_base = get_base(current->ldt[2]);
	// 由于Linux-0.11内核
    // 还不支持代码和数据段分立的情况，因此这里需要检查代码段和数据段基址
    // 和限长是否都分别相同。否则内核显示出错信息，并停止运行。
	if (old_data_base != old_code_base)
		panic("We don't support separate I&D");
	if (data_limit < code_limit)
		panic("Bad data_limit");
    /* 3. 设置LDT代码段和数据段的基地址，为0x4000000，这是线性地址，解析出来就是：
     * 页目录项：16，页表项：0，页内偏移：0*/
	new_data_base = new_code_base = nr * 0x4000000;		// nr=1
	p->start_code = new_code_base;
	set_base(p->ldt[1],new_code_base);
	set_base(p->ldt[2],new_data_base);
	/* 4. 将进程0的页表复制到进程1的页表中，也就是将0x1000~0x1280的内容复制
	 * 到倒数第二个页面的首地址，这段页表管理着160个页，也就是从0x0到0xA0000
	 * 的内容 */
	if (copy_page_tables(old_data_base,new_data_base,data_limit)) {
		printk("free_page_tables: from copy_mem\n");
		free_page_tables(new_data_base,data_limit);
		return -ENOMEM;
	}
	return 0;
}

/*
 *  Ok, this is the main fork-routine. It copies the system process
 * information (task[nr]) and sets up the necessary registers. It
 * also copies the data segment in it's entirety.
 */
// 复制进程
// 该函数的参数进入系统调用中断处理过程开始，直到调用本系统调用处理过程
// 和调用本函数前时逐步压入栈的各寄存器的值。这些在system_call.s程序中
// 逐步压入栈的值(参数)包括：
// 1. CPU执行中断指令压入的用户栈地址ss和esp,标志寄存器eflags和返回地址cs和eip;
// 2. 在刚进入system_call时压入栈的段寄存器ds、es、fs和edx、ecx、ebx；
// 3. 调用sys_call_table中sys_fork函数时压入栈的返回地址(用参数none表示)；
// 4. 在调用copy_process()分配任务数组项号。
int copy_process(int nr,long ebp,long edi,long esi,long gs,long none,
		long ebx,long ecx,long edx,
		long fs,long es,long ds,
		long eip,long cs,long eflags,long esp,long ss)
{
	struct task_struct *p;
	int i;
	struct file *f;
	
    /* 1. 初始化时是获取内存的最高端的一页并清零，然后将task[1]赋值，nr为
     * 任务号，在find_empty_process中获得，这里等于1 */
	p = (struct task_struct *) get_free_page();
	if (!p)
		return -EAGAIN;
	task[nr] = p;
	/* 2. 复制当前进程的task_struct 给子进程 */
	*p = *current;	/* NOTE! this doesn't copy the supervisor stack */
	/* 3. 设置当前进程状态为：不可中断等待状态 */
	p->state = TASK_UNINTERRUPTIBLE;
	/* 4. 子进程的进程id，在find_empty_process()中得到 */
	p->pid = last_pid;
	p->father = current->pid;       // 设置父进程
	p->counter = p->priority;       // 运行时间片值
	p->signal = 0;                  // 信号位图置0
	p->alarm = 0;                   // 报警定时值(滴答数)
	p->leader = 0;		/* process leadership doesn't inherit */
	p->utime = p->stime = 0;        // 用户态时间和和心态运行时间
	p->cutime = p->cstime = 0;      // 子进程用户态和和心态运行时间
	p->start_time = jiffies;        // 进程开始运行时间(当前时间滴答数)
    // 这里只是对TSS赋值，当CPU执行切换任务时，会自动从TSS中把LDT段描述符
    // 的选择符加载到ldtr寄存器中（对于进程0是手动加载到TR和LDTR寄存器的）。
	p->tss.back_link = 0;
	/* 5. 将当前进程的内核栈指针指向当前页的末尾 */
	p->tss.esp0 = PAGE_SIZE + (long) p;
	p->tss.ss0 = 0x10;                      // 内核态栈的段选择符(与内核数据段相同)
	/* 6. eip在fork函数中调用 int 0x80 时压入栈中，即返回时返回int 0x80的下一行 */
	p->tss.eip = eip;                       
	p->tss.eflags = eflags;                 // 与eip同一时间压入栈
	p->tss.eax = 0;                         // fork返回时就是返回eax
	p->tss.ecx = ecx;
	p->tss.edx = edx;
	p->tss.ebx = ebx;
	p->tss.esp = esp;
	p->tss.ebp = ebp;
	p->tss.esi = esi;
	p->tss.edi = edi;
	p->tss.es = es & 0xffff;                // 段寄存器仅16位有效
	p->tss.cs = cs & 0xffff;
	p->tss.ss = ss & 0xffff;
	p->tss.ds = ds & 0xffff;
	p->tss.fs = fs & 0xffff;
	p->tss.gs = gs & 0xffff;
	/* 7. 记录当前进程的LDT在GDT中的偏移量，即段选择符，nr=1，_LDT(1)=56=0x38=0b00111000
	 * 特权级3，GDT，第7项 */
	p->tss.ldt = _LDT(nr);                  
	p->tss.trace_bitmap = 0x80000000;       // 高16位有效
    // 如果当前任务使用了协处理器，就保存其上下文。汇编指令clts用于清除控制寄存器CRO中
    // 的任务已交换(TS)标志。每当发生任务切换，CPU都会设置该标志。该标志用于管理数学协
    // 处理器：如果该标志置位，那么每个ESC指令都会被捕获(异常7)。如果协处理器存在标志MP
    // 也同时置位的话，那么WAIT指令也会捕获。因此，如果任务切换发生在一个ESC指令开始执行
    // 之后，则协处理器中的内容就可能需要在执行新的ESC指令之前保存起来。捕获处理句柄会
    // 保存协处理器的内容并复位TS标志。指令fnsave用于把协处理器的所有状态保存到目的操作数
    // 指定的内存区域中。
	if (last_task_used_math == current)
		__asm__("clts ; fnsave %0"::"m" (p->tss.i387));
    /* 8. 设置子进程的代码段、数据段，创建、复制子进程的第一个页表 */
	if (copy_mem(nr,p)) {
		task[nr] = NULL;
		free_page((long) p);
		return -EAGAIN;
	}
    /* 9. 如果父进程中打开了某个文件，则该文件的引用计数加一，
     * 表示有两个进程都打开了这个文件（共享） */
	for (i=0; i<NR_OPEN;i++)
		if ((f=p->filp[i]))
			f->f_count++;
	if (current->pwd)
		current->pwd->i_count++;
	if (current->root)
		current->root->i_count++;
	if (current->executable)
		current->executable->i_count++;
    /* 10. 将进程1的TSS和LDT挂接到GDT中，这里进程1是放到gdt[6]和gdt[7] */
	set_tss_desc(gdt+(nr<<1)+FIRST_TSS_ENTRY,&(p->tss));
	set_ldt_desc(gdt+(nr<<1)+FIRST_LDT_ENTRY,&(p->ldt));
	/* 11. 将进程1改为就绪态 */
	p->state = TASK_RUNNING;	/* do this last, just in case */
	return last_pid;
}

// 为新进程取得不重复的进程号last_pid.函数返回在任务数组中的任务号(数组项)。
int find_empty_process(void)
{
	int i;

    /* 1. 从1开始查找没有使用的pid值，放在last_pid中 */
	repeat:
		if ((++last_pid)<0) last_pid=1;
		for(i=0 ; i<NR_TASKS ; i++)
			if (task[i] && task[i]->pid == last_pid) goto repeat;
	/* 2. 找到task[]中的空闲进程，也就是为NULL的进程 */
	for(i=1 ; i<NR_TASKS ; i++)         // 任务0项被排除在外
		if (!task[i])
			return i;
	return -EAGAIN;
}
