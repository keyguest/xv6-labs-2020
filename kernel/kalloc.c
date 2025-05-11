// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

// struct {
//   struct spinlock lock;
//   struct run *freelist;
// } kmem;
char* kmem_lock_names[] = {
  "kmem_cpu0",
  "kmem_cpu1",
  "kmem_cpu2",
  "kmem_cpu3",
  "kmem_cpu4",
  "kmem_cpu5",
  "kmem_cpu6",
  "kmem_cpu7",
};


struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU]; //为每个CPU分配空闲链表


void
kinit()
{
  for(int i=0;i<NCPU;i++){
    initlock(&kmem[i].lock, kmem_lock_names[i]);
  }
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

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  push_off();
  int id = cpuid();
  acquire(&kmem[id].lock);
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  release(&kmem[id].lock);

  pop_off(); // 防止中断使得获取的CPU ID不一致（中断切换进程，然后另一个cpu调用了这个进程）
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  int id;
  push_off();
  id = cpuid();
  if(kmem[id].freelist){
    acquire(&kmem[id].lock);
    r = kmem[id].freelist;
    if(r)
      kmem[id].freelist = r->next;
    release(&kmem[id].lock);
  } else {
    
    for(int i=0;i<NCPU;i++){
      if(i==id)
        continue;
      if(id>i) {
        acquire(&kmem[i].lock);
        acquire(&kmem[id].lock);
      } else {
        acquire(&kmem[id].lock);
        acquire(&kmem[i].lock);
      } // 处理死锁问题，通过id的大小来决定锁的顺序
      r = kmem[i].freelist;
      if(r){
        kmem[i].freelist = r->next;
        release(&kmem[i].lock);
        release(&kmem[id].lock);
        break;
      }
      release(&kmem[i].lock);
      release(&kmem[id].lock);
    }
  }
  pop_off();

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
