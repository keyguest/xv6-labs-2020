//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "memlayout.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  if(argfd(0, 0, &f) < 0 || argaddr(1, &st) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  if((n = argstr(0, path, MAXPATH)) < 0 || argint(1, &omode) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  if((argstr(0, path, MAXPATH)) < 0 ||
     argint(1, &major) < 0 ||
     argint(2, &minor) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  if(argstr(0, path, MAXPATH) < 0 || argaddr(1, &uargv) < 0){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = exec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  if(argaddr(0, &fdarray) < 0)
    return -1;
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}


uint64
sys_mmap(void)
{
  uint64 addr, length, offset;
  int flags, i;
  int fd, prot;
  struct file *f;

  if(argaddr(0, &addr) < 0 || argaddr(1, &length) < 0 || argint(2, &prot) < 0)
    return -1;
  if(argint(3, &flags) < 0 || argfd(4, &fd, &f) < 0 || argaddr(5, &offset) < 0 || length <= 0)
    return -1;

  if(((!f->writable && (prot & PROT_WRITE)) && !(flags & MAP_PRIVATE) )||
     ((!f->readable) && (prot & PROT_READ))){
    return -1; // 共享映射不支持写入
  }

  length = PGROUNDUP(length); // 向上取整到页大小
  struct proc *p = myproc(); // 获取进程
  struct vma *v = 0;
  uint64 vaend = MMAPEND; // 结束地址 随便找了个位置存放内容（找了进程页的最后一部分来存放内容）
  // if(length > MAXVA || length < 0){
  //   return -1;
  // }
  
  

  for(i = 0; i < NVMA; i ++){
    struct vma *vv = &p->vmas[i];
    if(vv->valid == 0){ // 找到一个空闲的vma
      if(v == 0) {
        v = &p->vmas[i];
        v->valid = 1;
      }
    }else if(vv->addr < vaend){ // 找到一个空闲的vma
      vaend = PGROUNDDOWN(vv->addr);// 所以每次找页都找最小的，因为地址向下增长。
    }
  }
  if(v == 0){
    panic("mmap: vma is full");
  }

  v->addr = vaend - length; // length 为页的整数倍，相当于向小的地方增长页
  v->sz = length;
  v->f = f; // 复制文件描述符
  v->prot = prot;
  v->flags = flags;
  v->offset = offset;

  filedup(v->f); // 复制文件描述符

  return v->addr; // 返回映射的地址

}

struct vma*
findvma(struct proc *p, uint64 va) {
  for(int i = 0; i < NVMA; i++){ // 遍历虚拟内存区域
    struct vma *vv = &p->vmas[i];
    if(vv->valid == 1 && vv->addr <= va && vv->addr + vv->sz > va){
      return vv;
    }
  }
  return 0;
}

uint64
sys_munmap(void)
{
  // 取消映射，如果带有标志MAP_SHARED，则需要将文件内容写回到文件中
  // 如果没有标志MAP_SHARED，则直接取消映射
  
  // printf("sys_munmap not implemented\n");
  uint64 addr, sz;  
  if(argaddr(0, &addr) < 0 || argaddr(1, &sz) < 0 || sz == 0) // 获取参数
      return -1; 
  
  struct proc *p = myproc();
  struct vma *v = findvma(p, addr);// 查找虚拟内存区域  va一定大于起始位置
  if(v == 0)
    return -1; // 没有找到虚拟内存区域

  if(addr > v->addr && addr + sz < v->addr + v->sz){
      return -1; // 不能在中间打洞
  }

  uint64 va_alined = addr; 
  // 释放一定是按页边释放
  // 1.va等于开始位置，则就是要将开头的几块释放了
  // 2.va大于开始位置，则就是要将结尾的几块释放了
  if(addr > v->addr){
    va_alined = PGROUNDUP(addr); // 向上取整到页大小
  }
  int leave = sz - (va_alined - addr); // 计算剩余的长度
  if(leave < 0)
    leave = 0; // 剩余长度小于0，则不释放
  
  vmaunmap(p->pagetable, va_alined, leave, v); // 取消映射 todo

  if(addr <= v->addr && addr+ sz > v->addr){ // 起始位置改变
    v->offset += addr + sz - v->addr; // 更新偏移量
    v->addr = addr + sz; // 更新起始位置
  }
  v->sz -= sz; // 更新大小

  if(v->sz <= 0){ // 如果大小小于等于0，则释放
    fileclose(v->f); // 关闭文件
    v->valid = 0; // 标记为无效
  }
  return 0;
}



int vmaalloc(uint64 va) { // 传入的是虚拟地址，但是缺发现没有对应的物理映射，需要分配
  struct proc *p = myproc();
  struct vma *v = findvma(p, va);// 查找虚拟内存区域
  if(v == 0)
    return 0;
  // 根据文件信息分配页
  void* pa = kalloc();// 分配物理页
  if(pa == 0)
    panic("vmaalloc: kalloc failed");
  memset(pa, 0, PGSIZE);

  begin_op();
  ilock(v->f->ip); // 锁定文件
  readi(v->f->ip, 0, (uint64)pa, v->offset + PGROUNDDOWN(va - v->addr), PGSIZE); // 读取文件内容到物理页
  iunlock(v->f->ip); // 解锁文件
  end_op();

  if(mappages(p->pagetable, va, PGSIZE, (uint64)pa, PTE_R | PTE_W | PTE_U) < 0){
    panic("vmaalloc: mappages failed");
  }
  return 1;

}

