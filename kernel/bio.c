// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NUM_BUCKETS 13 // 哈希桶的数量
#define HASH_FUNC(dev, blockno) (((dev<<27) | blockno ) % NUM_BUCKETS) // 哈希函数

struct {
  struct spinlock global_lock; // 配置一个全局的锁，锁住整个桶
  struct buf buf[NBUF]; // 保留buf，因为以前是直接采用malloc进行分配，而这里则是用结构体自身来当缓冲区。

  struct buf hash_bucket[NUM_BUCKETS]; // 哈希桶
  struct spinlock hash_lock[NUM_BUCKETS]; // 哈希桶锁
  // // Linked list of all buffers, through prev/next.
  // // Sorted by how recently the buffer was used.
  // // head.next is most recent, head.prev is least.
  // struct buf head;
} bcache;


void
binit(void)
{
  struct buf *b;
  initlock(&bcache.global_lock, "global_lock");
  for(int i = 0;i < NUM_BUCKETS;i++){
    initlock(&bcache.hash_lock[i], "hash_lock");// 初始化哈希锁
    bcache.hash_bucket[i].next = 0; // 初始化哈希桶
  }

  // Create linked list of buffers
  // bcache.head.prev = &bcache.head;
  // bcache.head.next = &bcache.head;
  // for(b = bcache.buf; b < bcache.buf+NBUF; b++){
  //   b->next = bcache.head.next;
  //   b->prev = &bcache.head;
  //   initsleeplock(&b->lock, "buffer");
  //   bcache.head.next->prev = b;
  //   bcache.head.next = b;
  // }

  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    // 初始化buf的锁
    initsleeplock(&b->lock, "buffer");
    b->last_user = 0; // 初始化LRU
    b->refcnt = 0; // 引用计数
    // 将b放入第0号哈希桶
    b->next = bcache.hash_bucket[0].next;
    bcache.hash_bucket[0].next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno) // 获取设备的buf,如果没有则加入
{
  // struct buf *b;

  // acquire(&bcache.lock);

  // // Is the block already cached?
  // for(b = bcache.head.next; b != &bcache.head; b = b->next){
  //   if(b->dev == dev && b->blockno == blockno){
  //     b->refcnt++;
  //     release(&bcache.lock);
  //     acquiresleep(&b->lock);
  //     return b;
  //   }
  // }

  // // Not cached.
  // // Recycle the least recently used (LRU) unused buffer.
  // for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
  //   if(b->refcnt == 0) {
  //     b->dev = dev;
  //     b->blockno = blockno;
  //     b->valid = 0;
  //     b->refcnt = 1;
  //     release(&bcache.lock);
  //     acquiresleep(&b->lock);
  //     return b;
  //   }
  // }
  // panic("bget: no buffers");

  struct buf *b;
  int hash_index = HASH_FUNC(dev, blockno); // 计算哈希索引
  acquire(&bcache.hash_lock[hash_index]); // 获取哈希锁
  // 如果存在对应的buf
  for(b = bcache.hash_bucket[hash_index].next; b; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.hash_lock[hash_index]); // 释放哈希锁
      acquiresleep(&b->lock); // 获取buf锁
      return b;
    }
  }
  // 如果在局部的哈希桶中没有找到对应的buf
  // 那么需要在全局的哈希桶也就是0号桶中查找，因此这里相当于原先的代码，需要配置一个全局锁。
  release(&bcache.hash_lock[hash_index]); // 释放哈希锁
  // 由于可能中间会有其他线程插入对应的桶，因此还要重新判断。
  acquire(&bcache.global_lock); // 获取全局锁
  for(b = bcache.hash_bucket[hash_index].next; b; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      acquire(&bcache.hash_lock[hash_index]); // 对引用需要加锁
      b->refcnt++;
      release(&bcache.hash_lock[hash_index]); // 释放哈希锁
      release(&bcache.global_lock); // 释放全局锁
      acquiresleep(&b->lock); // 获取buf锁
      return b;
    }
  }

  // 如果不存在对应的buf
  // 不能直接在0中查找（因为虽然引用计数为0，但是它依旧有缓存磁盘的内容，因此可以接着使用，
  // 而刚开始啥也没有，可以都放在0的地方有标志位valid可以区分是否有内容）

  uint maxtime = 0;// 用于记录最久未使用的时间
  struct buf *pre_maxb = 0; // 用于记录最久未使用的buf的前一个buf,方便删除
  int holding_bucket = -1; // 记录当前持有的桶，因为如果每次都持有再释放，之前找到得最小得LRU桶可能会被释放掉或是占有掉
  for(int i=0;i<NUM_BUCKETS;i++){

      uint newflag = 0; // 用于标志是否找到更适合得LRU桶
      struct buf *pre = &bcache.hash_bucket[i];

      acquire(&bcache.hash_lock[i]);

      for(b = bcache.hash_bucket[i].next; b; b = b->next, pre = pre->next){
        // 如果引用计数为0
        // printf("%d %d\n", maxtime, b->last_user);
        if(b->refcnt == 0 && (b->last_user == 0 || b->last_user > maxtime)){
          // printf("1111111111111111111");
          // 如果是最久未使用的buf
            maxtime = b->last_user;
            pre_maxb = pre;
            newflag = 1; // 可以更新用于桶得切换
        }
      }
      if(newflag){
        if(holding_bucket != -1) {
          // 如果之前有持有的桶
          release(&bcache.hash_lock[holding_bucket]); // 释放之前的桶
        } 
        holding_bucket = i; // 记录当前持有的桶
      }else {
        release(&bcache.hash_lock[i]); // 释放当前桶
      }
  }

  // 如果没有找到合适的buf
  if(holding_bucket == -1) {
    release(&bcache.global_lock); // 释放全局锁
    panic("bget: no buffers");
  }

  b = pre_maxb->next; // 取出最久未使用的buf


  // if(holding_bucket != hash_index) {
    // 将LRU的buf放入hash_index桶中
  // 先删除对应的桶
  pre_maxb->next = b->next; // 删除最久未使用的buf
  release(&bcache.hash_lock[holding_bucket]); // 释放持有的桶
  // 然后放入hash_index桶中
  acquire(&bcache.hash_lock[hash_index]); // 获取hash_index桶的锁
  b->next = bcache.hash_bucket[hash_index].next; // 放入hash_index桶中
  bcache.hash_bucket[hash_index].next = b; // 更新hash_index桶
  // }
  

  // 设置b的值
  b->blockno = blockno;
  b->valid = 0;
  b->refcnt = 1;
  b->dev = dev;
  // 释放相关锁
  release(&bcache.global_lock); // 释放全局锁
  release(&bcache.hash_lock[hash_index]); // 释放hash_index桶的锁
  acquiresleep(&b->lock);
  return b;
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  // 获取哈希索引
  int hash_index = HASH_FUNC(b->dev, b->blockno);

  acquire(&bcache.hash_lock[hash_index]); // 获取哈希锁
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    // b->next->prev = b->prev;
    // b->prev->next = b->next;
    // b->next = bcache.head.next;
    // b->prev = &bcache.head;
    // bcache.head.next->prev = b;
    // bcache.head.next = b;
    b->last_user = ticks; // 更新最后使用时间
  }
  
  release(&bcache.hash_lock[hash_index]); // 释放哈希锁
}

void
bpin(struct buf *b) {
  // 获取哈希索引
  int hash_index = HASH_FUNC(b->dev, b->blockno);
  acquire(&bcache.hash_lock[hash_index]);
  b->refcnt++;
  release(&bcache.hash_lock[hash_index]);
}

void
bunpin(struct buf *b) {
  // 获取哈希索引
  int hash_index = HASH_FUNC(b->dev, b->blockno);
  acquire(&bcache.hash_lock[hash_index]);
  b->refcnt--;
  release(&bcache.hash_lock[hash_index]);
}


