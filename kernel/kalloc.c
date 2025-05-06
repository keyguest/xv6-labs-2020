// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
// #include "spinlock.h"


#define PA2PGREF_ID(p) (((p)-KERNBASE)/PGSIZE) // 获取物理地址的页码
#define PGREF_MAX_ENTRIES PA2PGREF_ID(PHYSTOP) // 物理页上限

int pageref[PGREF_MAX_ENTRIES];
struct spinlock pgreflock;

#define PA2PGREF(p) pageref[PA2PGREF_ID((uint64)(p))] // 直接获取对应的引用数



void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&pgreflock, "pgreflock"); // 初始化页面引用计数锁
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");



  acquire(&pgreflock); 

  if(--PA2PGREF(pa) <= 0) {
      // Fill with junk to catch dangling refs.
    memset(pa, 1, PGSIZE);

    r = (struct run*)pa;

    acquire(&kmem.lock);
    r->next = kmem.freelist;
    kmem.freelist = r;
    release(&kmem.lock);
  }
  release(&pgreflock);

}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r) {
    memset((char*)r, 5, PGSIZE); // fill with junk
    PA2PGREF(r) = 1; // 将新分配的页面引用置为1
  }
    
  return (void*)r;
}

void krefpage(void* pa) {
  acquire(&pgreflock); 
  PA2PGREF(pa)++;
  release(&pgreflock);
}

// 分配内存
void* kcopy_n_deref(void* pa) {
  acquire(&pgreflock); 
  
  // 如果当前物理页只有1个引用，就不需要多余分配
  if(PA2PGREF(pa) <= 1) {
    release(&pgreflock);
    return pa;
  }

  //否则就复制新的内存空间
  char* mem = kalloc();
  if(mem == 0) // 没有空间
  {
    release(&pgreflock);
    return 0;
  }
  memmove((void*)mem, (void*)pa, PGSIZE);

  // 复制完，引用减少
  PA2PGREF(pa)--;
  release(&pgreflock);
  return (void*) mem;
}
