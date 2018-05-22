/*
 *  linux/fs/pipe.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <signal.h>

#include <linux/sched.h>
#include <linux/mm.h>	/* for get_free_page */
#include <asm/segment.h>

//// 管道读操作函数
// 参数inode是管道对应的i节点，buf是用户数据缓冲区指针，count是读取的字节数。
int read_pipe(struct m_inode * inode, char * buf, int count)
{
	int chars, size, read = 0;
    
	while (count>0) {
		/* 0. size表示还有多少未读数据，size==0，说明管道中没有数据可供读取 */
		while (!(size=PIPE_SIZE(*inode))) {
			wake_up(&inode->i_wait); // 唤醒写进程
			if (inode->i_count != 2) /* are there any writers? */
				return read;
			sleep_on(&inode->i_wait);// 如果还有写进程，挂起本进程
		}
        /* 1. chars表示管道中还剩余的字节数 */
		chars = PAGE_SIZE-PIPE_TAIL(*inode);
		/* 2. 剩余字节数大于需读取的字节数，说明读取的内容在一页的范围内 */
		if (chars > count)
			chars = count;
		/* 3. 要读的数据大于管道剩余未读的数据，说明需要阻塞等待数据写入 */
		if (chars > size)
			chars = size;
		/* 4. 到这里chars为本次实际需要读取的字节数，总共需要读入字节数count减少
		 * 已读入字节数read增加*/
		count -= chars;
		read += chars;
        // 5. size指向当前管道尾指针处，目的是在while中作为指针拷贝到用户空间
		size = PIPE_TAIL(*inode);
		/* 6. 本次需读出chars字节，移动尾指针，若尾指针超过一页，则回滚到页首 */
		PIPE_TAIL(*inode) += chars;
		PIPE_TAIL(*inode) &= (PAGE_SIZE-1);
		/* 7. 拷贝内容到用户空间 */
		while (chars-->0)
			put_fs_byte(((char *)inode->i_size)[size++],buf++);
	}
    /* 8. 读取完毕，说明管道已有剩余空间了，唤醒写进程 */
	wake_up(&inode->i_wait);
	return read;
}

//// 管道写操作函数。
// 参数inode是管道对应的i节点，buf是数据缓冲区指针，count是将写入管道的字节数。
int write_pipe(struct m_inode * inode, char * buf, int count)
{
	int chars, size, written = 0;

	while (count>0) {
		/* 1. size表示管道中还有多少空间可供写入，等于0表示空间以写满 */
		while (!(size=(PAGE_SIZE-1)-PIPE_SIZE(*inode))) {
			wake_up(&inode->i_wait);			// 唤醒读进程
			if (inode->i_count != 2) { /* no readers */
				current->signal |= (1<<(SIGPIPE-1));
				return written?written:-1;
			}
			sleep_on(&inode->i_wait);			// 写进程挂起
		}
        // 程序执行到这里表示管道缓冲区中有可写空间size.于是我们管道头指针到缓冲区
        // 末端空间字节数chars。写管道操作是从管道头指针处开始写的。如果chars大于还
        // 需要写入的字节数count，则令其等于count。如果chars大于当前管道中空闲空间
        // 长度size，则令其等于size，然后把需要写入字节数count减去此次可写入的字节数
        // chars，并把写入字节数累驾到witten中。
		chars = PAGE_SIZE-PIPE_HEAD(*inode);
		if (chars > count)
			chars = count;
		if (chars > size)
			chars = size;
		count -= chars;
		written += chars;
        // 再令size指向管道数据头指针处，并调整当前管道数据头部指针(前移chars字节)。
        // 若头指针超过管道末端则绕回。然后从用户缓冲区复制chars个字节到管道头指针
        // 开始处。对于管道i节点，其i_size字段中是管道缓冲块指针。
		size = PIPE_HEAD(*inode);
		PIPE_HEAD(*inode) += chars;
		PIPE_HEAD(*inode) &= (PAGE_SIZE-1);
		while (chars-->0)
			((char *)inode->i_size)[size++]=get_fs_byte(buf++);
	}
    // 写操作结束，此时已经有数据可供读取，唤醒读进程。注意，这里只是唤醒，没有切换
	wake_up(&inode->i_wait);
	return written;
}

//// 创建管道系统调用。
// 在fildes所指的数组中创建一对文件句柄(描述符)。这对句柄指向一管道i节点。
// 参数：filedes - 文件句柄数组。fildes[0]用于读管道数据，fildes[1]向管道写入数据。
// 成功时返回0，出错时返回-1.
/* current->filp[fd[0]] = file_table[x]
 * current->filp[fd[1]] = file_table[y]
 * file_table[x].f_mode = 1 读
 * file_table[y].f_mode = 2 写
 * file_table[x].f_inode = file_table[y].f_inode 
 */
int sys_pipe(unsigned long * fildes)
{
	struct m_inode * inode;
	struct file * f[2];
	int fd[2];
	int i,j;

    // 1. 首先从file_table[32]中取两个空闲项，并分别设置引用计数为1。
    // 若只有1个空闲项，则释放该项(引用计数复位).若没有找到两个空闲项，则返回-1.
	j=0;
	for(i=0;j<2 && i<NR_FILE;i++)
		if (!file_table[i].f_count)
			(f[j++]=i+file_table)->f_count++;
	if (j==1)
		f[0]->f_count=0;
	if (j<2)
		return -1;
    // 2. 在filp[20]中挂接申请到的file_table项
	j=0;
	for(i=0;j<2 && i<NR_OPEN;i++)
		if (!current->filp[i]) {
			current->filp[ fd[j]=i ] = f[j];
			j++;
		}
	if (j==1)
		current->filp[fd[0]]=NULL;
	if (j<2) {
		f[0]->f_count=f[1]->f_count=0;
		return -1;
	}
    // 3. 利用函数get_pipe_inode()申请一个管道使用的i节点，并为管道分配一页内存作为
    // 缓冲区。
	if (!(inode=get_pipe_inode())) {
		current->filp[fd[0]] =
			current->filp[fd[1]] = NULL;
		f[0]->f_count = f[1]->f_count = 0;
		return -1;
	}
    // 4. 对两个文件结构进行初始化操作，让他们都指向同一个管道i节点，并把读写指针都置零。最后将文件句柄数组复制到对应的用户空间数组中，成功返回0，退出。
	f[0]->f_inode = f[1]->f_inode = inode;
	f[0]->f_pos = f[1]->f_pos = 0;
	// 5. 第1个文件结构的文件模式置为读，第2个文件结构的文件模式置为写。
	f[0]->f_mode = 1;		/* read */
	f[1]->f_mode = 2;		/* write */
	// 6. 返回文件描述符给用户
	put_fs_long(fd[0],0+fildes);
	put_fs_long(fd[1],1+fildes);
	return 0;
}
